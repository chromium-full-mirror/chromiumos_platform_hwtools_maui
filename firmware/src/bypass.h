/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef BYPASS_H_
#define BYPASS_H_

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

#endif /* BYPASS_H_ */
