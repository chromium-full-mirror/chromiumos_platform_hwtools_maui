/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef GPIO_H_
#define GPIO_H_

#include <zephyr/devicetree.h>

#define _GPIO_NAME_TO_ENUM_INTERMEDIATE(name) GPIO_##name
#define GPIO_NAME_TO_ENUM(name) _GPIO_NAME_TO_ENUM_INTERMEDIATE(name)

#define GPIO_DT_SPEC_EXT_GET_BY_IDX(node) \
	GPIO_NAME_TO_ENUM(DT_NODE_FULL_NAME_UPPER_TOKEN(node)),

#define ALL_CHILDREN(id, compat, ...) \
	DT_FOREACH_CHILD(DT_INST(id, compat), GPIO_DT_SPEC_EXT_GET_BY_IDX)

/**
 * Enum for all available GPIOs
 * They use node's name in uppercase with GPIO_ prefix.
 * For node h2h_reset the enum will be GPIO_H2H_RESET
 *
 * To get list of all declared values here, we can use
 * CONFIG_COMPILER_SAVE_TEMPS=y and check the gpio.c.i file from build
 * directory.
 */
enum gpio_t {
	_GPIO_START = -1,
	DT_COMPAT_FOREACH_STATUS_OKAY_VARGS(google_maui_gpios, ALL_CHILDREN)
	GPIO_COUNT,
};

#undef ALL_CHILDREN
#undef GPIO_DT_SPEC_EXT_GET_BY_IDX

/**
 * Get name for the GPIO from enum type
 * @param gpio_id GPIO
 * @return Node name for the gpio
 */
const char *gpio_get_name(enum gpio_t gpio_id);

/**
 * Get flags used to initialize the GPIO
 * @param gpio_id GPIO
 * @return flags used to initialize the GPIO
 */
int gpio_get_init_flags(enum gpio_t gpio_id);

/**
 * Get value reading from input GPIO, or buffered, set value for output GPIO
 * @param gpio_id GPIO
 * @return 0/1 if call is successful, negative value otherwise
 */
int gpio_get(enum gpio_t gpio_id);

/**
 * Set value for output GPIO
 * @param gpio_id GPIO
 * @param val 0 or 1, logical state of pin
 * @return 0 if success, negative value if error or if GPIO is not an output
 */
int gpio_set(enum gpio_t gpio_id, int val);

#endif /* GPIO_H_ */