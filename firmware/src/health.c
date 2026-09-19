/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "health.h"
#include "nvs_storage.h"

#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/bbram.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/crc.h>
#include <zephyr/sys/reboot.h>
#include <zephyr/sys/sys_io.h>

#include <soc.h>

LOG_MODULE_REGISTER(health);

/*
 * This file is also compiled for maui_proto_3507 and maui_proto_5117, which are
 * Arm MSPM0 parts with none of the ITE registers used below. Gate every
 * IT82002-specific piece on this one macro rather than repeating the condition.
 */
#if defined(CONFIG_SOC_IT82002AW) || defined(CONFIG_BOARD_MAUI_V1P2_IT82002)
#define MAUI_SOC_IT82002 1
#else
#define MAUI_SOC_IT82002 0
#endif

/* ITE IT82002 GCTRL & WDT Hardware Registers */

#ifndef IT83XX_GCTRL_BASE
#define IT83XX_GCTRL_BASE 0x00F02000UL
#endif
#define IT83XX_GCTRL_EPLR (*(volatile uint8_t *)(IT83XX_GCTRL_BASE + 0x37))
#define IT83XX_GCTRL_ETWDUARTCR \
	(*(volatile uint8_t *)(IT83XX_GCTRL_BASE + 0x4B))
#ifndef IT83XX_GCTRL_EWPR0PFH
#define IT83XX_GCTRL_EWPR0PFH(i) \
	(*(volatile uint8_t *)(IT83XX_GCTRL_BASE + 0x60 + (i)))
#define IT83XX_GCTRL_EWPR0PFD(i) \
	(*(volatile uint8_t *)(IT83XX_GCTRL_BASE + 0xA0 + (i)))
#endif
#ifndef IT83XX_GCTRL_EWPR0PFEC
#define IT83XX_GCTRL_EWPR0PFEC(i) \
	(*(volatile uint8_t *)(IT83XX_GCTRL_BASE + 0xC0 + (i)))
#endif

#ifndef IT8XXX2_GCTRL_EPLR_ENABLE
#define IT8XXX2_GCTRL_EPLR_ENABLE BIT(0)
#endif
#ifndef IT8XXX2_GCTRL_ETWD_HW_RST_EN
#define IT8XXX2_GCTRL_ETWD_HW_RST_EN BIT(0)
#endif

/* On IT82xx2 (IT82002), twd0 is at 0x00F01F80 (unlike IT81xx2/IT83xx at
 * 0x00F01F00) */
#define IT83XX_WDT_BASE 0x00F01F80UL
#define IT83XX_WDT_ETWCFG (*(volatile uint8_t *)(IT83XX_WDT_BASE + 0x01))
#define IT83XX_WDT_ET1PSR (*(volatile uint8_t *)(IT83XX_WDT_BASE + 0x02))
#define IT83XX_WDT_ETWCTRL (*(volatile uint8_t *)(IT83XX_WDT_BASE + 0x05))
#define IT83XX_WDT_EWDCNTLR (*(volatile uint8_t *)(IT83XX_WDT_BASE + 0x06))
#define IT83XX_WDT_EWDKEYR (*(volatile uint8_t *)(IT83XX_WDT_BASE + 0x07))
#define IT83XX_WDT_EWDCNTHR (*(volatile uint8_t *)(IT83XX_WDT_BASE + 0x09))

#ifndef IT8XXX2_WDT_EWDKEYEN
#define IT8XXX2_WDT_EWDKEYEN BIT(5)
#define IT8XXX2_WDT_EWDSRC BIT(4)
#define IT8XXX2_WDT_LEWDCNTL BIT(3)
#define IT8XXX2_WDT_EWDSCEN BIT(5)
#define IT8XXX2_WDT_ETPS_1P024_KHZ 0x01
#endif

/* Disable all ILM SCAR mappings before jumping/resetting */
#define IT8XXX2_SMFI_SCARH_DISABLE BIT(3)
#define DISABLE_ILM_MAPPING(idx, node_id)                          \
	do {                                                       \
		uintptr_t addr = DT_REG_ADDR_BY_IDX(node_id, idx); \
		sys_write8(IT8XXX2_SMFI_SCARH_DISABLE, addr + 2);  \
	} while (0)

static void it82002_disable_ilm_mappings(void)
{
#if DT_NODE_HAS_STATUS(DT_NODELABEL(ilm), okay)
	LISTIFY(DT_NUM_REGS(DT_NODELABEL(ilm)), DISABLE_ILM_MAPPING, (;),
		DT_NODELABEL(ilm));
#endif
}

static int it82002_early_clear_ilm(void)
{
	/* Clear any SCAR ILM mappings left over from previous bootloader stage
	 * before PRE_KERNEL_1 (it8xxx2_ilm_init) copies and maps this stage's
	 * ILM.
	 */
	it82002_disable_ilm_mappings();

	/*
	 * Note: the Application deliberately does NOT program the GCTRL Eflash
	 * write-protect registers (EWPRxPFEC/PFH/PFD). The RO and RW
	 * Bootloaders set them up before chainloading, so that protection
	 * policy can never be influenced by application code. See health
	 * test_flash_protect.
	 */
	return 0;
}

SYS_INIT(it82002_early_clear_ilm, EARLY, 0);

#if MAUI_SOC_IT82002
/**
 * Trigger an External Timer Watchdog (ETWD) Hardware Reset on IT82002.
 * This pulses the internal hardware reset signal and resets all CPU/SCAR state.
 * As an EC-domain reset it clears the EWPRxPF* flash write-protect registers
 * (which the bootloaders then reprogram) and, measured on silicon, GCTRL_EPLR
 * as well - so a reset always returns the chip to a programmable state.
 */
static void __attribute__((noreturn)) it82002_trigger_etwd_hw_reset(void)
{
	irq_lock();

	/*
	 * Arm the external watchdog with the shortest possible timeout, then
	 * deliberately fail its key check to fire it immediately. Writing
	 * ETWCFG = 0 first is what unlocks the other WDT registers, and EWDSCEN
	 * must be cleared or the counter is held off and never reaches zero.
	 */
	IT83XX_GCTRL_ETWDUARTCR |= IT8XXX2_GCTRL_ETWD_HW_RST_EN;

	IT83XX_WDT_ETWCFG = 0x00;
	IT83XX_WDT_ET1PSR = IT8XXX2_WDT_ETPS_1P024_KHZ;
	IT83XX_WDT_ETWCTRL &= ~IT8XXX2_WDT_EWDSCEN;
	IT83XX_WDT_EWDCNTHR = 0x00;
	IT83XX_WDT_EWDCNTLR = 0x01;
	IT83XX_WDT_ETWCFG = (IT8XXX2_WDT_EWDKEYEN | IT8XXX2_WDT_EWDSRC |
			     IT8XXX2_WDT_LEWDCNTL);

	/* Any key other than 0x5c resets the chip. */
	IT83XX_WDT_EWDKEYR = 0x00;

	while (1) {
		k_busy_wait(1000);
	}
}

/*
 * Override Zephyr's RISC-V sys_arch_reboot stub, which is an empty __weak
 * function, so 'kernel reboot' and sys_reboot() perform a real hardware reset.
 *
 * Only for IT82002: on the Arm boards the arch implementation is also __weak,
 * so defining this unconditionally would silently replace a working
 * NVIC_SystemReset with a spin loop.
 */
void sys_arch_reboot(int type)
{
	ARG_UNUSED(type);
	it82002_trigger_etwd_hw_reset();
}
#endif /* MAUI_SOC_IT82002 */

#define BBRAM_CRASHLOOP_MAGIC_MASK 0xFFFFFF00UL
#define BBRAM_CRASHLOOP_MAGIC_VAL 0x43525300UL

static inline uint32_t bbram_decode_crashloop(uint32_t raw)
{
	if ((raw & BBRAM_CRASHLOOP_MAGIC_MASK) == BBRAM_CRASHLOOP_MAGIC_VAL) {
		return raw & 0xFFU;
	}
	return 0;
}

static inline uint32_t bbram_encode_crashloop(uint32_t cnt)
{
	return BBRAM_CRASHLOOP_MAGIC_VAL | (cnt & 0xFFU);
}

/*
 * How long the Main Application must run before the boot is declared healthy
 * and the BBRAM crashloop counter is cleared. It has to outlast early-init
 * crashes (drivers, USB enumeration) so that a boot which dies shortly after
 * start still counts as a failure and eventually escalates to recovery.
 */
#define HEALTH_STABLE_DELAY_SEC 3

static struct k_work_delayable health_stable_work;

static void health_mark_stable_handler(struct k_work *work)
{
	ARG_UNUSED(work);

#if DT_NODE_EXISTS(DT_NODELABEL(bbram)) && IS_ENABLED(CONFIG_BBRAM)
	const struct device *const bbram_dev =
		DEVICE_DT_GET(DT_NODELABEL(bbram));
	if (device_is_ready(bbram_dev)) {
		uint32_t rst_val = bbram_encode_crashloop(0);
		(void)bbram_write(bbram_dev, BBRAM_OFFSET_CRASHLOOP_CNT,
				  sizeof(rst_val), (const uint8_t *)&rst_val);
		LOG_INF("OS stable for %ds: reset BBRAM crashloop counter to 0",
			HEALTH_STABLE_DELAY_SEC);
	}
#endif
}

int health_init(void)
{
#if DT_NODE_EXISTS(DT_NODELABEL(bbram)) && IS_ENABLED(CONFIG_BBRAM)
	const struct device *const bbram_dev =
		DEVICE_DT_GET(DT_NODELABEL(bbram));
	if (device_is_ready(bbram_dev)) {
		uint32_t raw_cnt = 0;
		uint32_t test_magic = 0;

		(void)bbram_read(bbram_dev, BBRAM_OFFSET_CRASHLOOP_CNT,
				 sizeof(raw_cnt), (uint8_t *)&raw_cnt);
		(void)bbram_read(bbram_dev, BBRAM_OFFSET_TEST_MAGIC,
				 sizeof(test_magic), (uint8_t *)&test_magic);

		uint32_t cnt = bbram_decode_crashloop(raw_cnt);
		LOG_INF("Boot health check: BBRAM crashloop_cnt=%u (raw=0x%08X)",
			cnt, raw_cnt);
		if (test_magic == ETWD_BBRAM_TEST_MAGIC) {
			LOG_INF("ETWD Hardware Reset Test PASSED: BBRAM preserved 0x%08X!",
				test_magic);
		}
	}
#endif

	k_work_init_delayable(&health_stable_work, health_mark_stable_handler);
	k_work_schedule(&health_stable_work,
			K_SECONDS(HEALTH_STABLE_DELAY_SEC));
	return 0;
}

int health_reboot_to(enum boot_target_t target)
{
#if DT_NODE_EXISTS(DT_NODELABEL(bbram)) && IS_ENABLED(CONFIG_BBRAM)
	const struct device *const bbram_dev =
		DEVICE_DT_GET(DT_NODELABEL(bbram));
	if (device_is_ready(bbram_dev)) {
		uint32_t val = (uint32_t)target;
		(void)bbram_write(bbram_dev, BBRAM_OFFSET_BOOT_TARGET,
				  sizeof(val), (const uint8_t *)&val);
	}
#endif

	if (target == BOOT_TARGET_FORCE_RO) {
		/* Also store NVS fallback flag in case ETWD clears BBRAM on
		 * this board */
		(void)nvs_storage_set_boot_target(
			(uint32_t)BOOT_TARGET_FORCE_RO);
		LOG_INF("Triggering ETWD Hardware Reset into RO Bootloader Recovery...");
		k_msleep(20);
#if MAUI_SOC_IT82002
		it82002_trigger_etwd_hw_reset();
#else
		sys_reboot(SYS_REBOOT_COLD);
#endif
	} else {
		LOG_INF("Triggering software reboot (target=0x%08X)...",
			(uint32_t)target);
		k_msleep(20);
		sys_reboot(SYS_REBOOT_COLD);
	}

	return 0;
}

static int cmd_health_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

#if DT_NODE_EXISTS(DT_NODELABEL(bbram)) && IS_ENABLED(CONFIG_BBRAM)
	const struct device *const bbram_dev =
		DEVICE_DT_GET(DT_NODELABEL(bbram));
	if (device_is_ready(bbram_dev)) {
		uint32_t target = 0, cnt = 0, magic = 0;

		(void)bbram_read(bbram_dev, BBRAM_OFFSET_BOOT_TARGET, 4,
				 (uint8_t *)&target);
		(void)bbram_read(bbram_dev, BBRAM_OFFSET_CRASHLOOP_CNT, 4,
				 (uint8_t *)&cnt);
		(void)bbram_read(bbram_dev, BBRAM_OFFSET_TEST_MAGIC, 4,
				 (uint8_t *)&magic);

		shell_print(sh, "BBRAM boot_target   : 0x%08X", target);
		shell_print(sh, "BBRAM crashloop_cnt : %u (raw=0x%08X)",
			    bbram_decode_crashloop(cnt), cnt);
		shell_print(sh, "BBRAM test_magic    : 0x%08X %s", magic,
			    (magic == ETWD_BBRAM_TEST_MAGIC) ?
				    "(ETWD BBRAM retention VERIFIED)" :
				    "");
	}
#endif

#if MAUI_SOC_IT82002
	shell_print(sh, "GCTRL_EPLR          : 0x%02X (%s)", IT83XX_GCTRL_EPLR,
		    (IT83XX_GCTRL_EPLR & IT8XXX2_GCTRL_EPLR_ENABLE) ?
			    "LOCKED" :
			    "UNLOCKED");

	/*
	 * One EWPRx register covers 32 KB. EWPR0..6 span both bootloaders;
	 * EWPR7+ must stay clear so serial recovery can erase the App slot.
	 * See it82002_mcuboot_hook.c for how the geometry was established.
	 */
	shell_print(sh, "%-20s: %-9s %-9s %-9s", "EWPR register", "PFEC(EC)",
		    "PFH(Host)", "PFD(DBGR)");
	for (int i = 0; i < 8; i++) {
		shell_print(
			sh,
			"EWPR%-2d 0x%05X-0x%05X: 0x%02X      0x%02X      0x%02X   %s",
			i, i * 0x8000, (i + 1) * 0x8000 - 1,
			IT83XX_GCTRL_EWPR0PFEC(i), IT83XX_GCTRL_EWPR0PFH(i),
			IT83XX_GCTRL_EWPR0PFD(i),
			(i < 3) ? "RO Bootloader" :
			(i < 7) ? "RW Bootloader" :
				  "App slot (must be 0x00)");
	}
	shell_print(
		sh,
		"EC App Flash Guard  : 0x000000 - 0x037FFF (blocked when EWPR0..6 PFEC = 0xFF)");
#endif
	return 0;
}

#if MAUI_SOC_IT82002 && defined(CONFIG_MAUI_HEALTH_FLASH_DIAGNOSTICS)
/*
 * CRC the given flash range so a partially erased sector can never be missed,
 * and report whether the range is already blank (all 0xFF).
 */
static int flash_region_crc(const struct device *dev, off_t off, size_t len,
			    uint32_t *out, bool *blank)
{
	uint8_t buf[64];
	uint32_t crc = 0;
	bool is_blank = true;

	for (size_t done = 0; done < len; done += sizeof(buf)) {
		int rc = flash_read(dev, off + done, buf, sizeof(buf));

		if (rc != 0) {
			return rc;
		}
		for (size_t b = 0; b < sizeof(buf); b++) {
			if (buf[b] != 0xFF) {
				is_blank = false;
			}
		}
		crc = crc32_ieee_update(crc, buf, sizeof(buf));
	}
	*out = crc;
	if (blank != NULL) {
		*blank = is_blank;
	}
	return 0;
}

static int cmd_health_test_flash_protect(const struct shell *sh, size_t argc,
					 char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	const struct device *flash_dev = PARTITION_DEVICE(storage_partition);
	if (!device_is_ready(flash_dev)) {
		shell_error(sh, "Flash device not ready");
		return -ENODEV;
	}

	bool all_passed = true;

	shell_print(
		sh,
		"=== Verifying Bootloader Flash Write-Protection (0x00000 - 0x37FFF) ===");

	/*
	 * 1. The registers that actually block this CPU: EWPRxPFEC ("Eflash
	 * Write Protect Region Control Register for path from EC"), 32 KB per
	 * register (1 bit per 4 KB sector). EWPR0..6 must all read 0xFF to
	 * cover 0x00000 - 0x37FFF.
	 */
	unsigned int unprotected_regs = 0;

	for (int i = 0; i < 7; i++) {
		if (IT83XX_GCTRL_EWPR0PFEC(i) != 0xFF) {
			unprotected_regs++;
		}
	}
	shell_print(sh, "1. EC-path write protect (EWPR0..6 PFEC): %s",
		    (unprotected_regs == 0) ?
			    "all 0xFF -> PROTECTED (PASS)" :
			    "incomplete -> VULNERABLE (FAIL)");
	shell_print(
		sh, "2. Protection lock (GCTRL_EPLR): 0x%02X -> %s",
		IT83XX_GCTRL_EPLR,
		(IT83XX_GCTRL_EPLR & IT8XXX2_GCTRL_EPLR_ENABLE) ?
			"FROZEN (PASS - App cannot clear EWPR)" :
			"unlocked (WEAK - App could clear EWPR and then write)");

	/*
	 * Safety guard: step 3 below is a real attack - it erases and rewrites
	 * bootloader sectors. If the hardware is not actually protecting them,
	 * that attack succeeds and destroys the bootloaders (the board then
	 * only recovers with an external programmer). Bail out before doing any
	 * damage.
	 */
	if (unprotected_regs != 0) {
		shell_print(
			sh,
			"   (%u of 7 registers are not fully set, see 'health status')",
			unprotected_regs);
		shell_error(
			sh,
			"ABORTED: skipping the erase/write attack - it would DESTROY the "
			"unprotected bootloaders.");
		shell_error(
			sh,
			"=== RESULT: FAIL - Bootloader flash is NOT write-protected! ===");
		return -EACCES;
	}

	shell_print(sh,
		    "3. Active erase/write attack tests from the Application:");
	struct {
		const char *name;
		off_t offset;
	} test_regions[] = {
		{ "RO Bootloader Vectors", 0x00000000 },
		{ "RO Bootloader Last Code", 0x00012000 },
		{ "RO Bootloader Tail", 0x00017000 },
		{ "RW Bootloader Header", 0x00018000 },
		{ "RW Bootloader Last Code", 0x00029000 },
		{ "RW Bootloader Tail", 0x00037000 },
	};

	for (size_t i = 0; i < ARRAY_SIZE(test_regions); i++) {
		off_t off = test_regions[i].offset;
		uint8_t orig[16];
		uint8_t attack[16];
		uint32_t crc_before = 0, crc_after = 0;
		bool blank = false;

		/* CRC the whole 4 KB sector, not just the bytes we try to
		 * overwrite. */
		int rc_read = flash_region_crc(flash_dev, off, 4096,
					       &crc_before, &blank);

		if (rc_read == 0) {
			rc_read =
				flash_read(flash_dev, off, orig, sizeof(orig));
		}
		if (rc_read != 0) {
			shell_error(sh, "  [0x%06lx] Failed to read flash (%d)",
				    (unsigned long)off, rc_read);
			all_passed = false;
			continue;
		}

		/* Inverted pattern: only clears bits, so it works even without
		 * an erase. */
		for (size_t b = 0; b < sizeof(attack); b++) {
			attack[b] = (orig[b] == 0xFF) ? 0x5A : 0x00;
		}

		int rc_erase = flash_erase(flash_dev, off, 4096);
		int rc_write =
			flash_write(flash_dev, off, attack, sizeof(attack));

		(void)flash_region_crc(flash_dev, off, 4096, &crc_after, NULL);

		bool unchanged = (crc_before == crc_after);
		/*
		 * The hardware silently drops the transaction, so the upstream
		 * ITE driver only notices via its read-back verification and
		 * returns -EINVAL. On an already blank sector that verification
		 * (which just checks for 0xFF) trivially passes, so rc_erase ==
		 * 0 proves nothing there - the sector content is the real
		 * evidence.
		 */
		bool erase_blocked = (rc_erase != 0) || blank;
		bool blocked = erase_blocked && (rc_write != 0) && unchanged;

		shell_print(
			sh,
			"  [0x%06lx] %-24s: Erase=%-3d Write=%-3d %-9s %s -> %s",
			(unsigned long)off, test_regions[i].name, rc_erase,
			rc_write, blank ? "(blank)" : "(code)",
			unchanged ? "unchanged" : "CHANGED",
			blocked ? "PROTECTED (PASS)" : "VULNERABLE (FAIL)");

		if (!blocked) {
			all_passed = false;
		}
	}

	if (all_passed) {
		shell_print(
			sh,
			"=== RESULT: PASS - Bootloader flash space (0x00000 - 0x37FFF) cannot be modified by App ===");
		return 0;
	}

	shell_error(
		sh,
		"=== RESULT: FAIL - Bootloader flash protection check failed! ===");
	return -EACCES;
}
#endif /* MAUI_SOC_IT82002 && CONFIG_MAUI_HEALTH_FLASH_DIAGNOSTICS */

#if MAUI_SOC_IT82002
static int cmd_health_test_etwd(const struct shell *sh, size_t argc,
				char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

#if DT_NODE_EXISTS(DT_NODELABEL(bbram)) && IS_ENABLED(CONFIG_BBRAM)
	const struct device *const bbram_dev =
		DEVICE_DT_GET(DT_NODELABEL(bbram));
	if (device_is_ready(bbram_dev)) {
		uint32_t magic = ETWD_BBRAM_TEST_MAGIC;
		(void)bbram_write(bbram_dev, BBRAM_OFFSET_TEST_MAGIC,
				  sizeof(magic), (const uint8_t *)&magic);
	}
#endif
	shell_print(
		sh,
		"Wrote 0x%08X to BBRAM[8..11]. Triggering ETWD Hardware Reset now...",
		(unsigned int)ETWD_BBRAM_TEST_MAGIC);
	shell_print(
		sh,
		"After reboot, run 'health status' to verify BBRAM retention.");
	k_msleep(50);
	it82002_trigger_etwd_hw_reset();
	return 0;
}
#endif /* MAUI_SOC_IT82002 */

static int cmd_health_set_crashloop(const struct shell *sh, size_t argc,
				    char **argv)
{
	if (argc < 2) {
		shell_print(sh, "Usage: health set_crashloop <count>");
		return -EINVAL;
	}

	/* Cancel the stability timer so it doesn't overwrite our manual test
	 * counter */
	(void)k_work_cancel_delayable(&health_stable_work);

#if DT_NODE_EXISTS(DT_NODELABEL(bbram)) && IS_ENABLED(CONFIG_BBRAM)
	uint32_t cnt = (uint32_t)strtoul(argv[1], NULL, 0);
	const struct device *const bbram_dev =
		DEVICE_DT_GET(DT_NODELABEL(bbram));
	if (device_is_ready(bbram_dev)) {
		uint32_t val = bbram_encode_crashloop(cnt);
		(void)bbram_write(bbram_dev, BBRAM_OFFSET_CRASHLOOP_CNT,
				  sizeof(val), (const uint8_t *)&val);
		shell_print(
			sh,
			"Set BBRAM crashloop_cnt to %u (raw=0x%08X). Stability timer cancelled.",
			cnt, val);
		shell_print(
			sh,
			"Note: RO Bootloader increments counter by +1 on boot.");
		shell_print(
			sh,
			"  - Set to 2 + reboot -> reaches 3 -> stops in RW Bootloader");
		shell_print(
			sh,
			"  - Set to 3 + reboot -> reaches 4 -> stops in RO Recovery Bootloader");
	}
#endif
	return 0;
}

#if MAUI_SOC_IT82002 && defined(CONFIG_MAUI_HEALTH_FLASH_DIAGNOSTICS)
/*
 * Determine experimentally what GCTRL_EPLR actually locks.
 *
 * The datasheet only says EPLR "can only be write 1 and only be cleared by
 * power-on reset", but never states whether the EWPRx registers then become
 * fully read-only or merely un-clearable. That difference decides our policy:
 *
 *   - "sticky" (clearing blocked, setting still allowed): the RO Bootloader can
 *     lock its own region early and later stages can still add protection.
 *   - "frozen"  (all writes ignored): whoever sets EPLR freezes the register
 *     contents, so the lock must be applied by the last stage only - and since
 *     an EC-domain (warm/watchdog) reset clears the EWPR registers back to
 * 0x00, locking too early would leave the flash UNPROTECTED until the next
 * reset.
 *
 * Answered on silicon: FROZEN. Hence only the RW Bootloader locks, and only on
 * its final jump into the App. Kept so the result can be re-confirmed on new
 * silicon revisions.
 *
 * EWPR15PFEC (0xC0 + 15) covers 0x3C000-0x3FFFF, which is inside the App slot,
 * so this probe never protects anything we need to write.
 */
static int cmd_health_probe_eplr(const struct shell *sh, size_t argc,
				 char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	bool locked = (IT83XX_GCTRL_EPLR & IT8XXX2_GCTRL_EPLR_ENABLE) != 0;

	shell_print(sh, "GCTRL_EPLR = 0x%02X (%s)", IT83XX_GCTRL_EPLR,
		    locked ? "LOCKED" : "unlocked");
	if (!locked) {
		shell_print(sh, "EPLR is not set, so nothing to probe.");
		shell_print(
			sh,
			"Run 'health lock_eplr' first (cleared by the next reset).");
		return 0;
	}

	/* Can we still SET a protection bit that was previously clear? */
	uint8_t before_set = IT83XX_GCTRL_EWPR0PFEC(15);

	IT83XX_GCTRL_EWPR0PFEC(15) = 0xFF;
	uint8_t after_set = IT83XX_GCTRL_EWPR0PFEC(15);

	/* Can we CLEAR an already protected register? */
	uint8_t before_clr = IT83XX_GCTRL_EWPR0PFEC(0);

	IT83XX_GCTRL_EWPR0PFEC(0) = 0x00;
	uint8_t after_clr = IT83XX_GCTRL_EWPR0PFEC(0);

	if (after_clr != before_clr) {
		/* Should never happen while locked - restore immediately. */
		IT83XX_GCTRL_EWPR0PFEC(0) = before_clr;
	}

	shell_print(
		sh,
		"Set  test: EWPR15PFEC 0x%02X -> wrote 0xFF -> reads 0x%02X (%s)",
		before_set, after_set,
		(after_set == 0xFF) ? "SET ALLOWED" : "set blocked");
	shell_print(
		sh,
		"Clear test: EWPR0PFEC  0x%02X -> wrote 0x00 -> reads 0x%02X (%s)",
		before_clr, after_clr,
		(after_clr == before_clr) ? "clear BLOCKED" : "CLEAR ALLOWED");

	if (after_clr != before_clr) {
		shell_error(
			sh,
			"=> EPLR does not protect the EWPR registers at all!");
	} else if (after_set == 0xFF) {
		shell_print(
			sh,
			"=> EPLR semantics: STICKY (bits can be added, never removed)");
	} else {
		shell_print(
			sh,
			"=> EPLR semantics: FROZEN (register contents fixed until the next reset)");
	}
	return 0;
}

static int cmd_health_lock_eplr(const struct shell *sh, size_t argc,
				char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	IT83XX_GCTRL_EPLR |= IT8XXX2_GCTRL_EPLR_ENABLE;
	shell_print(sh, "GCTRL_EPLR = 0x%02X (cleared by the next reset)",
		    IT83XX_GCTRL_EPLR);
	shell_print(sh, "Now run 'health probe_eplr'.");
	return 0;
}
#endif /* MAUI_SOC_IT82002 && CONFIG_MAUI_HEALTH_FLASH_DIAGNOSTICS */

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_health_cmds,
	SHELL_CMD(status, NULL, "Show BBRAM health & flash protection status",
		  cmd_health_status),
	SHELL_CMD(set_crashloop, NULL, "Set BBRAM crashloop count (<cnt>)",
		  cmd_health_set_crashloop),
#if MAUI_SOC_IT82002
	SHELL_CMD(test_etwd_bbram, NULL,
		  "Test ETWD hardware reset vs BBRAM retention",
		  cmd_health_test_etwd),
#endif
#if MAUI_SOC_IT82002 && defined(CONFIG_MAUI_HEALTH_FLASH_DIAGNOSTICS)
	SHELL_CMD(test_flash_protect, NULL,
		  "Erase/write bootloader flash to prove it is write-protected",
		  cmd_health_test_flash_protect),
	SHELL_CMD(lock_eplr, NULL,
		  "Freeze the EWPR registers until the next reset",
		  cmd_health_lock_eplr),
	SHELL_CMD(probe_eplr, NULL,
		  "Probe whether EPLR freezes or only sticks the EWPR registers",
		  cmd_health_probe_eplr),
#endif
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(health, &sub_health_cmds,
		   "Health & Flash Protection diagnostics", NULL);
