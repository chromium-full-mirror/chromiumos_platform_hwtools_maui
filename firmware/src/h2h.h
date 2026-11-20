/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef H2H_H_
#define H2H_H_

#include <stdbool.h>
#include <stdint.h>

/**
 * Clients that may independently request holding the Prolific PL27A1 H2H
 * bridge in reset (active-low H2H_RESET_L).
 */
enum h2h_reset_client {
	H2H_RESET_CLIENT_BYPASS = 0,
	H2H_RESET_CLIENT_PENDRIVE,
	H2H_RESET_CLIENT_FLASH,
	H2H_RESET_CLIENT_DATA_DUT,
	H2H_RESET_CLIENT_COUNT,
};

/**
 * Hold the Prolific PL27A1 H2H bridge in reset on behalf of @p client.
 *
 * Safe to call repeatedly by the same client (idempotent). The bridge stays in
 * reset until every holding client has called h2h_reset_release().
 *
 * @param client Reset-hold client identifier.
 * @return 0 on success, negative errno on failure.
 */
int h2h_reset_hold(enum h2h_reset_client client);

/**
 * Release @p client's reset hold on the Prolific PL27A1 H2H bridge.
 *
 * Only deasserts H2H_RESET_L once no clients remain in the hold mask.
 * When deasserting an active reset, sleeps briefly for mux settling / minimum
 * RESETB pulse width before releasing the pin. When the bridge is already
 * running (H2H_RESET_L inactive), returns immediately with no delay.
 *
 * @param client Reset-hold client identifier.
 * @return 0 on success, negative errno on failure.
 */
int h2h_reset_release(enum h2h_reset_client client);

/**
 * Return the bitmask of clients currently holding PL27A1 in reset.
 */
uint32_t h2h_reset_holders(void);

/**
 * Return true if any client currently holds PL27A1 in reset.
 */
bool h2h_reset_is_held(void);

#endif /* H2H_H_ */
