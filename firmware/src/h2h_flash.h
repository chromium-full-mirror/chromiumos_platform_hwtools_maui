/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef H2H_FLASH_H_
#define H2H_FLASH_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <sys/types.h>

/**
 * Progress callback: @p done of @p total bytes processed. Return 0 to go on,
 * or a negative errno to stop the operation, which then returns that value.
 */
typedef int (*h2h_progress_cb)(uint32_t done, uint32_t total, void *user_data);

/** Who controls the H2H SPI flash bus. */
enum h2h_owner {
	H2H_OWNER_NONE,
	/* 'h2h_flash' shell commands (bring-up and debugging). */
	H2H_OWNER_SHELL,
	/* RPC flashing session (maui-flash). */
	H2H_OWNER_RPC,
};

/**
 * Take electrical control of the Prolific PL27A1 H2H SPI NOR flash for
 * @p who: hold the PL27A1 in reset, deassert WP#/HOLD# and switch the SPI mux
 * to the EC. Claiming again as the current owner is a no-op.
 *
 * @return 0, or -EBUSY if another owner holds the bus.
 */
int h2h_bus_claim(enum h2h_owner who);

/**
 * Hand the flash back to the PL27A1 (mux to PL27A1, release its reset) if
 * @p who owns it. No-op if nobody owns it.
 *
 * @return 0, or -EBUSY if another owner holds the bus.
 */
int h2h_bus_release(enum h2h_owner who);

/** Current owner of the H2H SPI flash bus. */
enum h2h_owner h2h_bus_owner(void);

/** Read the 3-byte JEDEC ID (manufacturer, type, capacity). */
int h2h_flash_read_id(uint8_t id[3]);

/**
 * Decode the flash size from a JEDEC ID.
 * @return 0, -ENODEV if nothing answered, -ENOTSUP for unknown/too big parts.
 */
int h2h_flash_size_from_id(const uint8_t id[3], uint32_t *size);

/**
 * Clear status-register block protection (BP/TB/SRWD) if set.
 * @return 0, or -EACCES if the bits did not clear (e.g. hardware WP#).
 */
int h2h_flash_unprotect(void);

/**
 * Read data from the H2H external SPI NOR flash.
 */
int h2h_flash_read(off_t offset, void *data, size_t len);

/**
 * Erase sectors on the H2H external SPI NOR flash. Offset and size must be
 * 4 KB aligned; 64 KB block erase is used wherever alignment allows.
 */
int h2h_flash_erase(off_t offset, size_t size);

/** h2h_flash_erase() with a progress callback (may be NULL). */
int h2h_flash_erase_cb(off_t offset, size_t size, h2h_progress_cb cb,
		       void *user_data);

/**
 * Write data to the H2H external SPI NOR flash. The range must already be
 * erased. Pages that are entirely 0xFF are skipped.
 */
int h2h_flash_write(off_t offset, const void *data, size_t len);

/**
 * CRC-32 (IEEE 802.3, same as zlib.crc32) of a flash range, with an optional
 * progress callback.
 */
int h2h_flash_crc32_cb(off_t offset, size_t len, uint32_t *crc,
		       h2h_progress_cb cb, void *user_data);

#endif /* H2H_FLASH_H_ */
