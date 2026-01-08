/* Copyright 2025 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "led.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(main);

/* 1000 msec = 1 sec */
#define SLEEP_TIME_MS 1000

#define USB3_HOST_NODE DT_NODELABEL(usb3_mux_host_oe)
static const struct gpio_dt_spec usb3_host_gpio =
	GPIO_DT_SPEC_GET(USB3_HOST_NODE, gpios);

int init_gpio(void)
{
	int ret;

	if (!gpio_is_ready_dt(&usb3_host_gpio)) {
		return 0;
	}

	/* Configure the pin to enable USB3 by default */
	ret = gpio_pin_configure_dt(&usb3_host_gpio, GPIO_OUTPUT_ACTIVE);
	if (ret < 0) {
		return 0;
	}

	return 1;
}

int main(void)
{
	int ret;

	LOG_INF("Maui Firmware Starting");

	ret = leds_init();
	if (ret < 0) {
		LOG_ERR("Failed to initialize LEDs (err %d)", ret);
		return 0;
	}

	ret = init_gpio();
	if (ret < 0) {
		LOG_ERR("Failed to initialize GPIOs (err %d)", ret);
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
