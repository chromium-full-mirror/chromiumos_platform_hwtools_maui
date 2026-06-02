/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef LED_H_
#define LED_H_

#include <zephyr/kernel.h>

enum led_color { LED_GREEN, LED_AMBER, LED_COUNT };

enum led_state {
	// Green
	LED_STATE_SYS_GOOD = 0, LED_STATE_SYS_ERROR = 1,
	// Amber
	LED_STATE_PD_NO_POWER = 0, LED_STATE_PD_CONTRACT = 1,

	LED_STATE_COUNT
};

/**
 * @brief Initialize the LED module.
 *
 * @return 0 on success, negative errno code on failure.
 */
int leds_init(void);

/**
 * @brief Turn on an LED.
 *
 * @param color The LED color to turn on.
 * @return 0 on success, negative errno code on failure.
 */
int led_enable(enum led_color color);

/**
 * @brief Turn off an LED.
 *
 * @param color The LED color to turn off.
 * @return 0 on success, negative errno code on failure.
 */
int led_disable(enum led_color color);

/**
 * @brief Toggle an LED.
 *
 * @param color The LED color to toggle.
 * @return 0 on success, negative errno code on failure.
 */
int led_toggle(enum led_color color);

/**
 * @brief Set led's state machine to blink in specified period
 *
 * @param color The LED color which state will be changed.
 * @param state State to set.
 * @return 0 on success, negative errno code on failure.
 */
int led_set_state(enum led_color color, enum led_state state);

#endif /* LED_H_ */
