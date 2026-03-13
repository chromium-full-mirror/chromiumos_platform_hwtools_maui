/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "hwrev.h"

#include <zephyr/drivers/gpio/gpio_emul.h>
#include <zephyr/kernel.h>
#include <zephyr/ztest.h>

const struct device *gpio_emul = DEVICE_DT_GET(DT_NODELABEL(gpioemu));

static int test_init()
{
	gpio_pin_configure(gpio_emul, 0, GPIO_INPUT);
	gpio_pin_configure(gpio_emul, 1, GPIO_INPUT);
	gpio_pin_configure(gpio_emul, 2, GPIO_INPUT);

	gpio_emul_input_set(gpio_emul, 0, !!(CONFIG_HWREV_VALUE & BIT(0)));
	gpio_emul_input_set(gpio_emul, 1, !!(CONFIG_HWREV_VALUE & BIT(1)));
	gpio_emul_input_set(gpio_emul, 2, !!(CONFIG_HWREV_VALUE & BIT(2)));

	return 0;
}

SYS_INIT(test_init, POST_KERNEL, 98);

ZTEST(suite_hwrev, test_value_is_cached)
{
	zassert_equal(hwrev_read(), CONFIG_HWREV_VALUE);

	// Check if value was cached at boot-up
	gpio_emul_input_set(gpio_emul, 0, !(CONFIG_HWREV_VALUE & BIT(0)));
	gpio_emul_input_set(gpio_emul, 1, !(CONFIG_HWREV_VALUE & BIT(1)));
	gpio_emul_input_set(gpio_emul, 2, !(CONFIG_HWREV_VALUE & BIT(2)));

	zassert_equal(hwrev_read(), CONFIG_HWREV_VALUE);
}

ZTEST_SUITE(suite_hwrev, NULL, NULL, NULL, NULL, NULL);
