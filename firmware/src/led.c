/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "led.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(leds);

/*
 * The LEDs are defined as children of the "gpio-leds" compatible node.
 * We identify the controller by the parent of one of the LEDs.
 */
#define GREEN_LED_NODE DT_NODELABEL(ec_led_green)
#define AMBER_LED_NODE DT_NODELABEL(ec_led_amber)
#define LED_CONTROLLER_NODE DT_NODELABEL(leds)

static const struct device *led_dev = DEVICE_DT_GET(LED_CONTROLLER_NODE);

static const uint32_t led_indices[LED_COUNT] = {
	[LED_GREEN] = DT_NODE_CHILD_IDX(GREEN_LED_NODE),
	[LED_AMBER] = DT_NODE_CHILD_IDX(AMBER_LED_NODE),
};

static bool led_states[LED_COUNT];

int leds_init(void)
{
	if (!device_is_ready(led_dev)) {
		LOG_ERR("LED device %s not ready", led_dev->name);
		return -ENODEV;
	}

	/* Initialize LEDs to off */
	for (int i = 0; i < LED_COUNT; i++) {
		led_disable(i);
	}

	LOG_INF("LEDs initialized");
	return 0;
}

int led_enable(enum led_color color)
{
	if (color >= LED_COUNT) {
		return -EINVAL;
	}

	int ret = led_on(led_dev, led_indices[color]);
	if (ret == 0) {
		led_states[color] = true;
	}
	return ret;
}

int led_disable(enum led_color color)
{
	if (color >= LED_COUNT) {
		return -EINVAL;
	}

	int ret = led_off(led_dev, led_indices[color]);
	if (ret == 0) {
		led_states[color] = false;
	}
	return ret;
}

int led_toggle(enum led_color color)
{
	if (color >= LED_COUNT) {
		return -EINVAL;
	}

	if (led_states[color]) {
		return led_disable(color);
	} else {
		return led_enable(color);
	}
}
