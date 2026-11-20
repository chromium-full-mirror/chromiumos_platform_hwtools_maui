/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "gpio.h"
#include "h2h.h"

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/util.h>

LOG_MODULE_REGISTER(h2h);

#define H2H_RESET_SETTLE_MS 10

BUILD_ASSERT(H2H_RESET_CLIENT_COUNT <= 32,
	     "h2h_reset_hold_mask holds at most 32 clients");

static const char *const h2h_client_names[H2H_RESET_CLIENT_COUNT] = {
	[H2H_RESET_CLIENT_BYPASS] = "bypass",
	[H2H_RESET_CLIENT_PENDRIVE] = "pendrive",
	[H2H_RESET_CLIENT_FLASH] = "flash",
	[H2H_RESET_CLIENT_DATA_DUT] = "data_dut",
};

static K_MUTEX_DEFINE(h2h_reset_lock);
static uint32_t h2h_reset_hold_mask;

int h2h_reset_hold(enum h2h_reset_client client)
{
	int rc;
	bool newly_held;

	if ((unsigned int)client >= H2H_RESET_CLIENT_COUNT) {
		return -EINVAL;
	}

	k_mutex_lock(&h2h_reset_lock, K_FOREVER);

	newly_held = (h2h_reset_hold_mask & BIT(client)) == 0;
	rc = gpio_set(GPIO_H2H_RESET_L, 1);
	if (rc == 0) {
		h2h_reset_hold_mask |= BIT(client);
		if (newly_held) {
			LOG_INF("H2H reset held by %s (mask=0x%x)",
				h2h_client_names[client],
				(unsigned int)h2h_reset_hold_mask);
		}
	} else {
		LOG_ERR("Failed to hold H2H reset for %s (%d)",
			h2h_client_names[client], rc);
	}

	k_mutex_unlock(&h2h_reset_lock);
	return rc;
}

int h2h_reset_release(enum h2h_reset_client client)
{
	int rc = 0;
	bool had_hold;

	if ((unsigned int)client >= H2H_RESET_CLIENT_COUNT) {
		return -EINVAL;
	}

	k_mutex_lock(&h2h_reset_lock, K_FOREVER);

	had_hold = (h2h_reset_hold_mask & BIT(client)) != 0;
	h2h_reset_hold_mask &= ~BIT(client);

	if (h2h_reset_hold_mask != 0) {
		if (had_hold) {
			LOG_INF("H2H reset released by %s, still held by others (mask=0x%x)",
				h2h_client_names[client],
				(unsigned int)h2h_reset_hold_mask);
		}
		k_mutex_unlock(&h2h_reset_lock);
		return 0;
	}

	if (gpio_get(GPIO_H2H_RESET_L) > 0) {
		/*
		 * Wait inside the mutex for mux settling and minimum RESETB
		 * pulse width so a concurrent h2h_reset_hold() cannot race
		 * with releasing the pin.
		 */
		k_msleep(H2H_RESET_SETTLE_MS);
		rc = gpio_set(GPIO_H2H_RESET_L, 0);
		if (rc == 0) {
			LOG_INF("H2H reset released by %s (PL27A1 running)",
				h2h_client_names[client]);
		} else {
			LOG_ERR("Failed to release H2H reset for %s (%d)",
				h2h_client_names[client], rc);
		}
	}

	k_mutex_unlock(&h2h_reset_lock);
	return rc;
}

uint32_t h2h_reset_holders(void)
{
	uint32_t mask;

	k_mutex_lock(&h2h_reset_lock, K_FOREVER);
	mask = h2h_reset_hold_mask;
	k_mutex_unlock(&h2h_reset_lock);
	return mask;
}

bool h2h_reset_is_held(void)
{
	return h2h_reset_holders() != 0;
}
