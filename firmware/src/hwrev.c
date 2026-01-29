/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/shell/shell.h>

#define DT_DRV_COMPAT google_maui_hwrev

#if DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) != 1
#error Invalid number of google,maui-hwrev instances
#endif

#define GPIO_DT_SPEC_GET_BY_IDX_WITH_COMMA(node_id, prop, idx) \
	GPIO_DT_SPEC_GET_BY_IDX(node_id, prop, idx),

static struct gpio_dt_spec pins[] = {
	DT_FOREACH_PROP_ELEM(DT_DRV_INST(0), gpios,
		GPIO_DT_SPEC_GET_BY_IDX_WITH_COMMA)
};

static int hwrev_id = -1;

int hwrev_read()
{
	return hwrev_id;
}

static int cmd_hwrev(const struct shell *sh, size_t argc, char **argv)
{
	shell_print(sh, "Hardware revision: %d", hwrev_id);
	return 0;
}

SHELL_CMD_REGISTER(hwrev, NULL, "Print hardware revision id", cmd_hwrev);

static int hwrev_init(const struct device *dev)
{
	int id = 0;
	int rv;

	for (unsigned a = 0; a < ARRAY_SIZE(pins); a++) {
		rv = gpio_pin_configure_dt(&pins[a], GPIO_INPUT);
		if (rv < 0)
			return rv;
	}

	for (unsigned a = 0; a < ARRAY_SIZE(pins); a++) {
		rv = gpio_pin_get_dt(&pins[a]);
		if (rv < 0)
			return rv;

		id <<= 1;
		id |= rv;
	}

	hwrev_id = id;
	return 0;
}

DEVICE_DT_INST_DEFINE(0, hwrev_init, NULL, NULL, NULL,
	POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT, NULL);
