/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef HEALTH_H_
#define HEALTH_H_

#include <stdint.h>

/* BBRAM Offsets */
#define BBRAM_OFFSET_BOOT_TARGET 0
#define BBRAM_OFFSET_CRASHLOOP_CNT 4
#define BBRAM_OFFSET_TEST_MAGIC 8

/* Boot Target Magic Constants */
enum boot_target_t {
	BOOT_TARGET_NORMAL = 0x00000000UL,
	BOOT_TARGET_FORCE_RO = 0x524F4254UL, /* 'ROBT' */
	BOOT_TARGET_FORCE_RW_RECOVERY = 0x52574254UL, /* 'RWBT' */
};

#define ETWD_BBRAM_TEST_MAGIC 0xCAFEBABEUL

/**
 * Initialize OS Health & Crashloop Monitoring subsystem.
 * Schedules a 2.0-second delayable work timer that resets the BBRAM
 * crashloop counter once the Main Application is confirmed stable.
 */
int health_init(void);

/**
 * Reboot the EC into a specified boot target (Normal, RW Bootloader Recovery,
 * or RO Root Bootloader Recovery).
 * On IT82002 this triggers an External Timer Watchdog (ETWD) hardware reset.
 * Being an EC-domain reset it clears the EWPRxPF* flash write-protect
 * registers, and - measured on silicon, contradicting the datasheet, which
 * claims a power-on reset is required - it also clears GCTRL_EPLR. That is
 * what makes freezing EPLR safe: any reset returns the chip to a programmable
 * state, so reflashing never needs a power cycle.
 */
int health_reboot_to(enum boot_target_t target);

#endif /* HEALTH_H_ */
