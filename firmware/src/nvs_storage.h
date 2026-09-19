/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef NVS_STORAGE_H_
#define NVS_STORAGE_H_

#include <stddef.h>
#include <stdint.h>

#define NVS_SERIAL_MAX_LEN 32
#define NVS_DEFAULT_SERIAL "Uninitialized"

#define NVS_KEY_SERIAL_NUMBER 0x0001
#define NVS_KEY_BOOT_TARGET 0x0002

/**
 * Initialize NVS persistent storage partition (0x000F8000, 32 KB).
 * @return 0 on success, negative errno on failure.
 */
int nvs_storage_init(void);

/**
 * Retrieve persistent device serial number from NVS.
 * Defaults to "Uninitialized" if not yet provisioned.
 */
int nvs_storage_get_serial(char *buf, size_t len);

/**
 * Store persistent device serial number in NVS.
 */
int nvs_storage_set_serial(const char *serial);

/**
 * Write boot target fallback flag to NVS (used prior to ETWD hardware reset).
 */
int nvs_storage_set_boot_target(uint32_t target);

/**
 * Read and clear boot target fallback flag from NVS.
 */
int nvs_storage_get_and_clear_boot_target(uint32_t *target);

#endif /* NVS_STORAGE_H_ */
