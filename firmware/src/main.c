/* Copyright 2025 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "adc.h"
#include "led.h"
#include "usb_init.h"

#ifdef CONFIG_MAUI_HEALTH
#include "health.h"
#endif
#ifdef CONFIG_MAUI_NVS_STORAGE
#include "nvs_storage.h"
#endif
#ifdef CONFIG_MAUI_RPC
#include "rpc/rpc.h"
#endif

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

#ifdef CONFIG_MAUI_NVS_STORAGE
	(void)nvs_storage_init();
#endif
#ifdef CONFIG_MAUI_HEALTH
	(void)health_init();
#endif

	ret = leds_init();
	if (ret < 0) {
		LOG_ERR("Failed to initialize LEDs (err %d)", ret);
		led_set_pattern(LED_GREEN, LED_PATTERN_BLINK_FAST);
	}

	ret = adcs_init();
	if (ret < 0) {
		LOG_ERR("Failed to initialize ADCs (err %d)", ret);
		led_set_pattern(LED_GREEN, LED_PATTERN_BLINK_FAST);
	}

	ret = init_usb();
	if (ret < 0) {
		LOG_ERR("Failed to initialize USB (err %d)", ret);
		led_set_pattern(LED_GREEN, LED_PATTERN_BLINK_FAST);
	}

#ifdef CONFIG_MAUI_RPC
	ret = rpc_init();
	if (ret < 0) {
		LOG_ERR("Failed to initialize RPC (err %d)", ret);
	}
#endif

	LOG_INF("Maui Firmware Initialized Successfully");

	/*
	 * Nothing further to do on this thread. Everything runs from its own
	 * thread or workqueue from here, and Zephyr is happy for main() to
	 * return.
	 */
	return 0;
}
