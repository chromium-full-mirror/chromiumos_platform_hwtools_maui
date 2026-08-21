/* Copyright 2025 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "app_version.h"
#include "gpio.h"
#include "led.h"
#include "tps6699x.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#ifdef CONFIG_MAUI_HEALTH
#include "health.h"
#endif
#ifdef CONFIG_MAUI_NVS_STORAGE
#include "nvs_storage.h"
#endif

#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/logging/log.h>
#include <zephyr/logging/log_ctrl.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

#if defined(CONFIG_SOC_FAMILY_TI_MSPM0) || defined(CONFIG_SOC_MSPM0G3507) || \
	defined(CONFIG_SOC_MSPM0G5117)
#include <ti/driverlib/m0p/dl_sysctl.h>
#endif

#if DT_HAS_COMPAT_STATUS_OKAY(ti_tps6699x)
/**
 * Handles the power_dut command which controls the power to DUT using PDC.
 */
static int cmd_power_dut_handler(const struct shell *sh, size_t argc,
				 char **argv)
{
	const struct device *pdc_dev = DEVICE_DT_GET_ONE(ti_tps6699x);
	int rv;

	if (!device_is_ready(pdc_dev)) {
		shell_error(sh, "PDC device not ready");
		return -ENODEV;
	}

	if (argc < 2) {
		shell_error(sh, "Usage: power_dut <on|off|cycle>");
		return -EINVAL;
	}

	if (strcmp(argv[1], "on") == 0) {
		/* Use GAID to recover from a previous permanent disconnect
		 * (DISC 0) or just to ensure port is enabled.
		 */
		rv = tps_cmd_gaid(pdc_dev, false);
		if (rv) {
			shell_error(sh, "Failed to enable DUT power: %d", rv);
			return rv;
		}
		shell_print(sh, "DUT Power: ON (PDC Reset)");
	} else if (strcmp(argv[1], "off") == 0) {
		/* DISC 0: Permanent disconnect (Simulate Unplug) */
		rv = tps_cmd_disc(pdc_dev, 0);
		if (rv) {
			shell_error(sh, "Failed to cut DUT power: %d", rv);
			return rv;
		}
		shell_print(sh, "DUT Power: OFF (PDC Disconnect)");
	} else if (strcmp(argv[1], "cycle") == 0) {
		/* DISC 1: Disconnect for 1 second then reconnect */
		shell_print(sh, "DUT Power: CYCLING...");
		rv = tps_cmd_disc(pdc_dev, 1);
		if (rv) {
			shell_error(sh, "Failed to cycle DUT power: %d", rv);
			return rv;
		}
		shell_print(sh, "DUT Power: Cycling started (1s delay)");
	} else {
		shell_error(sh, "Invalid argument: %s", argv[1]);
		return -EINVAL;
	}

	return 0;
}
#endif
/**
 * Handles the data_dut command which controls the USB data muxes to DUT.
 */
static int cmd_data_dut_handler(const struct shell *sh, size_t argc,
				char **argv)
{
	if (argc < 2) {
		shell_error(sh, "Usage: data_dut <on|off|cycle>");
		return -EINVAL;
	}

	if (strcmp(argv[1], "on") == 0) {
		/* Bring H2H Bridge out of reset
		 * and give it some time to settle up before DUT reconnection
		 */
		gpio_set(GPIO_H2H_RESET_L, 0);
		k_sleep(K_MSEC(300));
		/* Logic 1 = Active (Low physically for OE_L) */
		gpio_set(GPIO_USB3_MUX_DUT_OE_R_L, 1);
		gpio_set(GPIO_USB2_MUX_DUT_OE_R_L, 1);
		shell_print(sh, "DUT Data: ON (CONNECTED)");
	} else if (strcmp(argv[1], "off") == 0) {
		/* Put H2H Bridge into reset.
		 * For some reasons we see instabilities without this reset.
		 */
		gpio_set(GPIO_H2H_RESET_L, 1);
		/* Logic 0 = Inactive (High physically for OE_L) */
		gpio_set(GPIO_USB3_MUX_DUT_OE_R_L, 0);
		gpio_set(GPIO_USB2_MUX_DUT_OE_R_L, 0);
		shell_print(sh, "DUT Data: OFF (DISCONNECTED)");
	} else if (strcmp(argv[1], "cycle") == 0) {
		shell_print(sh, "DUT Data: Cycling started (1s delay)");
		gpio_set(GPIO_H2H_RESET_L, 1);
		gpio_set(GPIO_USB3_MUX_DUT_OE_R_L, 0);
		gpio_set(GPIO_USB2_MUX_DUT_OE_R_L, 0);
		k_sleep(K_MSEC(1000));

		/* Bring H2H Bridge out of reset
		 * and give it some time to settle up before DUT reconnection
		 */
		gpio_set(GPIO_H2H_RESET_L, 0);
		k_sleep(K_MSEC(300));

		gpio_set(GPIO_USB3_MUX_DUT_OE_R_L, 1);
		gpio_set(GPIO_USB2_MUX_DUT_OE_R_L, 1);
		shell_print(sh, "DUT Data: ON (CONNECTED)");
	} else {
		shell_error(sh, "Invalid argument: %s", argv[1]);
		return -EINVAL;
	}

	return 0;
}

/**
 * Handles the command that jumps from main code to BSL / Bootloader.
 */
static int cmd_jump_to_bsl_handler(const struct shell *sh, size_t argc,
				   char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

#if defined(CONFIG_SOC_FAMILY_TI_MSPM0) || defined(CONFIG_SOC_MSPM0G3507) || \
	defined(CONFIG_SOC_MSPM0G5117)
	shell_print(sh, "Going into BSL mode");

	/* Erase SRAM completely before jumping to BSL */
	__asm__ volatile(
#if defined(__GNUC__)
		".syntax unified\n" /* Load SRAMFLASH register*/
#endif
		"ldr     r4, = 0x41C40018\n" /* Load SRAMFLASH register*/
		"ldr     r4, [r4]\n"
		"ldr     r1, = 0x03FF0000\n" /* SRAMFLASH.SRAM_SZ mask */
		"ands    r4, r1\n" /* Get SRAMFLASH.SRAM_SZ */
		"lsrs    r4, r4, #6\n" /* SRAMFLASH.SRAM_SZ to kB */
		"ldr     r1, = 0x20300000\n" /* Start of ECC-code */
		"adds    r2, r4, r1\n" /* End of ECC-code */
		"movs    r3, #0\n"
		"init_ecc_loop: \n" /* Loop to clear ECC-code */
		"str     r3, [r1]\n"
		"adds    r1, r1, #4\n"
		"cmp     r1, r2\n"
		"blo     init_ecc_loop\n"
		"ldr     r1, = 0x20200000\n" /* Start of NON-ECC-data */
		"adds    r2, r4, r1\n" /* End of NON-ECC-data */
		"movs    r3, #0\n"
		"init_data_loop:\n" /* Loop to clear ECC-data */
		"str     r3, [r1]\n"
		"adds    r1, r1, #4\n"
		"cmp     r1, r2\n"
		"blo     init_data_loop\n"
		/* Force a reset calling BSL after clearing SRAM */
		"str     %[resetLvlVal], [%[resetLvlAddr], #0x00]\n"
		"str     %[resetCmdVal], [%[resetCmdAddr], #0x00]\n"
		"b       .\n"
		: /* No outputs */
		: [resetLvlAddr] "r"(&SYSCTL->SOCLOCK.RESETLEVEL),
		  [resetLvlVal] "r"(DL_SYSCTL_RESET_BOOTLOADER_ENTRY),
		  [resetCmdAddr] "r"(&SYSCTL->SOCLOCK.RESETCMD),
		  [resetCmdVal] "r"(SYSCTL_RESETCMD_KEY_VALUE |
				    SYSCTL_RESETCMD_GO_TRUE)
		: "r1", "r2", "r3", "r4");
	return 0;
#else
	shell_print(sh, "Rebooting into RW MCUboot Bootloader...");
	k_msleep(50);
	return health_reboot_to(BOOT_TARGET_FORCE_RW_RECOVERY);
#endif
}

/**
 * Handles the version command which prints the firmware version
 */
static int cmd_version_handler(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	/*
	 * CONFIG_FLASH_LOAD_OFFSET is the offset of whichever partition this
	 * image was linked into (0 standalone, slot0 under MCUboot), and
	 * CONFIG_ROM_START_OFFSET covers the MCUboot image header. Deriving the
	 * address keeps this honest if the partition table ever moves.
	 */
	const unsigned long run_addr = DT_REG_ADDR(DT_CHOSEN(zephyr_flash)) +
				       CONFIG_FLASH_LOAD_OFFSET +
				       CONFIG_ROM_START_OFFSET;
#if IS_ENABLED(CONFIG_BOOTLOADER_MCUBOOT)
	const char *mode = "MCUBOOT-SLOT0";
#else
	const char *mode = "STANDALONE";
#endif

	shell_print(sh, "Maui version %s [%s (0x%08lx)]", APP_VERSION_STRING,
		    mode, run_addr);

	return 0;
}

static int cmd_reboot_handler(const struct shell *sh, size_t argc, char **argv)
{
#ifdef CONFIG_MAUI_HEALTH
	/*
	 * Only boards with the 3-stage bootloader chain can be steered to a
	 * particular stage; elsewhere every form of this command is a plain
	 * cold reboot.
	 */
	if (argc > 1) {
		if (strcmp(argv[1], "bootloader") == 0) {
			shell_print(sh,
				    "Rebooting into RW MCUboot Bootloader...");
			k_msleep(50);
			return health_reboot_to(BOOT_TARGET_FORCE_RW_RECOVERY);
		} else if (strcmp(argv[1], "ro-bootloader") == 0 ||
			   strcmp(argv[1], "ro") == 0) {
			shell_print(sh,
				    "Rebooting into RO Recovery Bootloader...");
			k_msleep(50);
			return health_reboot_to(BOOT_TARGET_FORCE_RO);
		}
	}

	shell_print(sh, "Resetting device...\n");
	k_msleep(50);
	return health_reboot_to(BOOT_TARGET_NORMAL);
#else
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Resetting device...\n");
	k_msleep(50);
	sys_reboot(SYS_REBOOT_COLD);

	return 0;
#endif
}

static int cmd_reset_handler(const struct shell *shell, size_t argc,
			     char **argv)
{
	return cmd_reboot_handler(shell, argc, argv);
}

#ifdef CONFIG_MAUI_NVS_STORAGE
static int cmd_serial_get_handler(const struct shell *sh, size_t argc,
				  char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);
	char sn[NVS_SERIAL_MAX_LEN] = { 0 };
	int rc;

	rc = nvs_storage_get_serial(sn, sizeof(sn));
	if (rc) {
		shell_error(sh, "Failed to read serial number (%d)", rc);
		return rc;
	}

	shell_print(sh, "Serial Number: %s", sn);
	return 0;
}

static int cmd_serial_set_handler(const struct shell *sh, size_t argc,
				  char **argv)
{
	if (argc < 2) {
		shell_error(sh, "Usage: serial set <SERIAL_NUMBER>");
		return -EINVAL;
	}

	int rc = nvs_storage_set_serial(argv[1]);
	if (rc) {
		shell_error(sh, "Failed to set serial number: %d", rc);
		return rc;
	}

	shell_print(
		sh,
		"Serial Number updated to: %s (reboot required for USB descriptor update)",
		argv[1]);
	return 0;
}
#endif /* CONFIG_MAUI_NVS_STORAGE */

static int cmd_log_exclusive_handler(const struct shell *sh, size_t argc,
				     char **argv)
{
	int sources = log_src_cnt_get(0);

	if (strcmp(argv[1], "reset") == 0) {
		for (int i = 0; i < sources; i++) {
			uint32_t level = log_filter_get(NULL, 0, i, false);
			log_filter_set(NULL, 0, i, level);
		}
		shell_print(sh, "Reset all modules to default log levels");
		return 0;
	}

	if (strcmp(argv[1], "status") == 0) {
		shell_print(sh, "Current log levels:");
		for (int i = 0; i < sources; i++) {
			const char *name = log_source_name_get(0, i);
			uint32_t level = log_filter_get(NULL, 0, i, true);
			shell_print(sh, "  %-20s: %d", name, level);
		}
		return 0;
	}

	/* Exclusive mode: Validate all specified modules first */
	for (int j = 1; j < argc; j++) {
		bool found = false;

		for (int i = 0; i < sources; i++) {
			if (strcmp(log_source_name_get(0, i), argv[j]) == 0) {
				found = true;
				break;
			}
		}

		if (!found) {
			shell_error(sh, "Module '%s' not found", argv[j]);
			return -EINVAL;
		}
	}

	/* All modules validated, apply exclusive logging */
	for (int i = 0; i < sources; i++) {
		const char *name = log_source_name_get(0, i);
		bool enable = false;

		/* Check if this module is in the argument list */
		for (int j = 1; j < argc; j++) {
			if (strcmp(name, argv[j]) == 0) {
				enable = true;
				break;
			}
		}

		if (enable) {
			uint32_t level = log_filter_get(NULL, 0, i, false);
			log_filter_set(NULL, 0, i, level);
		} else {
			log_filter_set(NULL, 0, i, LOG_LEVEL_NONE);
		}
	}

	shell_print(sh, "Exclusive logging enabled for:");
	for (int j = 1; j < argc; j++) {
		shell_print(sh, "  - %s", argv[j]);
	}

	return 0;
}

#ifdef CONFIG_MAUI_NVS_STORAGE
SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_serial,
	SHELL_CMD(get, NULL, "Get persistent serial number",
		  cmd_serial_get_handler),
	SHELL_CMD_ARG(set, NULL, "Set persistent serial number <SERIAL_NUMBER>",
		      cmd_serial_set_handler, 2, 0),
	SHELL_SUBCMD_SET_END);
#endif

static int cmd_led_handler(const struct shell *sh, size_t argc, char **argv)
{
	if (argc < 2) {
		shell_error(
			sh,
			"Usage: led <green|amber> <on|off|blink|auto> | status | reset");
		return -EINVAL;
	}

	if (strcmp(argv[1], "status") == 0) {
		shell_print(sh, "LED Status:");
		shell_print(sh, "  Green: pattern=%s, overridden=%s",
			    led_pattern_to_str(led_get_pattern(LED_GREEN)),
			    led_is_overridden(LED_GREEN) ? "Yes" : "No");
		shell_print(sh, "  Amber: pattern=%s, overridden=%s",
			    led_pattern_to_str(led_get_pattern(LED_AMBER)),
			    led_is_overridden(LED_AMBER) ? "Yes" : "No");
		return 0;
	}

	if (strcmp(argv[1], "reset") == 0) {
		led_clear_override(LED_GREEN);
		led_clear_override(LED_AMBER);
		shell_print(sh, "LED overrides reset to automatic control");
		return 0;
	}

	enum led_id id;
	if (strcmp(argv[1], "green") == 0) {
		id = LED_GREEN;
	} else if (strcmp(argv[1], "amber") == 0) {
		id = LED_AMBER;
	} else {
		shell_error(sh, "Invalid LED: '%s' (must be green or amber)",
			    argv[1]);
		return -EINVAL;
	}

	if (argc < 3) {
		shell_error(sh, "Usage: led %s <on|off|blink|auto>", argv[1]);
		return -EINVAL;
	}

	if (strcmp(argv[2], "on") == 0) {
		led_set_override(id, LED_OVERRIDE_ON);
		shell_print(sh, "%s LED forced ON", argv[1]);
	} else if (strcmp(argv[2], "off") == 0) {
		led_set_override(id, LED_OVERRIDE_OFF);
		shell_print(sh, "%s LED forced OFF", argv[1]);
	} else if (strcmp(argv[2], "blink") == 0) {
		led_set_override(id, LED_OVERRIDE_BLINK);
		shell_print(sh, "%s LED forced BLINK", argv[1]);
	} else if (strcmp(argv[2], "auto") == 0 ||
		   strcmp(argv[2], "reset") == 0) {
		led_clear_override(id);
		shell_print(sh, "%s LED returned to automatic control",
			    argv[1]);
	} else {
		shell_error(
			sh,
			"Invalid action: '%s' (must be on, off, blink, or auto)",
			argv[2]);
		return -EINVAL;
	}

	return 0;
}

SHELL_CMD_REGISTER(reset, NULL, "Reset the device", cmd_reset_handler);
SHELL_CMD_ARG_REGISTER(reboot, NULL,
		       "Reboot the device [bootloader|ro-bootloader]",
		       cmd_reboot_handler, 1, 1);
SHELL_CMD_REGISTER(version, NULL, "Prints firmware version",
		   cmd_version_handler);
#ifdef CONFIG_MAUI_NVS_STORAGE
SHELL_CMD_REGISTER(serial, &sub_serial, "Manage persistent serial number",
		   NULL);
#endif
SHELL_CMD_REGISTER(bsl, NULL, "Invokes BSL / Bootloader",
		   cmd_jump_to_bsl_handler);
SHELL_CMD_ARG_REGISTER(
	log_exclusive, NULL,
	"Log only specific modules, reset all, or show status.\n"
	"Usage: log_exclusive <module1> [module2] ... | reset | status",
	cmd_log_exclusive_handler, 2, 10);
SHELL_CMD_ARG_REGISTER(
	led, NULL,
	"Control LEDs: led <green|amber> <on|off|blink|auto> | status | reset",
	cmd_led_handler, 2, 1);
#if DT_HAS_COMPAT_STATUS_OKAY(ti_tps6699x)
SHELL_CMD_REGISTER(power_dut, NULL, "Control DUT power <on|off|cycle>",
		   cmd_power_dut_handler);
#endif
SHELL_CMD_REGISTER(data_dut, NULL, "Control DUT data <on|off|cycle>",
		   cmd_data_dut_handler);
