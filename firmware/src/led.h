/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef LED_H_
#define LED_H_

#include <stdbool.h>

#include <zephyr/kernel.h>

enum led_id {
	LED_GREEN = 0,
	LED_AMBER = 1,
	LED_COUNT = 2,
};

enum led_pattern {
	LED_PATTERN_OFF = 0,
	LED_PATTERN_SOLID_ON = 1,
	LED_PATTERN_BLINK_SLOW = 2, /* 500 ms period */
	LED_PATTERN_BLINK_FAST = 3, /* 100 ms period */
	LED_PATTERN_COUNT = 4,
};

enum led_override_mode {
	LED_OVERRIDE_NONE = 0,
	LED_OVERRIDE_OFF,
	LED_OVERRIDE_ON,
	LED_OVERRIDE_BLINK,
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
 * @param id The LED identifier.
 * @return 0 on success, negative errno code on failure.
 */
int led_enable(enum led_id id);

/**
 * @brief Turn off an LED.
 *
 * @param id The LED identifier.
 * @return 0 on success, negative errno code on failure.
 */
int led_disable(enum led_id id);

/**
 * @brief Toggle an LED.
 *
 * @param id The LED identifier.
 * @return 0 on success, negative errno code on failure.
 */
int led_toggle(enum led_id id);

/**
 * @brief Set an LED's visual pattern.
 *
 * @param id The LED identifier.
 * @param pattern Visual pattern to set.
 * @return 0 on success, negative errno code on failure.
 */
int led_set_pattern(enum led_id id, enum led_pattern pattern);

/**
 * @brief Get an LED's current visual pattern.
 *
 * @param id The LED identifier.
 * @return Current pattern.
 */
enum led_pattern led_get_pattern(enum led_id id);

/**
 * @brief Override LED pattern manually for testing.
 *
 * @param id The LED identifier.
 * @param mode The override mode (ON, OFF, BLINK, NONE).
 * @return 0 on success, negative errno code on failure.
 */
int led_set_override(enum led_id id, enum led_override_mode mode);

/**
 * @brief Clear manual override on an LED and return to automatic control.
 *
 * @param id The LED identifier.
 * @return 0 on success, negative errno code on failure.
 */
int led_clear_override(enum led_id id);

/**
 * @brief Check if an LED is currently overridden.
 *
 * @param id The LED identifier.
 * @return true if overridden, false otherwise.
 */
bool led_is_overridden(enum led_id id);

/**
 * @brief Convert LED pattern to human-readable string.
 *
 * @param pattern The LED pattern.
 * @return String description of pattern.
 */
const char *led_pattern_to_str(enum led_pattern pattern);

#endif /* LED_H_ */
