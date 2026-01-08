/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef LED_H_
#define LED_H_

#include <zephyr/kernel.h>

enum led_color { LED_GREEN, LED_AMBER, LED_COUNT };

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

#endif /* LED_H_ */
