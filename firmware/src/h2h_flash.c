/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "gpio.h"
#include "h2h_flash.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/spi.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(h2h_flash);

#define SPI_CMD_WRSR 0x01
#define SPI_CMD_PAGE_PROG 0x02
#define SPI_CMD_READ 0x03
#define SPI_CMD_RDSR 0x05
#define SPI_CMD_WREN 0x06
#define SPI_CMD_SECTOR_ERASE 0x20 /* 4 KB */
#define SPI_CMD_JEDEC_ID 0x9F
#define SPI_CMD_BLOCK_ERASE 0xD8 /* 64 KB */
#define SPI_SR_WIP BIT(0)
/*
 * Block protection in SR1: SRWD (bit 7) and BP0..BP3/TB (bits 2..5), the same
 * positions on Winbond, GigaDevice, Macronix, Puya and XMC. Bit 6 is left
 * alone: it is QE on Macronix (clearing it could break a quad-read boot) and
 * SEC on Winbond/GD, which does nothing while the BP bits are 0. CMP in SR2
 * (Winbond/GD) is not touched; it is 0 from the factory, and if it were set
 * the read-back CRC check would catch the unprogrammed flash.
 */
#define SPI_SR_PROTECT_MASK 0xBC

#define H2H_PAGE_SIZE 256
#define H2H_SECTOR_SIZE 4096
#define H2H_BLOCK_SIZE 65536
/* 3-byte addressing only (READ 0x03 / PP 0x02 / SE 0x20). */
#define H2H_MAX_FLASH_SIZE (16U * 1024U * 1024U)

/*
 * Datasheet worst cases for common 1-16 MB SPI NOR parts, with margin (64 KB
 * block erase is specified up to 3 s on Puya/XMC parts).
 */
#define H2H_TIMEOUT_PAGE_MS 10
#define H2H_TIMEOUT_SECTOR_MS 1000
#define H2H_TIMEOUT_BLOCK_MS 5000
#define H2H_TIMEOUT_WRSR_MS 100

/* Who currently owns the flash bus; changed only under h2h_spi_lock. */
static enum h2h_owner bus_owner = H2H_OWNER_NONE;

/* Reject ranges outside the 3-byte address space (also catches offset < 0). */
static bool range_ok(off_t offset, size_t len)
{
	return offset >= 0 && len <= H2H_MAX_FLASH_SIZE &&
	       (size_t)offset <= H2H_MAX_FLASH_SIZE - len;
}

#if DT_NODE_EXISTS(DT_NODELABEL(spi0))
static const struct device *const h2h_spi_dev =
	DEVICE_DT_GET(DT_NODELABEL(spi0));
static const struct spi_config h2h_spi_cfg = {
	.frequency = CONFIG_MAUI_H2H_SPI_FREQUENCY,
	/* Controller mode and peripheral index 0 are the Zephyr defaults (both
	 * encode as 0), so they are deliberately left out here.
	 */
	.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
};

/*
 * The IT8xxx2 SSPI command queue DMAs from/to SRAM, so transmit data that
 * might live in flash (const tables) is staged here. Received data goes
 * straight into the caller's buffer, which is always RAM.
 */
static uint8_t sram_tx_buf[4 + H2H_PAGE_SIZE] __aligned(4);
static uint8_t crc_buf[H2H_SECTOR_SIZE] __aligned(4);
#endif /* DT_NODE_EXISTS(DT_NODELABEL(spi0)) */

/* Serializes SPI transfers and bus ownership changes (recursive). */
static K_MUTEX_DEFINE(h2h_spi_lock);

#if DT_NODE_EXISTS(DT_NODELABEL(spi0))

/*
 * The SSPI controller sends tx first and then clocks in rx (half duplex), so
 * tx = command + address and rx = the data that follows.
 */
static int h2h_spi_xfer(const uint8_t *tx, size_t tx_len, uint8_t *rx,
			size_t rx_len)
{
	if (!device_is_ready(h2h_spi_dev)) {
		return -ENODEV;
	}
	if (tx_len > sizeof(sram_tx_buf)) {
		return -EINVAL;
	}

	if (tx != sram_tx_buf) {
		memcpy(sram_tx_buf, tx, tx_len);
	}

	struct spi_buf tx_buf = { .buf = sram_tx_buf, .len = tx_len };
	struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
	struct spi_buf rx_buf = { .buf = rx, .len = rx_len };
	struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };

	return spi_transceive(h2h_spi_dev, &h2h_spi_cfg, &tx_set,
			      (rx && rx_len > 0) ? &rx_set : NULL);
}

static int h2h_read_sr(uint8_t *status)
{
	uint8_t cmd = SPI_CMD_RDSR;

	return h2h_spi_xfer(&cmd, 1, status, 1);
}

/*
 * Poll WIP. Page programs finish in well under a millisecond, so they are
 * polled back to back (each RDSR is itself a full SPI transaction); erases
 * take tens of milliseconds and sleep between polls.
 */
static int h2h_wait_ready(uint32_t timeout_ms, bool sleep)
{
	int64_t deadline = k_uptime_get() + timeout_ms;
	uint8_t status;

	do {
		int rc = h2h_read_sr(&status);

		if (rc != 0) {
			return rc;
		}
		if (status == 0xFF) {
			/* MISO floating/unconnected */
			return -ENODEV;
		}
		if ((status & SPI_SR_WIP) == 0) {
			return 0;
		}
		if (sleep) {
			k_msleep(1);
		}
	} while (k_uptime_get() < deadline);

	return -ETIMEDOUT;
}

static int h2h_write_enable(void)
{
	uint8_t cmd = SPI_CMD_WREN;

	return h2h_spi_xfer(&cmd, 1, NULL, 0);
}

static void put_addr(uint8_t *buf, uint32_t addr)
{
	buf[1] = (uint8_t)(addr >> 16);
	buf[2] = (uint8_t)(addr >> 8);
	buf[3] = (uint8_t)addr;
}

static int h2h_erase_one(uint8_t opcode, uint32_t addr, uint32_t timeout_ms)
{
	uint8_t cmd[4] = { opcode };
	int rc;

	put_addr(cmd, addr);
	rc = h2h_write_enable();
	if (rc == 0) {
		rc = h2h_spi_xfer(cmd, sizeof(cmd), NULL, 0);
	}
	if (rc == 0) {
		rc = h2h_wait_ready(timeout_ms, true);
	}
	return rc;
}

static bool is_erased(const uint8_t *data, size_t len)
{
	for (size_t i = 0; i < len; i++) {
		if (data[i] != 0xFF) {
			return false;
		}
	}
	return true;
}
#endif /* DT_NODE_EXISTS(DT_NODELABEL(spi0)) */

static int bus_on(void)
{
#if defined(CONFIG_BOARD_MAUI_V1P2_IT82002)
	/* 1. Hold Prolific PL27A1 in reset (H2H_RESET_L = active/low) */
	(void)gpio_set(GPIO_H2H_RESET_L, 1);
	k_msleep(10);

	/* 2. Deassert SPI Flash WP# and HOLD# (logical 0 = physical HIGH) */
	(void)gpio_set(GPIO_SPI_EC_WP_MEM_R_L, 0);
	(void)gpio_set(GPIO_SPI_EC_HOLD_MEM_R_L, 0);

	/* 3. Route SPI mux to EC (EC_MEM_SEL = 1 [EC], EC_MEM_OE_L = 1
	 * [active/low]) */
	(void)gpio_set(GPIO_EC_MEM_SEL, 1);
	(void)gpio_set(GPIO_EC_MEM_OE_L, 1);
	k_msleep(5);

	LOG_INF("H2H SPI flash bus acquired by EC");
	return 0;
#else
	return -ENOTSUP;
#endif
}

static int bus_off(void)
{
#if defined(CONFIG_BOARD_MAUI_V1P2_IT82002)
	/*
	 * 1. Switch the mux back to the PL27A1. The mux output buffer
	 * (EC_MEM_OE_L, active low) must stay ENABLED: it sits between the
	 * PL27A1 and its own flash too, so disabling it here would leave the
	 * bridge unable to boot.
	 */
	(void)gpio_set(GPIO_EC_MEM_SEL, 0);
	(void)gpio_set(GPIO_EC_MEM_OE_L, 1);
	k_msleep(5);

	/* 2. Release Prolific PL27A1 reset so it boots from SPI flash */
	(void)gpio_set(GPIO_H2H_RESET_L, 0);

	LOG_INF("H2H SPI flash bus released to PL27A1");
	return 0;
#else
	return -ENOTSUP;
#endif
}

int h2h_bus_claim(enum h2h_owner who)
{
	int rc = 0;

	k_mutex_lock(&h2h_spi_lock, K_FOREVER);
	if (bus_owner == H2H_OWNER_NONE) {
		rc = bus_on();
		if (rc == 0) {
			bus_owner = who;
		}
	} else if (bus_owner != who) {
		rc = -EBUSY;
	}
	k_mutex_unlock(&h2h_spi_lock);
	return rc;
}

int h2h_bus_release(enum h2h_owner who)
{
	int rc = 0;

	k_mutex_lock(&h2h_spi_lock, K_FOREVER);
	if (bus_owner == who) {
		rc = bus_off();
		bus_owner = H2H_OWNER_NONE;
	} else if (bus_owner != H2H_OWNER_NONE) {
		rc = -EBUSY;
	}
	k_mutex_unlock(&h2h_spi_lock);
	return rc;
}

enum h2h_owner h2h_bus_owner(void)
{
	return bus_owner;
}

int h2h_flash_read_id(uint8_t id[3])
{
#if DT_NODE_EXISTS(DT_NODELABEL(spi0))
	uint8_t cmd = SPI_CMD_JEDEC_ID;
	int rc;

	k_mutex_lock(&h2h_spi_lock, K_FOREVER);
	rc = h2h_spi_xfer(&cmd, 1, id, 3);
	k_mutex_unlock(&h2h_spi_lock);
	return rc;
#else
	ARG_UNUSED(id);
	return -ENOTSUP;
#endif
}

int h2h_flash_size_from_id(const uint8_t id[3], uint32_t *size)
{
	/* Nothing answering: MISO floating high or pulled low. */
	if ((id[0] == 0xFF && id[1] == 0xFF && id[2] == 0xFF) ||
	    (id[0] == 0x00 && id[1] == 0x00 && id[2] == 0x00)) {
		return -ENODEV;
	}
	/* JEDEC capacity code is log2(bytes) for standard SPI NOR. */
	if (id[2] < 16 || id[2] > 24) {
		return -ENOTSUP;
	}
	*size = BIT(id[2]);
	return (*size <= H2H_MAX_FLASH_SIZE) ? 0 : -ENOTSUP;
}

int h2h_flash_unprotect(void)
{
#if DT_NODE_EXISTS(DT_NODELABEL(spi0))
	uint8_t status;
	int rc;

	k_mutex_lock(&h2h_spi_lock, K_FOREVER);
	rc = h2h_read_sr(&status);
	if (rc == 0 && (status & SPI_SR_PROTECT_MASK) != 0) {
		uint8_t cmd[2] = { SPI_CMD_WRSR,
				   status & ~SPI_SR_PROTECT_MASK };

		LOG_INF("Clearing H2H flash block protection (SR=0x%02x)",
			status);
		rc = h2h_write_enable();
		if (rc == 0) {
			rc = h2h_spi_xfer(cmd, sizeof(cmd), NULL, 0);
		}
		if (rc == 0) {
			rc = h2h_wait_ready(H2H_TIMEOUT_WRSR_MS, true);
		}
		if (rc == 0) {
			rc = h2h_read_sr(&status);
		}
		if (rc == 0 && (status & SPI_SR_PROTECT_MASK) != 0) {
			/* e.g. SRWD set with WP# held low in hardware. */
			LOG_ERR("H2H flash still protected (SR=0x%02x)",
				status);
			rc = -EACCES;
		}
	}
	k_mutex_unlock(&h2h_spi_lock);
	return rc;
#else
	return -ENOTSUP;
#endif
}

int h2h_flash_read(off_t offset, void *data, size_t len)
{
#if DT_NODE_EXISTS(DT_NODELABEL(spi0))
	uint8_t *out = (uint8_t *)data;
	int rc = 0;

	if (!range_ok(offset, len)) {
		return -EINVAL;
	}

	k_mutex_lock(&h2h_spi_lock, K_FOREVER);
	while (len > 0 && rc == 0) {
		/* One command-queue transfer moves up to 64 KB; 4 KB keeps
		 * the lock hold time short.
		 */
		size_t chunk = MIN(len, H2H_SECTOR_SIZE);
		uint8_t tx[4] = { SPI_CMD_READ };

		put_addr(tx, (uint32_t)offset);
		rc = h2h_spi_xfer(tx, sizeof(tx), out, chunk);

		out += chunk;
		offset += chunk;
		len -= chunk;
	}
	k_mutex_unlock(&h2h_spi_lock);
	return rc;
#else
	return -ENOTSUP;
#endif
}

int h2h_flash_erase_cb(off_t offset, size_t size, h2h_progress_cb cb,
		       void *user_data)
{
#if DT_NODE_EXISTS(DT_NODELABEL(spi0))
	size_t done = 0;
	int rc = 0;

	if (!range_ok(offset, size) || (offset % H2H_SECTOR_SIZE) != 0 ||
	    (size % H2H_SECTOR_SIZE) != 0) {
		return -EINVAL;
	}

	k_mutex_lock(&h2h_spi_lock, K_FOREVER);
	while (done < size && rc == 0) {
		uint32_t cur = (uint32_t)offset + done;
		size_t left = size - done;

		/* 64 KB block erase is ~4x faster per byte than 4 KB sectors;
		 * use it wherever alignment allows.
		 */
		if ((cur % H2H_BLOCK_SIZE) == 0 && left >= H2H_BLOCK_SIZE) {
			rc = h2h_erase_one(SPI_CMD_BLOCK_ERASE, cur,
					   H2H_TIMEOUT_BLOCK_MS);
			done += H2H_BLOCK_SIZE;
		} else {
			rc = h2h_erase_one(SPI_CMD_SECTOR_ERASE, cur,
					   H2H_TIMEOUT_SECTOR_MS);
			done += H2H_SECTOR_SIZE;
		}

		if (rc == 0 && cb) {
			rc = cb(done, size, user_data);
		}
	}
	k_mutex_unlock(&h2h_spi_lock);
	return rc;
#else
	ARG_UNUSED(offset);
	ARG_UNUSED(size);
	ARG_UNUSED(cb);
	ARG_UNUSED(user_data);
	return -ENOTSUP;
#endif
}

int h2h_flash_erase(off_t offset, size_t size)
{
	return h2h_flash_erase_cb(offset, size, NULL, NULL);
}

int h2h_flash_write(off_t offset, const void *data, size_t len)
{
#if DT_NODE_EXISTS(DT_NODELABEL(spi0))
	const uint8_t *in = (const uint8_t *)data;
	int rc = 0;

	if (!range_ok(offset, len)) {
		return -EINVAL;
	}

	k_mutex_lock(&h2h_spi_lock, K_FOREVER);
	while (len > 0 && rc == 0) {
		size_t page_off = offset % H2H_PAGE_SIZE;
		size_t chunk = MIN(len, H2H_PAGE_SIZE - page_off);

		/* Programming 0xFF is a no-op on NOR flash; skipping those
		 * pages is free speed for padded images.
		 */
		if (!is_erased(in, chunk)) {
			/* WREN is also staged in sram_tx_buf, so issue it
			 * before filling the buffer with the page.
			 */
			rc = h2h_write_enable();
			if (rc == 0) {
				sram_tx_buf[0] = SPI_CMD_PAGE_PROG;
				put_addr(sram_tx_buf, (uint32_t)offset);
				memcpy(&sram_tx_buf[4], in, chunk);
				rc = h2h_spi_xfer(sram_tx_buf, chunk + 4, NULL,
						  0);
			}
			if (rc == 0) {
				rc = h2h_wait_ready(H2H_TIMEOUT_PAGE_MS, false);
			}
		}

		in += chunk;
		offset += chunk;
		len -= chunk;
	}
	k_mutex_unlock(&h2h_spi_lock);
	return rc;
#else
	return -ENOTSUP;
#endif
}

int h2h_flash_crc32_cb(off_t offset, size_t len, uint32_t *crc,
		       h2h_progress_cb cb, void *user_data)
{
#if DT_NODE_EXISTS(DT_NODELABEL(spi0))
	size_t done = 0;
	uint32_t c = 0;
	int rc = 0;

	if (!range_ok(offset, len)) {
		return -EINVAL;
	}

	/* crc_buf is shared: hold the (recursive) lock for the whole run. */
	k_mutex_lock(&h2h_spi_lock, K_FOREVER);
	while (done < len && rc == 0) {
		size_t chunk = MIN(len - done, sizeof(crc_buf));

		rc = h2h_flash_read(offset + done, crc_buf, chunk);
		if (rc == 0) {
			c = crc32_ieee_update(c, crc_buf, chunk);
			done += chunk;
			if (cb) {
				rc = cb(done, len, user_data);
			}
		}
	}
	k_mutex_unlock(&h2h_spi_lock);
	*crc = c;
	return rc;
#else
	ARG_UNUSED(offset);
	ARG_UNUSED(len);
	ARG_UNUSED(crc);
	ARG_UNUSED(cb);
	ARG_UNUSED(user_data);
	return -ENOTSUP;
#endif
}

/* ------------------------------------------------------------------------ */
/* Shell commands (bring-up / debugging; maui-flash uses the RPC channel)   */
/* ------------------------------------------------------------------------ */

/*
 * Take the bus for one shell command. Returns 1 if it was claimed just for
 * this command (release afterwards), 0 if the shell already held it via
 * 'h2h_flash acquire', or -EBUSY during an RPC flashing session.
 */
static int shell_bus_get(const struct shell *sh)
{
	bool held = h2h_bus_owner() == H2H_OWNER_SHELL;
	int rc = h2h_bus_claim(H2H_OWNER_SHELL);

	if (rc == -EBUSY) {
		shell_error(sh, "H2H flash is busy (RPC flashing session)");
	} else if (rc != 0) {
		shell_error(sh, "Failed to acquire H2H SPI bus (%d)", rc);
	}
	return rc < 0 ? rc : (held ? 0 : 1);
}

static void shell_bus_put(int claimed)
{
	if (claimed == 1) {
		(void)h2h_bus_release(H2H_OWNER_SHELL);
	}
}

/*
 * Parse a numeric shell argument in a fixed base. Offsets are hexadecimal
 * ("100" is 0x100, a "0x" prefix is optional); sizes and lengths are
 * decimal. A fixed base avoids strtoul(..., 0) reading "010" as octal.
 */
static int shell_parse_num(const struct shell *sh, const char *arg, int base,
			   const char *name, unsigned long *out)
{
	int err = 0;

	*out = shell_strtoul(arg, base, &err);
	if (err) {
		shell_error(sh, "Invalid %s %s '%s'",
			    base == 16 ? "hex" : "decimal", name, arg);
	}
	return err;
}

static int cmd_h2h_acquire(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int rc = h2h_bus_claim(H2H_OWNER_SHELL);
	if (rc == 0) {
		shell_print(
			sh,
			"H2H SPI bus acquired by EC (PL27A1 held in reset)");
	} else if (rc == -EBUSY) {
		shell_error(sh, "H2H flash is busy (RPC flashing session)");
	} else {
		shell_error(sh, "Failed to acquire H2H SPI bus (%d)", rc);
	}
	return rc;
}

static int cmd_h2h_release(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int rc = h2h_bus_release(H2H_OWNER_SHELL);
	if (rc == 0) {
		shell_print(sh, "H2H SPI bus released (PL27A1 running)");
	} else if (rc == -EBUSY) {
		shell_error(sh, "H2H flash is busy (RPC flashing session)");
	} else {
		shell_error(sh, "Failed to release H2H SPI bus (%d)", rc);
	}
	return rc;
}

static int cmd_h2h_read(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 3) {
		shell_error(sh, "Usage: h2h_flash read <offset_hex> <len>");
		return -EINVAL;
	}

	unsigned long offset, len;

	if (shell_parse_num(sh, argv[1], 16, "offset", &offset) ||
	    shell_parse_num(sh, argv[2], 10, "length", &len)) {
		return -EINVAL;
	}
	if (len > 256) {
		len = 256;
	}

	uint8_t buf[256];
	int claimed = shell_bus_get(sh);

	if (claimed < 0) {
		return claimed;
	}

	int rc = h2h_flash_read(offset, buf, len);
	if (rc == 0) {
		shell_hexdump(sh, buf, len);
	} else {
		shell_error(sh, "Flash read failed (%d)", rc);
	}

	shell_bus_put(claimed);
	return rc;
}

static int cmd_h2h_erase(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 3) {
		shell_error(sh,
			    "Usage: h2h_flash erase <offset_hex> <size>");
		return -EINVAL;
	}

	unsigned long offset, size;

	if (shell_parse_num(sh, argv[1], 16, "offset", &offset) ||
	    shell_parse_num(sh, argv[2], 10, "size", &size)) {
		return -EINVAL;
	}

	int claimed = shell_bus_get(sh);

	if (claimed < 0) {
		return claimed;
	}

	int rc = h2h_flash_erase(offset, size);
	if (rc == 0) {
		shell_print(sh, "Erased %lu bytes at offset 0x%lx", size,
			    offset);
	} else {
		shell_error(sh, "Flash erase failed (%d)", rc);
	}

	shell_bus_put(claimed);
	return rc;
}

static int cmd_h2h_id(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	uint8_t rx[3] = { 0 };
	uint32_t size = 0;
	int claimed = shell_bus_get(sh);

	if (claimed < 0) {
		return claimed;
	}

	int rc = h2h_flash_read_id(rx);
	if (rc == 0) {
		shell_print(
			sh,
			"JEDEC ID: 0x%02X 0x%02X 0x%02X (Manufacturer: 0x%02X, Type: 0x%02X, Capacity: 0x%02X)",
			rx[0], rx[1], rx[2], rx[0], rx[1], rx[2]);
		if (h2h_flash_size_from_id(rx, &size) == 0) {
			shell_print(sh, "Size: %u KB", size / 1024);
		}
	} else {
		shell_error(sh, "Failed to read JEDEC ID (%d)", rc);
	}

	shell_bus_put(claimed);
	return rc;
}

static int cmd_h2h_write(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 3) {
		shell_error(sh,
			    "Usage: h2h_flash write <offset_hex> <hex_string>");
		shell_error(sh,
			    "Example: h2h_flash write 0x0 deadbeef12345678");
		return -EINVAL;
	}

	unsigned long offset;

	if (shell_parse_num(sh, argv[1], 16, "offset", &offset)) {
		return -EINVAL;
	}

	const char *hex = argv[2];

	if (hex[0] == '0' && (hex[1] == 'x' || hex[1] == 'X')) {
		hex += 2;
	}

	size_t hex_len = strlen(hex);
	if (hex_len == 0 || (hex_len % 2) != 0 || hex_len > 512) {
		shell_error(
			sh,
			"Data must be an even number of hex digits (up to 256 bytes)");
		return -EINVAL;
	}

	uint8_t buf[256];
	size_t bin_len = hex2bin(hex, hex_len, buf, sizeof(buf));
	if (bin_len == 0) {
		shell_error(sh, "Failed to parse hex string '%s'", argv[2]);
		return -EINVAL;
	}

	int claimed = shell_bus_get(sh);

	if (claimed < 0) {
		return claimed;
	}

	int rc = h2h_flash_write(offset, buf, bin_len);
	if (rc == 0) {
		shell_print(sh, "Wrote %zu bytes to offset 0x%lx", bin_len,
			    offset);
	} else {
		shell_error(sh, "Flash write failed (%d)", rc);
	}

	shell_bus_put(claimed);
	return rc;
}

static int cmd_h2h_crc(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 3) {
		shell_error(sh, "Usage: h2h_flash crc <offset_hex> <len>");
		return -EINVAL;
	}

	unsigned long offset, len;

	if (shell_parse_num(sh, argv[1], 16, "offset", &offset) ||
	    shell_parse_num(sh, argv[2], 10, "length", &len)) {
		return -EINVAL;
	}

	uint32_t crc = 0;
	int claimed = shell_bus_get(sh);

	if (claimed < 0) {
		return claimed;
	}
	int64_t start = k_uptime_get();

	int rc = h2h_flash_crc32_cb(offset, len, &crc, NULL, NULL);
	if (rc == 0) {
		shell_print(sh, "CRC32 [0x%lx, +%lu) = 0x%08x (%u ms)",
			    offset, len, crc,
			    (uint32_t)(k_uptime_get() - start));
	} else {
		shell_error(sh, "CRC failed (%d)", rc);
	}

	shell_bus_put(claimed);
	return rc;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_h2h_flash_cmds,
	SHELL_CMD(acquire, NULL, "Acquire H2H SPI bus for EC", cmd_h2h_acquire),
	SHELL_CMD(release, NULL, "Release H2H SPI bus to PL27A1",
		  cmd_h2h_release),
	SHELL_CMD(id, NULL, "Read JEDEC ID from H2H flash", cmd_h2h_id),
	SHELL_CMD(read, NULL, "Read bytes from H2H flash", cmd_h2h_read),
	SHELL_CMD(write, NULL, "Write hex bytes to H2H flash", cmd_h2h_write),
	SHELL_CMD(erase, NULL, "Erase sectors on H2H flash", cmd_h2h_erase),
	SHELL_CMD(crc, NULL, "CRC32 of an H2H flash range", cmd_h2h_crc),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(h2h_flash, &sub_h2h_flash_cmds,
		   "Prolific PL27A1 H2H SPI Flash commands", NULL);
