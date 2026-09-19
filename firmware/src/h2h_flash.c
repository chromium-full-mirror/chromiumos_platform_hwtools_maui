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

LOG_MODULE_REGISTER(h2h_flash);

#if DT_NODE_EXISTS(DT_NODELABEL(spi0))
static const struct device *const h2h_spi_dev =
	DEVICE_DT_GET(DT_NODELABEL(spi0));
static const struct spi_config h2h_spi_cfg = {
	.frequency = 12000000,
	/* Controller mode and peripheral index 0 are the Zephyr defaults (both
	 * encode as 0), so they are deliberately left out here.
	 */
	.operation = SPI_WORD_SET(8) | SPI_TRANSFER_MSB,
};

/* IT8xxx2 SSPI DMA requires buffers in SRAM */
static uint8_t sram_tx_buf[260];
static uint8_t sram_rx_buf[260];
static struct k_mutex h2h_spi_lock;
static bool lock_initialized;
#endif

#define SPI_CMD_WREN 0x06
#define SPI_CMD_RDSR 0x05
#define SPI_CMD_READ 0x03
#define SPI_CMD_PAGE_PROG 0x02
#define SPI_CMD_SECTOR_ERASE 0x20
#define SPI_CMD_JEDEC_ID 0x9F
#define SPI_SR_WIP BIT(0)

static bool bus_acquired;

#if DT_NODE_EXISTS(DT_NODELABEL(spi0))
static void ensure_lock_init(void)
{
	if (!lock_initialized) {
		k_mutex_init(&h2h_spi_lock);
		lock_initialized = true;
	}
}

static int h2h_spi_xfer(const uint8_t *tx, size_t tx_len, uint8_t *rx,
			size_t rx_len)
{
	if (!device_is_ready(h2h_spi_dev)) {
		return -ENODEV;
	}
	if (tx_len > sizeof(sram_tx_buf) || rx_len > sizeof(sram_rx_buf)) {
		return -EINVAL;
	}

	memcpy(sram_tx_buf, tx, tx_len);
	memset(sram_rx_buf, 0, rx_len);

	struct spi_buf tx_buf = { .buf = sram_tx_buf, .len = tx_len };
	struct spi_buf_set tx_set = { .buffers = &tx_buf, .count = 1 };
	struct spi_buf rx_buf = { .buf = sram_rx_buf, .len = rx_len };
	struct spi_buf_set rx_set = { .buffers = &rx_buf, .count = 1 };

	int rc = spi_transceive(h2h_spi_dev, &h2h_spi_cfg, &tx_set,
				(rx && rx_len > 0) ? &rx_set : NULL);
	if (rc == 0 && rx && rx_len > 0) {
		memcpy(rx, sram_rx_buf, rx_len);
	}
	return rc;
}

static int h2h_wait_ready(uint32_t timeout_ms)
{
	int64_t deadline = k_uptime_get() + timeout_ms;
	uint8_t cmd[2] = { SPI_CMD_RDSR, 0x00 };
	uint8_t rx[2] = { 0 };

	while (k_uptime_get() < deadline) {
		int rc = h2h_spi_xfer(cmd, sizeof(cmd), rx, sizeof(rx));
		if (rc != 0) {
			return rc;
		}
		if (rx[1] == 0xFF) {
			/* MISO floating/unconnected */
			return -ENODEV;
		}
		if ((rx[1] & SPI_SR_WIP) == 0) {
			return 0;
		}
		k_msleep(1);
	}
	return -ETIMEDOUT;
}

static int h2h_write_enable(void)
{
	uint8_t cmd = SPI_CMD_WREN;
	return h2h_spi_xfer(&cmd, 1, NULL, 0);
}
#endif

int h2h_spi_acquire(void)
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

	bus_acquired = true;
	LOG_INF("H2H SPI flash bus acquired by EC");
	return 0;
#else
	return -ENOTSUP;
#endif
}

int h2h_spi_release(void)
{
#if defined(CONFIG_BOARD_MAUI_V1P2_IT82002)
	/* 1. Disable SPI mux output buffer and switch mux back to PL27A1 */
	(void)gpio_set(GPIO_EC_MEM_OE_L, 0);
	(void)gpio_set(GPIO_EC_MEM_SEL, 0);
	k_msleep(5);

	/* 2. Release Prolific PL27A1 reset so it boots from SPI flash */
	(void)gpio_set(GPIO_H2H_RESET_L, 0);

	bus_acquired = false;
	LOG_INF("H2H SPI flash bus released to PL27A1");
	return 0;
#else
	return -ENOTSUP;
#endif
}

int h2h_flash_read(off_t offset, void *data, size_t len)
{
#if DT_NODE_EXISTS(DT_NODELABEL(spi0))
	ensure_lock_init();
	uint8_t *out = (uint8_t *)data;
	size_t remaining = len;

	k_mutex_lock(&h2h_spi_lock, K_FOREVER);
	while (remaining > 0) {
		size_t chunk = (remaining > 256) ? 256 : remaining;
		uint8_t tx[4] = {
			SPI_CMD_READ,
			(uint8_t)((offset >> 16) & 0xFF),
			(uint8_t)((offset >> 8) & 0xFF),
			(uint8_t)(offset & 0xFF),
		};
		uint8_t rx_full[260];

		if (!device_is_ready(h2h_spi_dev)) {
			k_mutex_unlock(&h2h_spi_lock);
			return -ENODEV;
		}

		memset(sram_tx_buf, 0, chunk + 4);
		memcpy(sram_tx_buf, tx, 4);
		memset(sram_rx_buf, 0, chunk + 4);

		struct spi_buf tx_b = { .buf = sram_tx_buf, .len = chunk + 4 };
		struct spi_buf_set tx_s = { .buffers = &tx_b, .count = 1 };
		struct spi_buf rx_b = { .buf = sram_rx_buf, .len = chunk + 4 };
		struct spi_buf_set rx_s = { .buffers = &rx_b, .count = 1 };

		int rc =
			spi_transceive(h2h_spi_dev, &h2h_spi_cfg, &tx_s, &rx_s);
		if (rc != 0) {
			k_mutex_unlock(&h2h_spi_lock);
			return rc;
		}
		memcpy(rx_full, sram_rx_buf, chunk + 4);
		memcpy(out, &rx_full[4], chunk);

		out += chunk;
		offset += chunk;
		remaining -= chunk;
	}
	k_mutex_unlock(&h2h_spi_lock);
	return 0;
#else
	return -ENOTSUP;
#endif
}

int h2h_flash_erase(off_t offset, size_t size)
{
#if DT_NODE_EXISTS(DT_NODELABEL(spi0))
	ensure_lock_init();
	if ((offset % 4096) != 0 || (size % 4096) != 0) {
		return -EINVAL;
	}

	k_mutex_lock(&h2h_spi_lock, K_FOREVER);
	for (size_t erased = 0; erased < size; erased += 4096) {
		off_t cur = offset + erased;
		int rc = h2h_write_enable();
		if (rc != 0) {
			k_mutex_unlock(&h2h_spi_lock);
			return rc;
		}

		uint8_t cmd[4] = {
			SPI_CMD_SECTOR_ERASE,
			(uint8_t)((cur >> 16) & 0xFF),
			(uint8_t)((cur >> 8) & 0xFF),
			(uint8_t)(cur & 0xFF),
		};
		rc = h2h_spi_xfer(cmd, sizeof(cmd), NULL, 0);
		if (rc != 0) {
			k_mutex_unlock(&h2h_spi_lock);
			return rc;
		}

		rc = h2h_wait_ready(500);
		if (rc != 0) {
			k_mutex_unlock(&h2h_spi_lock);
			return rc;
		}
	}
	k_mutex_unlock(&h2h_spi_lock);
	return 0;
#else
	return -ENOTSUP;
#endif
}

int h2h_flash_write(off_t offset, const void *data, size_t len)
{
#if DT_NODE_EXISTS(DT_NODELABEL(spi0))
	ensure_lock_init();
	const uint8_t *in = (const uint8_t *)data;
	size_t remaining = len;

	k_mutex_lock(&h2h_spi_lock, K_FOREVER);
	while (remaining > 0) {
		size_t page_off = offset % 256;
		size_t chunk = 256 - page_off;
		if (chunk > remaining) {
			chunk = remaining;
		}

		int rc = h2h_write_enable();
		if (rc != 0) {
			k_mutex_unlock(&h2h_spi_lock);
			return rc;
		}

		uint8_t tx[260];
		tx[0] = SPI_CMD_PAGE_PROG;
		tx[1] = (uint8_t)((offset >> 16) & 0xFF);
		tx[2] = (uint8_t)((offset >> 8) & 0xFF);
		tx[3] = (uint8_t)(offset & 0xFF);
		memcpy(&tx[4], in, chunk);

		rc = h2h_spi_xfer(tx, chunk + 4, NULL, 0);
		if (rc != 0) {
			k_mutex_unlock(&h2h_spi_lock);
			return rc;
		}

		rc = h2h_wait_ready(50);
		if (rc != 0) {
			k_mutex_unlock(&h2h_spi_lock);
			return rc;
		}

		in += chunk;
		offset += chunk;
		remaining -= chunk;
	}
	k_mutex_unlock(&h2h_spi_lock);
	return 0;
#else
	return -ENOTSUP;
#endif
}

static int cmd_h2h_acquire(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int rc = h2h_spi_acquire();
	if (rc == 0) {
		shell_print(
			sh,
			"H2H SPI bus acquired by EC (PL27A1 held in reset)");
	} else {
		shell_error(sh, "Failed to acquire H2H SPI bus (%d)", rc);
	}
	return rc;
}

static int cmd_h2h_release(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	int rc = h2h_spi_release();
	if (rc == 0) {
		shell_print(sh, "H2H SPI bus released (PL27A1 running)");
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

	off_t offset = (off_t)strtoul(argv[1], NULL, 0);
	size_t len = (size_t)strtoul(argv[2], NULL, 0);
	if (len > 256) {
		len = 256;
	}

	uint8_t buf[256];
	bool auto_acquired = false;
	if (!bus_acquired) {
		(void)h2h_spi_acquire();
		auto_acquired = true;
	}

	int rc = h2h_flash_read(offset, buf, len);
	if (rc == 0) {
		shell_hexdump(sh, buf, len);
	} else {
		shell_error(sh, "Flash read failed (%d)", rc);
	}

	if (auto_acquired) {
		(void)h2h_spi_release();
	}
	return rc;
}

static int cmd_h2h_erase(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 3) {
		shell_error(sh,
			    "Usage: h2h_flash erase <offset_hex> <size_hex>");
		return -EINVAL;
	}

	off_t offset = (off_t)strtoul(argv[1], NULL, 0);
	size_t size = (size_t)strtoul(argv[2], NULL, 0);

	bool auto_acquired = false;
	if (!bus_acquired) {
		(void)h2h_spi_acquire();
		auto_acquired = true;
	}

	int rc = h2h_flash_erase(offset, size);
	if (rc == 0) {
		shell_print(sh, "Erased 0x%zx bytes at offset 0x%lx", size,
			    (unsigned long)offset);
	} else {
		shell_error(sh, "Flash erase failed (%d)", rc);
	}

	if (auto_acquired) {
		(void)h2h_spi_release();
	}
	return rc;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_h2h_flash_cmds,
	SHELL_CMD(acquire, NULL, "Acquire H2H SPI bus for EC", cmd_h2h_acquire),
	SHELL_CMD(release, NULL, "Release H2H SPI bus to PL27A1",
		  cmd_h2h_release),
	SHELL_CMD(read, NULL, "Read bytes from H2H flash", cmd_h2h_read),
	SHELL_CMD(erase, NULL, "Erase sectors on H2H flash", cmd_h2h_erase),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(h2h_flash, &sub_h2h_flash_cmds,
		   "Prolific PL27A1 H2H SPI Flash commands", NULL);
