/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "bootutil/boot_hooks.h"
#include "bootutil/bootutil.h"
#include "bootutil/fault_injection_hardening.h"
#include "bootutil/image.h"
#include "flash_map_backend/flash_map_backend.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/bbram.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/storage/flash_map.h>
#include <zephyr/sys/sys_io.h>

#include <soc.h>

/* ITE IT82002 GCTRL Flash Protection, ETWD, & ILM Registers */
#ifndef IT83XX_GCTRL_BASE
#define IT83XX_GCTRL_BASE 0x00F02000UL
#endif
#define IT83XX_GCTRL_EPLR (*(volatile uint8_t *)(IT83XX_GCTRL_BASE + 0x37))
#define IT83XX_GCTRL_ETWDUARTCR \
	(*(volatile uint8_t *)(IT83XX_GCTRL_BASE + 0x4B))
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

/* BBRAM Layout Offsets & Magic Constants */
#define BBRAM_OFFSET_BOOT_TARGET 0
#define BBRAM_OFFSET_CRASHLOOP_CNT 4
#define BBRAM_OFFSET_TEST_MAGIC 8
/* One-shot marker for the stale-EPLR recovery below. */
#define BBRAM_OFFSET_EPLR_RETRY 12
#define BBRAM_EPLR_RETRY_MAGIC 0x45504C52UL /* 'EPLR' */

#define BOOT_TARGET_NORMAL 0x00000000UL
#define BOOT_TARGET_FORCE_RO 0x524F4254UL /* 'ROBT' */
#define BOOT_TARGET_FORCE_RW_RECOVERY 0x52574254UL /* 'RWBT' */
#define RW_CRASHLOOP_THRESHOLD 3
#define RO_CRASHLOOP_THRESHOLD 4

/*
 * Magic prefix ('CRS\0') for crashloop counter in BBRAM so uninitialized BBRAM
 * on boards without a VBAT capacitor never falsely triggers recovery on cold
 * boot.
 */
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

/* NVS Keys & Serial Number Constants */
#define NVS_KEY_SERIAL_NUMBER 0x0001
#define NVS_KEY_BOOT_TARGET 0x0002
#define NVS_DEFAULT_SERIAL "Uninitialized"
#define NVS_SERIAL_MAX_LEN 32

/*
 * Eflash write protection (GCTRL "Eflash Write Protect Region Control
 * Registers").
 *
 * There is one register set per bus initiator path:
 *   EWPRxPFH  (0x60 + x) - path from Host   (eSPI/LPC)
 *   EWPRxPFD  (0xA0 + x) - path from DBGR   (debugger)
 *   EWPRxPFEC (0xC0 + x) - path from EC     (this CPU, including the SMFI
 *                                            EC-indirect/follow-mode
 * programming path used by the Zephyr flash driver)
 *
 * Measured on silicon: each EWPR bit covers one 4 KB
 * flash sector, so each register covers 32 KB and the 16 registers span the
 * first 512 KB of flash. (The datasheet's register tables claim 2 KB per bit /
 * 256 KB total, which holds for 256 KB parts; on this 1 MB part the block size
 * scales with the flash size. Setting 14 registers protected 0x00000-0x6FFFF
 * and broke MCUboot serial recovery, which erases the App slot at 0x38000.)
 *
 * The registers are plain R/W until GCTRL_EPLR is set, after which they are
 * frozen. They live in the EC power domain, so any reset clears them to 0x00 -
 * and the ETWD hardware reset clears EPLR with them (measured; the datasheet
 * claims power-on reset only). Every boot path below therefore reprograms
 * them from scratch.
 *
 * Flash layout -> register count:
 *   RO Bootloader  0x00000 - 0x17FFF (96 KB)  -> EWPR0..EWPR2  (3 registers)
 *   RW Bootloader  0x18000 - 0x37FFF (128 KB) -> EWPR3..EWPR6  (4 registers)
 *   App slot       0x38000 - 0xF7FFF          -> never protected (serial
 * recovery must be able to erase it)
 */
#define IT8XXX2_EWPR_BLOCK_SIZE 0x1000U /* 4 KB per bit */
#define IT8XXX2_EWPR_REG_SPAN \
	(8U * IT8XXX2_EWPR_BLOCK_SIZE) /* 32 KB per register */

#define EWPR_REGS_RO_BOOTLOADER \
	(0x18000U / IT8XXX2_EWPR_REG_SPAN) /* 3: 0x00000-0x17FFF */
#define EWPR_REGS_BOTH_LOADERS \
	(0x38000U / IT8XXX2_EWPR_REG_SPAN) /* 7: 0x00000-0x37FFF */

BUILD_ASSERT((0x18000U % IT8XXX2_EWPR_REG_SPAN) == 0,
	     "RO Bootloader must end on an EWPR register boundary");
BUILD_ASSERT((0x38000U % IT8XXX2_EWPR_REG_SPAN) == 0,
	     "RW Bootloader must end on an EWPR register boundary");

/*
 * Protection policy.
 *
 * The RO Bootloader is the only stage that can be trusted, because it is the
 * only stage that cannot be reflashed over USB. It therefore owns the policy
 * and applies it before handing control anywhere:
 *
 *   RO -> chainload RW   EWPR0..6 (0x00000-0x37FFF) + EPLR freeze
 *   RO -> serial recovery EWPR0..2 (0x00000-0x17FFF) + EPLR freeze
 *   RW -> any path        re-applies the same registers; already frozen, so
 *                         every write is a no-op. Kept as a belt-and-braces
 *                         fallback for the case where RO could not arm them.
 *   App                   inherits; never touches these registers.
 *
 * Freezing before the chainload is what closes the escalation path: without
 * it, anything running as the RW Bootloader (which a USB-only attacker can
 * replace, since images are only hash-checked - see the board README's threat
 * model) could clear EWPR0..2 and overwrite the RO Bootloader.
 *
 * The RO recovery path deliberately arms only EWPR0..2 before freezing, so RO
 * serial recovery can still reflash the RW Bootloader at 0x18000. The App slot
 * is never protected on any path, because MCUboot's bs_upload() erases the
 * whole target slot on its first chunk.
 */
#if defined(MAUI_RO_BOOTLOADER)
#define EWPR_REGS_RECOVERY EWPR_REGS_RO_BOOTLOADER
#else
#define EWPR_REGS_RECOVERY EWPR_REGS_BOTH_LOADERS
#endif

/* Write-protect flash [0, reg_count * 32 KB) against EC writes.
 * Host (eSPI/LPC) and Debugger paths remain unlocked (0x00) so external
 * programmers and the host can always modify flash. */
static void it82002_flash_protect(unsigned int reg_count)
{
	for (unsigned int i = 0; i < reg_count; i++) {
		IT83XX_GCTRL_EWPR0PFEC(i) = 0xFF;
		IT83XX_GCTRL_EWPR0PFH(i) = 0x00;
		IT83XX_GCTRL_EWPR0PFD(i) = 0x00;
	}
}

/*
 * Freeze the EWPR registers via GCTRL_EPLR so no later stage can clear them
 * and rewrite an earlier one.
 *
 * Measured on silicon ('health lock_eplr' + 'health probe_eplr'): EPLR is a
 * hard freeze - once set, EWPR bits can be neither cleared nor set. The
 * datasheet states EPLR is "cleared only by power-on reset", but on this part
 * the ETWD hardware reset (what sys_reboot() performs - see sys_arch_reboot()
 * in src/health.c) clears it too, and the bootloaders then reprogram EWPR from
 * scratch. That is what makes freezing safe: any reset returns the chip to a
 * programmable state, so reflashing a bootloader never needs a power cycle.
 *
 * Fail-safe: only freeze after reading back the EC register file (EWPRxPFEC)
 * as fully set. Freezing a partially programmed register file would leave
 * flash unprotected AND unprotectable until the next reset, which is strictly
 * worse than not freezing at all.
 *
 * Idempotent: a second call from a later stage just re-ORs a bit that is
 * already set, and the read-back above still passes because the registers are
 * frozen in the armed state.
 */
static void it82002_flash_protect_lock(unsigned int reg_count)
{
	for (unsigned int i = 0; i < reg_count; i++) {
		if (IT83XX_GCTRL_EWPR0PFEC(i) != 0xFF) {
			return;
		}
	}

	IT83XX_GCTRL_EPLR |= IT8XXX2_GCTRL_EPLR_ENABLE;
}

/*
 * Arm and freeze in one step. Every exit from boot_go_hook() must do both, so
 * keep them together rather than risk a path that arms without freezing.
 */
static void it82002_flash_protect_and_lock(unsigned int reg_count)
{
	it82002_flash_protect(reg_count);
	it82002_flash_protect_lock(reg_count);
}

extern uint8_t *usb_get_device_descriptor(void);

/*
 * Clear the boot target and the crashloop counter. Used wherever we commit to a
 * known-good state: entering serial recovery, finishing an upload, or handling
 * an explicit 'os reset'.
 */
static void reset_bbram_boot_state(const struct device *bbram_dev)
{
	uint32_t zero = 0;
	uint32_t rst_cnt = bbram_encode_crashloop(0);

	(void)bbram_write(bbram_dev, BBRAM_OFFSET_BOOT_TARGET, sizeof(zero),
			  (const uint8_t *)&zero);
	(void)bbram_write(bbram_dev, BBRAM_OFFSET_CRASHLOOP_CNT,
			  sizeof(rst_cnt), (const uint8_t *)&rst_cnt);
}

static char boot_serial_str[NVS_SERIAL_MAX_LEN] = NVS_DEFAULT_SERIAL;

#if IS_ENABLED(CONFIG_NVS)
/*
 * Mount the storage partition. PARTITION_DEVICE() resolves to the flash
 * controller, so nvs_fs offsets are absolute flash addresses.
 */
static int mount_storage_nvs(struct nvs_fs *fs)
{
	struct flash_pages_info info;
	int rc;

	*fs = (struct nvs_fs){ 0 };
	fs->flash_device = PARTITION_DEVICE(storage_partition);
	if (!device_is_ready(fs->flash_device)) {
		return -ENODEV;
	}

	fs->offset = PARTITION_OFFSET(storage_partition);
	rc = flash_get_page_info_by_offs(fs->flash_device, fs->offset, &info);
	if (rc != 0) {
		return rc;
	}

	fs->sector_size = info.size;
	fs->sector_count = PARTITION_SIZE(storage_partition) / info.size;

	return nvs_mount(fs);
}
#endif /* CONFIG_NVS */

static void read_nvs_serial_number(void)
{
#if IS_ENABLED(CONFIG_NVS)
	struct nvs_fs fs;

	if (mount_storage_nvs(&fs) != 0) {
		return;
	}

	ssize_t len = nvs_read(&fs, NVS_KEY_SERIAL_NUMBER, boot_serial_str,
			       sizeof(boot_serial_str) - 1);
	if (len > 0) {
		boot_serial_str[len] = '\0';
	}
	/* On failure boot_serial_str keeps its NVS_DEFAULT_SERIAL initialiser.
	 */
#endif
}

uint8_t *usb_update_sn_string_descriptor(void)
{
	return (uint8_t *)boot_serial_str;
}

static void it82002_prepare_usb_serial_descriptor(void)
{
	read_nvs_serial_number();
	uint8_t *p = usb_get_device_descriptor();
	if (!p) {
		return;
	}

	/* Walk descriptor table to find SerialNumber String Descriptor (index
	 * 3) and trim bLength to match the exact NVS serial number string
	 * length.
	 */
	uint32_t str_idx = 0;
	while (p[0] != 0U) {
		uint8_t len = p[0];
		uint8_t type = p[1];

		if (type == 0x03) { /* USB_DESC_STRING */
			if (str_idx == 3) { /* USB_DESC_SERIAL_NUMBER_IDX */
				size_t actual_len =
					2 + strlen(boot_serial_str) * 2;
				if (actual_len <= len) {
					p[0] = (uint8_t)actual_len;
					/* Place terminator (bLength=0) right
					 * after SN descriptor */
					p[actual_len] = 0U;
				}
				break;
			}
			str_idx++;
		}
		p += len;
	}
}

/* Disable all ILM SCAR mappings before jumping between XIP images */
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
	return 0;
}
SYS_INIT(it82002_early_clear_ilm, EARLY, 0);

void sys_arch_reboot(int type)
{
	ARG_UNUSED(type);
	irq_lock();

#if defined(CONFIG_SOC_IT82002AW) || defined(CONFIG_BOARD_MAUI_V1P2_IT82002)
	IT83XX_GCTRL_ETWDUARTCR |= IT8XXX2_GCTRL_ETWD_HW_RST_EN;
	IT83XX_WDT_ETWCFG = 0x00;
	IT83XX_WDT_ET1PSR = IT8XXX2_WDT_ETPS_1P024_KHZ;
	IT83XX_WDT_ETWCTRL &= ~IT8XXX2_WDT_EWDSCEN;
	IT83XX_WDT_EWDCNTHR = 0x00;
	IT83XX_WDT_EWDCNTLR = 0x01;
	IT83XX_WDT_ETWCFG = (IT8XXX2_WDT_EWDKEYEN | IT8XXX2_WDT_EWDSRC |
			     IT8XXX2_WDT_LEWDCNTL);
	IT83XX_WDT_EWDKEYR = 0x00;
#endif
	while (1) {
		;
	}
}

/*
 * Recover from a stale EPLR freeze.
 *
 * The EWPR registers live in the EC power domain and are cleared to 0x00 by
 * every reset, so at the top of a boot they must be writable. If a write does
 * not stick, GCTRL_EPLR survived the reset: flash is then simultaneously
 * UNPROTECTED (EWPR is all zeroes) and UNPROTECTABLE (EPLR ignores writes), and
 * every it82002_flash_protect() call below would silently do nothing.
 *
 * We measured that the ETWD hardware reset clears EPLR, so one reset fixes it.
 * That measurement contradicts the datasheet, which claims only a power-on
 * reset clears the bit - hence the one-shot marker in BBRAM: if we come back
 * still frozen, the reset did not help on this part, so give up and boot
 * unprotected rather than reset-loop forever.
 *
 * Only the RO Bootloader needs this: it runs first on every boot, and it
 * chainloads the RW Bootloader without an intervening reset.
 */
#if defined(MAUI_RO_BOOTLOADER)
static void it82002_recover_stale_eplr(const struct device *bbram_dev)
{
	uint32_t retry = 0;

	(void)bbram_read(bbram_dev, BBRAM_OFFSET_EPLR_RETRY, sizeof(retry),
			 (uint8_t *)&retry);

	/* This bit is set by every protect path anyway, so probing with it is
	 * free. */
	IT83XX_GCTRL_EWPR0PFEC(0) = 0xFF;

	if (IT83XX_GCTRL_EWPR0PFEC(0) == 0xFF) {
		if (retry != 0) {
			retry = 0;
			(void)bbram_write(bbram_dev, BBRAM_OFFSET_EPLR_RETRY,
					  sizeof(retry),
					  (const uint8_t *)&retry);
		}
		return;
	}

	if (retry == BBRAM_EPLR_RETRY_MAGIC) {
		/* Already reset once and still frozen - stop trying. */
		retry = 0;
		(void)bbram_write(bbram_dev, BBRAM_OFFSET_EPLR_RETRY,
				  sizeof(retry), (const uint8_t *)&retry);
		return;
	}

	retry = BBRAM_EPLR_RETRY_MAGIC;
	(void)bbram_write(bbram_dev, BBRAM_OFFSET_EPLR_RETRY, sizeof(retry),
			  (const uint8_t *)&retry);
	sys_arch_reboot(0);
}

static bool check_nvs_force_ro_fallback(void)
{
#if IS_ENABLED(CONFIG_NVS)
	struct nvs_fs fs;
	uint32_t nvs_target = 0;
	int rc;

	if (mount_storage_nvs(&fs) != 0) {
		return false;
	}

	rc = nvs_read(&fs, NVS_KEY_BOOT_TARGET, &nvs_target,
		      sizeof(nvs_target));
	if (rc == sizeof(nvs_target) && nvs_target == BOOT_TARGET_FORCE_RO) {
		(void)nvs_delete(&fs, NVS_KEY_BOOT_TARGET);
		return true;
	}
#endif
	return false;
}
#endif

/* MCUboot Image Access Hooks (enabled via CONFIG_BOOT_IMAGE_ACCESS_HOOKS=y) */
int boot_read_image_header_hook(int img_index, int slot,
				struct image_header *img_head)
{
	ARG_UNUSED(img_index);
	ARG_UNUSED(slot);
	ARG_UNUSED(img_head);
	return BOOT_HOOK_REGULAR;
}

fih_ret boot_image_check_hook(int img_index, int slot)
{
	ARG_UNUSED(img_index);
	ARG_UNUSED(slot);
	FIH_RET(FIH_BOOT_HOOK_REGULAR);
}

int boot_perform_update_hook(int img_index, struct image_header *img_head,
			     const struct flash_area *area)
{
	ARG_UNUSED(img_index);
	ARG_UNUSED(img_head);
	ARG_UNUSED(area);
	return BOOT_HOOK_REGULAR;
}

int boot_copy_region_post_hook(int img_index, const struct flash_area *area,
			       size_t size)
{
	ARG_UNUSED(img_index);
	ARG_UNUSED(area);
	ARG_UNUSED(size);
	return 0;
}

int boot_read_swap_state_primary_slot_hook(int image_index,
					   struct boot_swap_state *state)
{
	ARG_UNUSED(image_index);
	ARG_UNUSED(state);
	return BOOT_HOOK_REGULAR;
}

int boot_img_install_stat_hook(int image_index, int slot, int *img_install_stat)
{
	ARG_UNUSED(image_index);
	ARG_UNUSED(slot);
	ARG_UNUSED(img_install_stat);
	return BOOT_HOOK_REGULAR;
}

int boot_reset_request_hook(bool force)
{
	ARG_UNUSED(force);

	/* When an explicit 'os reset' command is received over SMP (e.g.
	 * 'smpmgr os reset'), clear crashloop counter and boot target so the
	 * device boots straight into Main App. (Hardware resets/crashes do NOT
	 * call this hook, preserving the 3 -> 4 RO escalation).
	 */
	const struct device *const bbram_dev =
		DEVICE_DT_GET(DT_NODELABEL(bbram));
	if (device_is_ready(bbram_dev)) {
		reset_bbram_boot_state(bbram_dev);
	}
	return 0;
}

int boot_serial_uploaded_hook(int img_index, const struct flash_area *area,
			      size_t size)
{
	ARG_UNUSED(img_index);
	ARG_UNUSED(area);
	ARG_UNUSED(size);

	/* When a new image upload completes over SMP, reset crashloop counter
	 * to 0 so that the subsequent 'os reset' boots cleanly into the newly
	 * flashed image.
	 */
	const struct device *const bbram_dev =
		DEVICE_DT_GET(DT_NODELABEL(bbram));
	if (device_is_ready(bbram_dev)) {
		reset_bbram_boot_state(bbram_dev);
	}
	return 0;
}

/*
 * Restrict MCUboot's direct-XIP slot selection to slot 0.
 *
 * The RO Bootloader declares two slots: slot 0 = RW Bootloader (0x18000) and
 * slot 1 = App (0x38000). Slot 1 exists only so that RO's flash map covers the
 * App region; it is not reachable over serial recovery (which can only write
 * slot 0) and it must never be *booted* from here. MCUboot's
 * default selection does not know that - find_slot_with_highest_version()
 * simply walks every available slot, so:
 *
 *   - If the RW Bootloader image is invalid (e.g. an upload interrupted
 * halfway), MCUboot falls through to slot 1 and XIP-boots the App directly,
 * skipping the RW Bootloader. The board then looks healthy, but 'reboot
 * bootloader' can never reach the missing RW Bootloader to repair it - RO just
 * lands on the App again.
 *   - Slot selection is otherwise decided by image version. Both images are
 *     currently signed '--version 1.0.0', and a tie keeps the first candidate
 *     (slot 0), so the intended order only holds by accident today.
 *
 * Always selecting slot 0 fixes both: an invalid RW Bootloader now fails
 * boot_go() and drops into RO serial recovery, which is where it can actually
 * be repaired.
 *
 * The hook is called once per attempt from the retry loop in
 * boot_load_and_validate_images(); returning BOOT_SLOT_NONE on the second call
 * terminates that loop instead of retrying slot 0 forever.
 */
int boot_find_next_slot_hook(struct boot_loader_state *state, uint8_t image,
			     enum boot_slot *active_slot)
{
	static bool primary_slot_tried;

	ARG_UNUSED(state);
	ARG_UNUSED(image);

	if (!primary_slot_tried) {
		primary_slot_tried = true;
		*active_slot = BOOT_SLOT_PRIMARY;
		return 0;
	}

	/*
	 * Slot 0 failed validation, so boot_go() is about to fail and MCUboot
	 * will start serial recovery.
	 *
	 * We cannot recover from here, though: boot_go_hook() took the normal
	 * chainload path, which armed EWPR0..6 and froze them, so the RW
	 * Bootloader region we would need to rewrite is write-protected and the
	 * freeze cannot be undone without a reset.
	 *
	 * Force the crashloop counter past the RO threshold and reset. The next
	 * boot takes RO's recovery branch instead, which freezes at EWPR0..2
	 * and leaves 0x18000 writable. That branch clears the counter again, so
	 * this cannot loop.
	 */
#if defined(MAUI_RO_BOOTLOADER)
	const struct device *const bbram_dev =
		DEVICE_DT_GET(DT_NODELABEL(bbram));

	if (device_is_ready(bbram_dev)) {
		uint32_t forced =
			bbram_encode_crashloop(RO_CRASHLOOP_THRESHOLD);

		(void)bbram_write(bbram_dev, BBRAM_OFFSET_CRASHLOOP_CNT,
				  sizeof(forced), (const uint8_t *)&forced);
		sys_arch_reboot(0);
	}
#endif

	/*
	 * No BBRAM to stage the reset through. Fall through to serial recovery
	 * so the App can at least still be reflashed.
	 */
	it82002_prepare_usb_serial_descriptor();
	*active_slot = BOOT_SLOT_NONE;
	return 0;
}

fih_ret boot_go_hook(struct boot_rsp *rsp)
{
	const struct device *const bbram_dev =
		DEVICE_DT_GET(DT_NODELABEL(bbram));
	uint32_t boot_target = 0;
	uint32_t raw_crashloop = 0;
	uint32_t crashloop_cnt = 0;

	ARG_UNUSED(rsp);
	/* NOTE: Do NOT disable ILM mappings here because boot_go() runs after
	 * this hook and relies on flash_it8xxx2_read() (__soc_ram_code in ILM).
	 * Lingering ILM mappings are cleared at EARLY init of the next stage.
	 */

	/*
	 * The whole crashloop escape depends on BBRAM: without it the counter
	 * reads 0 forever and is never written, so a permanently crashing image
	 * would reboot endlessly with no way out. Treat that as a fault and
	 * stop in serial recovery, which is the one state that is always
	 * repairable.
	 */
	if (!device_is_ready(bbram_dev)) {
		it82002_flash_protect_and_lock(EWPR_REGS_RECOVERY);
		it82002_prepare_usb_serial_descriptor();
		FIH_RET(FIH_FAILURE);
	}

#if defined(MAUI_RO_BOOTLOADER)
	/* Must run before any it82002_flash_protect() call can silently no-op.
	 */
	it82002_recover_stale_eplr(bbram_dev);
#endif

	(void)bbram_read(bbram_dev, BBRAM_OFFSET_BOOT_TARGET,
			 sizeof(boot_target), (uint8_t *)&boot_target);
	(void)bbram_read(bbram_dev, BBRAM_OFFSET_CRASHLOOP_CNT,
			 sizeof(raw_crashloop), (uint8_t *)&raw_crashloop);
	crashloop_cnt = bbram_decode_crashloop(raw_crashloop);

#if defined(MAUI_RO_BOOTLOADER)
	bool nvs_triggered = check_nvs_force_ro_fallback();

	if (boot_target == BOOT_TARGET_FORCE_RO || nvs_triggered) {
		reset_bbram_boot_state(bbram_dev);
		/*
		 * RO serial recovery may reflash the RW Bootloader and the App,
		 * but never the RO Bootloader itself. Freeze at EWPR0..2 so
		 * whatever gets uploaded cannot widen its own access.
		 */
		it82002_flash_protect_and_lock(EWPR_REGS_RO_BOOTLOADER);
		it82002_prepare_usb_serial_descriptor();
		FIH_RET(FIH_FAILURE);
	}

	/*
	 * BOOT_TARGET_FORCE_RW_RECOVERY is deliberately NOT special-cased here.
	 * It is consumed by the RW Bootloader, which clears both the target and
	 * the counter when it enters recovery, so the normal path below already
	 * does the right thing for an intentional 'reboot bootloader'.
	 *
	 * Short-circuiting it here used to reset the counter to 0 and return
	 * early, which meant a valid-but-crashing RW Bootloader restarted the
	 * count on every boot and could never escalate to RO recovery.
	 */

	/* Normal boot path: increment crashloop counter in RO Bootloader */
	uint32_t next_cnt = crashloop_cnt + 1;

	if (next_cnt >= RO_CRASHLOOP_THRESHOLD) {
		/* Reached 4 consecutive reboots without OS stabilization -> RO
		 * Recovery! */
		reset_bbram_boot_state(bbram_dev);
		it82002_flash_protect_and_lock(EWPR_REGS_RO_BOOTLOADER);
		it82002_prepare_usb_serial_descriptor();
		FIH_RET(FIH_FAILURE);
	}

	uint32_t encoded_cnt = bbram_encode_crashloop(next_cnt);

	(void)bbram_write(bbram_dev, BBRAM_OFFSET_CRASHLOOP_CNT,
			  sizeof(encoded_cnt), (const uint8_t *)&encoded_cnt);

	/*
	 * Normal chainload. Protect BOTH bootloaders (0x00000 - 0x37FFF) and
	 * freeze, before handing control to the RW Bootloader.
	 *
	 * The RW Bootloader is field-updatable and only hash-checked, so it has
	 * to be treated as untrusted: if it could still reach these registers,
	 * it could clear EWPR0..2 and overwrite the RO Bootloader. Freezing
	 * here is the only point at which that is still under our control.
	 *
	 * The App slot (0x38000+) and the storage partition stay writable, so
	 * serial recovery of the App from either bootloader keeps working.
	 */
	it82002_flash_protect_and_lock(EWPR_REGS_BOTH_LOADERS);

#elif defined(MAUI_RW_BOOTLOADER)
	/*
	 * The RO Bootloader has already armed EWPR0..6 and set EPLR before
	 * chainloading us, so every it82002_flash_protect_and_lock() call below
	 * normally writes to frozen registers and does nothing. They are kept
	 * as a fallback for the one case where RO could not arm them (a stale
	 * EPLR it could not clear), so that protection is at least attempted
	 * again after the next reset.
	 */

	/* Check if explicit RW Serial Recovery was requested */
	if (boot_target == BOOT_TARGET_FORCE_RW_RECOVERY) {
		reset_bbram_boot_state(bbram_dev);
		/*
		 * Serial recovery in the RW Bootloader may only reflash the
		 * Application slot, so keep both bootloaders write-protected.
		 */
		it82002_flash_protect_and_lock(EWPR_REGS_BOTH_LOADERS);
		it82002_prepare_usb_serial_descriptor();
		FIH_RET(FIH_FAILURE);
	}

	/*
	 * Check if crashloop counter (already incremented by RO Bootloader on
	 * this boot) reached RW recovery threshold (3). Leave crashloop_cnt=3
	 * in BBRAM so that if RW Bootloader itself crashes or a 4th reboot
	 * occurs without uploading new firmware, RO Bootloader will increment
	 * 3 -> 4 and enter RO Recovery Bootloader.
	 */
	if (crashloop_cnt >= RW_CRASHLOOP_THRESHOLD) {
		it82002_flash_protect_and_lock(EWPR_REGS_BOTH_LOADERS);
		it82002_prepare_usb_serial_descriptor();
		FIH_RET(FIH_FAILURE);
	}

	it82002_flash_protect_and_lock(EWPR_REGS_BOTH_LOADERS);

#endif

	/* Follow regular boot_go() to validate & jump */
	FIH_RET(FIH_BOOT_HOOK_REGULAR);
}
