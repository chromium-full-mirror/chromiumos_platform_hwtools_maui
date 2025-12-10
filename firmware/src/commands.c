/* Copyright 2025 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "app_version.h"
#include <zephyr/drivers/gpio.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/reboot.h>

#include <ti/driverlib/m0p/dl_sysctl.h>

/**
 * Handles the command that jumps from main code to BSL.
 */
static int cmd_jump_to_bsl_handler(const struct shell *sh, size_t argc,
				   char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Going into BSL mode");

	/* Erase SRAM completely before jumping to BSL */
	__asm(
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
		"str     %[resetCmdVal], [%[resetCmdAddr], #0x00]"
		: /* No outputs */
		: [resetLvlAddr] "r"(&SYSCTL->SOCLOCK.RESETLEVEL),
		  [resetLvlVal] "r"(DL_SYSCTL_RESET_BOOTLOADER_ENTRY),
		  [resetCmdAddr] "r"(&SYSCTL->SOCLOCK.RESETCMD),
		  [resetCmdVal] "r"(SYSCTL_RESETCMD_KEY_VALUE |
				    SYSCTL_RESETCMD_GO_TRUE)
		: "r1", "r2", "r3", "r4");
	return 0;
}

/**
 * Handles the version command which prints the Dolos firmware version
 */
static int cmd_version_handler(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "Maui version %s", APP_VERSION_STRING);

	return 0;
}

static int cmd_reset_handler(const struct shell *shell, size_t argc,
			     char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(shell, "Resetting device...\n");

	// Delay to ensure the message is printed
	k_sleep(K_SECONDS(1));

	sys_reboot(SYS_REBOOT_COLD);

	return 0;
}

SHELL_CMD_REGISTER(reset, NULL, "Reset the device", cmd_reset_handler);
SHELL_CMD_REGISTER(version, NULL, "Prints firmware version", cmd_version_handler);
SHELL_CMD_REGISTER(bsl, NULL, "Invokes BSL", cmd_jump_to_bsl_handler);
