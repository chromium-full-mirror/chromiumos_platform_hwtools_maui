/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef H2H_FLASH_H_
#define H2H_FLASH_H_

#include <stddef.h>
#include <stdint.h>

#include <sys/types.h>

/**
 * Acquire electrical control of the Prolific PL27A1 H2H external SPI NOR flash.
 * Holds PL27A1 in reset (GPA2 low), deasserts WP#/HOLD# (GPF5/GPF4 high), and
 * switches the SPI multiplexer to the EC (GPC4 high, GPM4 low).
 */
int h2h_spi_acquire(void);

/**
 * Release electrical control of the H2H external SPI NOR flash back to the
 * Prolific PL27A1 and release H2H_RESET_L so PL27A1 boots from SPI flash.
 */
int h2h_spi_release(void);

/**
 * Read data from the H2H external SPI NOR flash.
 */
int h2h_flash_read(off_t offset, void *data, size_t len);

/**
 * Erase sectors on the H2H external SPI NOR flash.
 */
int h2h_flash_erase(off_t offset, size_t size);

/**
 * Write data to the H2H external SPI NOR flash.
 */
int h2h_flash_write(off_t offset, const void *data, size_t len);

#endif /* H2H_FLASH_H_ */
