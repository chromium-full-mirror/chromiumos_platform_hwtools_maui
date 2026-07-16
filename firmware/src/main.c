/* Copyright 2025 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "adc.h"
#include "led.h"
#include "usb_init.h"

#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main, LOG_LEVEL_INF);

/* 1000 msec = 1 sec */
#define SLEEP_TIME_MS 1000

int main(void)
{
	int ret;

	LOG_INF("Maui Firmware Starting");

	ret = leds_init();
	if (ret < 0) {
		LOG_ERR("Failed to initialize LEDs (err %d)", ret);
		led_set_state(LED_GREEN, LED_STATE_SYS_ERROR);
		return 0;
	}

	ret = adcs_init();
	if (ret < 0) {
		LOG_ERR("Failed to initialize ADCs (err %d)", ret);
		led_set_state(LED_GREEN, LED_STATE_SYS_ERROR);
		return 0;
	}

	ret = init_usb();
	if (ret < 0) {
		LOG_ERR("Failed to initialize USB (err %d)", ret);
		led_set_state(LED_GREEN, LED_STATE_SYS_ERROR);
		return 0;
	}

	return 0;
}
