/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "bypass.h"
#include "gpio.h"
#include "h2h.h"
#include "hwrev.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bypass);

#define BYPASS_HWREV_MIN_REQUIRED 1

static K_MUTEX_DEFINE(bypass_lock);
#ifdef CONFIG_BOARD_MAUI_V2P0_IT82002
static enum bypass_mode_t bypass_requested_mode = BYPASS_MODE_H2H;
static bool bypass_dut_flash_override;
#endif

#ifdef CONFIG_BOARD_MAUI_V2P0_IT82002
/*
 * On Maui v2.0, U0811 (USB2) and U0812 (USB3) are the Stage-1 DUT bypass muxes
 * routing the DUT either directly to Hub DN3 (Direct Device Mode) or onward to
 * Stage-2 U9/U10 (H2H / USB-C flash). U9/U10 and U1/U18 are owned by
 * pendrive.c.
 */
#define USB2_DUT_HUB_SEL_DIRECT 0
#define USB2_DUT_HUB_SEL_H2H 1

#define USB3_DUT_HUB_SEL_DIRECT 1
#define USB3_DUT_HUB_SEL_H2H 0

static void bypass_route_muxes(enum bypass_mode_t mode)
{
	int output_enable = (mode != BYPASS_MODE_DISCONNECTED);

	if (mode == BYPASS_MODE_DIRECT) {
		gpio_set(GPIO_USB2_DUT_HUB_SEL, USB2_DUT_HUB_SEL_DIRECT);
		gpio_set(GPIO_USB3_DUT_HUB_SEL, USB3_DUT_HUB_SEL_DIRECT);

		LOG_INF("Mux set to Direct Device Mode");
	} else if (mode == BYPASS_MODE_H2H) {
		gpio_set(GPIO_USB2_DUT_HUB_SEL, USB2_DUT_HUB_SEL_H2H);
		gpio_set(GPIO_USB3_DUT_HUB_SEL, USB3_DUT_HUB_SEL_H2H);

		LOG_INF("Mux set to Host-to-Host Bridge Mode");
	} else {
		LOG_INF("Mux disconnected");
	}

	gpio_set(GPIO_USB2_DUT_HUB_OE_L, output_enable);
	gpio_set(GPIO_USB3_DUT_HUB_OE_L, output_enable);
}
#else
#define USB2_MUX_DUT_SEL_DIRECT 1
#define USB2_MUX_DUT_SEL_H2H 0

#define USB3_MUX_DUT_SEL_DIRECT 0
#define USB3_MUX_DUT_SEL_H2H 1

#define USB2_MUX_HUB_SEL_DIRECT 1
#define USB2_MUX_HUB_SEL_H2H 0

#define USB3_MUX_HUB_SEL_DIRECT 1
#define USB3_MUX_HUB_SEL_H2H 0

static void bypass_route_muxes(enum bypass_mode_t mode)
{
	int output_enable = (mode != BYPASS_MODE_DISCONNECTED);

	if (mode == BYPASS_MODE_DIRECT) {
		gpio_set(GPIO_USB2_MUX_DUT_SEL_R, USB2_MUX_DUT_SEL_DIRECT);
		gpio_set(GPIO_USB3_MUX_DUT_SEL_R, USB3_MUX_DUT_SEL_DIRECT);

		gpio_set(GPIO_USB2_MUX_HUB_SEL, USB2_MUX_HUB_SEL_DIRECT);
		gpio_set(GPIO_USB3_MUX_HUB_SEL, USB3_MUX_HUB_SEL_DIRECT);

		LOG_INF("Mux set to Direct Device Mode");
	} else if (mode == BYPASS_MODE_H2H) {
		gpio_set(GPIO_USB2_MUX_DUT_SEL_R, USB2_MUX_DUT_SEL_H2H);
		gpio_set(GPIO_USB3_MUX_DUT_SEL_R, USB3_MUX_DUT_SEL_H2H);

		gpio_set(GPIO_USB2_MUX_HUB_SEL, USB2_MUX_HUB_SEL_H2H);
		gpio_set(GPIO_USB3_MUX_HUB_SEL, USB3_MUX_HUB_SEL_H2H);

		LOG_INF("Mux set to Host-to-Host Bridge Mode");
	} else {
		LOG_INF("Mux disconnected");
	}

	gpio_set(GPIO_USB2_MUX_DUT_OE_R_L, output_enable);
	gpio_set(GPIO_USB3_MUX_DUT_OE_R_L, output_enable);
	gpio_set(GPIO_USB2_MUX_HUB_OE_L, output_enable);
	gpio_set(GPIO_USB3_MUX_HUB_OE_L, output_enable);
}
#endif

void bypass_set_mode(enum bypass_mode_t mode)
{
	if (hwrev_read() < BYPASS_HWREV_MIN_REQUIRED) {
		if (mode != BYPASS_MODE_H2H) {
			LOG_ERR("Bypass not supported on this maui revision");
		}

		return;
	}

	k_mutex_lock(&bypass_lock, K_FOREVER);

#ifdef CONFIG_BOARD_MAUI_V2P0_IT82002
	bypass_requested_mode = mode;
#endif

	if (mode != BYPASS_MODE_H2H) {
		/*
		 * Hold Prolific PL27A1 in reset BEFORE switching muxes away
		 * from it so the bridge stays cleanly off USB while unused.
		 */
		(void)h2h_reset_hold(H2H_RESET_CLIENT_BYPASS);
	}

#ifdef CONFIG_BOARD_MAUI_V2P0_IT82002
	if (bypass_dut_flash_override) {
		/*
		 * Keep Stage-1 U0811/U0812 routed onward to Stage-2 U9/U10
		 * while the pendrive-to-DUT path is active.
		 */
		gpio_set(GPIO_USB2_DUT_HUB_SEL, USB2_DUT_HUB_SEL_H2H);
		gpio_set(GPIO_USB3_DUT_HUB_SEL, USB3_DUT_HUB_SEL_H2H);
		gpio_set(GPIO_USB2_DUT_HUB_OE_L, 1);
		gpio_set(GPIO_USB3_DUT_HUB_OE_L, 1);
		if (mode != BYPASS_MODE_H2H) {
			LOG_INF("Bypass mode %d deferred while pendrive DUT override is active",
				mode);
		}
	} else {
		bypass_route_muxes(mode);
	}
#else
	bypass_route_muxes(mode);
#endif

	if (mode == BYPASS_MODE_H2H) {
		/*
		 * Release our reset hold AFTER switching muxes back to H2H.
		 * h2h_reset_release() keeps PL27A1 in reset if another client
		 * still holds it, and is a no-op when already running.
		 */
		(void)h2h_reset_release(H2H_RESET_CLIENT_BYPASS);
	}

	k_mutex_unlock(&bypass_lock);
}

void bypass_set_dut_flash_override(bool active)
{
#ifdef CONFIG_BOARD_MAUI_V2P0_IT82002
	k_mutex_lock(&bypass_lock, K_FOREVER);

	if (bypass_dut_flash_override == active) {
		k_mutex_unlock(&bypass_lock);
		return;
	}

	bypass_dut_flash_override = active;

	if (active) {
		if (bypass_requested_mode != BYPASS_MODE_H2H) {
			gpio_set(GPIO_USB2_DUT_HUB_SEL, USB2_DUT_HUB_SEL_H2H);
			gpio_set(GPIO_USB3_DUT_HUB_SEL, USB3_DUT_HUB_SEL_H2H);
			gpio_set(GPIO_USB2_DUT_HUB_OE_L, 1);
			gpio_set(GPIO_USB3_DUT_HUB_OE_L, 1);
			LOG_INF("Stage-1 DUT muxes overridden to U9/U10 for pendrive");
		}
	} else {
		if (bypass_requested_mode != BYPASS_MODE_H2H) {
			bypass_route_muxes(bypass_requested_mode);
		}
	}

	k_mutex_unlock(&bypass_lock);
#else
	ARG_UNUSED(active);
#endif
}
