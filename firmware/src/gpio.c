/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include "gpio.h"

LOG_MODULE_REGISTER(gpio);

#define DT_DRV_COMPAT google_maui_gpios

#if DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) != 1
#error Invalid number of google,maui-sbu-mux instances
#endif

struct gpio_dt_spec_ext {
	/** GPIO device controlling the pin */
	const struct device *port;
	/** The pin's number on the device */
	gpio_pin_t pin;
	/** The pin's configuration flags as specified in devicetree */
	gpio_flags_t flags;
	/** Pin name used with shell commands */
	const char *name;
	/** Buffered value set for output pin */
	int value;
};

#define GPIO_DT_SPEC_EXT_GET_BY_IDX(node_id)                                   \
	[GPIO_NAME_TO_ENUM(DT_NODE_FULL_NAME_UPPER_TOKEN(node_id))] = {        \
		.port = DEVICE_DT_GET(DT_GPIO_CTLR_BY_IDX(node_id, gpios, 0)), \
		.pin = DT_GPIO_PIN_BY_IDX(node_id, gpios, 0),                  \
		.flags = DT_GPIO_FLAGS_BY_IDX(node_id, gpios, 0),              \
		.name = DT_NODE_FULL_NAME(node_id),                            \
		.value = 0,                                                    \
	}

#define GPIO_DT_SPEC_EXT_GET_BY_IDX_WITH_COMMA(node) \
	GPIO_DT_SPEC_EXT_GET_BY_IDX(node),

#define ALL_CHILDREN(idx) DT_FOREACH_CHILD(DT_DRV_INST(idx), \
	GPIO_DT_SPEC_EXT_GET_BY_IDX_WITH_COMMA)

static struct gpio_dt_spec_ext drv_gpios[] = {
	DT_INST_FOREACH_STATUS_OKAY(ALL_CHILDREN)
};

const char *gpio_get_name(enum gpio_t gpio_id)
{
	return drv_gpios[gpio_id].name;
}

int gpio_get_init_flags(enum gpio_t gpio_id)
{
	return drv_gpios[gpio_id].flags;
}

int gpio_get(enum gpio_t gpio_id)
{
	struct gpio_dt_spec_ext *gp = &drv_gpios[gpio_id];

	if (gp->flags & GPIO_INPUT)
		return gpio_pin_get(gp->port, gp->pin);
	else if (gp->flags & GPIO_OUTPUT)
		return gp->value;
	else
		return -EIO;
}

int gpio_set(enum gpio_t gpio_id, int val)
{
	struct gpio_dt_spec_ext *gp = &drv_gpios[gpio_id];
	int rv;

	if (!(gp->flags & GPIO_OUTPUT))
		return -EINVAL;

	rv = gpio_pin_set(gp->port, gp->pin, val);
	if (rv < 0)
		return rv;

	LOG_DBG("GPIO %s set to %d", gp->name, val);
	gp->value = val;

	return 0;
}


static int gpios_init(const struct device *dev)
{
	int rv;

	for (unsigned a = 0; a < ARRAY_SIZE(drv_gpios); a++) {
		struct gpio_dt_spec_ext *gp = &drv_gpios[a];

		LOG_DBG("Configuring GPIO: %s", gp->name);
		rv = gpio_pin_configure(gp->port, gp->pin, gp->flags);

		if (rv < 0)
			LOG_ERR("Couldn't configure GPIO: %s", gp->name);

		if (gp->flags & GPIO_OUTPUT) {
			gp->value = !!(gp->flags & GPIO_OUTPUT_INIT_HIGH);
			if ((gp->flags &
			     (GPIO_ACTIVE_LOW | GPIO_OUTPUT_INIT_LOGICAL)) ==
			    GPIO_ACTIVE_LOW)
				gp->value = !gp->value;
		}
	}

	LOG_INF("Initialized GPIOs");
	return 0;
}

DEVICE_DT_INST_DEFINE(0, gpios_init, NULL, NULL, NULL, POST_KERNEL,
		      CONFIG_GPIO_INIT_PRIORITY, NULL);
