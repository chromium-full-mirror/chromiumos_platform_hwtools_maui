/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

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
	{                                                                      \
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

static int gpios_init(const struct device *dev)
{
	int rv;

	for (unsigned a = 0; a < ARRAY_SIZE(drv_gpios); a++) {
		LOG_DBG("Configuring GPIO: %s", drv_gpios[a].name);
		rv = gpio_pin_configure(drv_gpios[a].port, drv_gpios[a].pin,
			drv_gpios[a].flags);

		if (rv < 0) {
			LOG_ERR("Couldn't configure GPIO: %s",
				drv_gpios[a].name);
		}
	}

	LOG_INF("Initialized GPIOs");
	return 0;
}

DEVICE_DT_INST_DEFINE(0, gpios_init, NULL, NULL, NULL, POST_KERNEL,
		      CONFIG_APPLICATION_INIT_PRIORITY, NULL);
