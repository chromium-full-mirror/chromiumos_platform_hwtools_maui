/* Copyright 2025 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "adc.h"
#include "led.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main);

/* 1000 msec = 1 sec */
#define SLEEP_TIME_MS 1000

int main(void)
{
	int ret;

	LOG_INF("Maui Firmware Starting");

	ret = leds_init();
	if (ret < 0) {
		LOG_ERR("Failed to initialize LEDs (err %d)", ret);
		return 0;
	}

	ret = adcs_init();
	if (ret < 0) {
		LOG_ERR("Failed to initialize ADCs (err %d)", ret);
		return 0;
	}

	LOG_INF("Main loop~!");
	while (1) {
		led_toggle(LED_GREEN);
		led_toggle(LED_AMBER);

		k_msleep(SLEEP_TIME_MS);
	}
	return 0;
}
