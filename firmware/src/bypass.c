/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "bypass.h"
#include "gpio.h"
#include "hwrev.h"

#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(bypass);

#define BYPASS_HWREV_MIN_REQUIRED 1

#define USB2_MUX_DUT_SEL_DIRECT 1
#define USB2_MUX_DUT_SEL_H2H 0

#define USB3_MUX_DUT_SEL_DIRECT 0
#define USB3_MUX_DUT_SEL_H2H 1

#define USB2_MUX_HUB_SEL_DIRECT 1
#define USB2_MUX_HUB_SEL_H2H 0

#define USB3_MUX_HUB_SEL_DIRECT 1
#define USB3_MUX_HUB_SEL_H2H 0

void bypass_set_mode(enum bypass_mode_t mode)
{
	if (hwrev_read() < BYPASS_HWREV_MIN_REQUIRED) {
		if (mode != BYPASS_MODE_H2H) {
			LOG_ERR("Bypass not supported on this maui revision");
		}

		return;
	}

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
