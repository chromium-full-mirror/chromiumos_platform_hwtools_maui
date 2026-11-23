/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef BYPASS_H_
#define BYPASS_H_

#include <stdbool.h>

enum bypass_mode_t {
	/// Disable the output-enable
	BYPASS_MODE_DISCONNECTED,
	/// Bypass the H2H chip and enable direct connection
	BYPASS_MODE_DIRECT,
	/// Connect devices through H2H chip
	BYPASS_MODE_H2H,
};

/**
 * Change bypass direction on supported revisions.
 * On unsupported revisions, it keeps silent for h2h mode
 * or prints error message for other modes.
 *
 * @param mode Mode to be set for bypass
 */
void bypass_set_mode(enum bypass_mode_t mode);

/**
 * Temporarily override Maui v2.0 Stage-1 DUT muxes (U0811/U0812) away from
 * Direct Device Mode (Hub DN3) onto Stage-2 (U9/U10) so the USB-C pendrive
 * path can reach the DUT, and restore the requested bypass mode when
 * cleared. No-op on revisions without Stage-1 muxes.
 *
 * @param active True to route U0811/U0812 to Stage-2; false to restore.
 */
void bypass_set_dut_flash_override(bool active);

#endif /* BYPASS_H_ */
