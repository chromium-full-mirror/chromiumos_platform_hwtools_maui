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
#include "bypass.h"

LOG_MODULE_REGISTER(pendrive);

#define DT_DRV_COMPAT google_maui_pendrive

struct pendrive_cfg_t {
	struct gpio_dt_spec gpio_cc_pol;
	struct gpio_dt_spec gpio_cc_det;

	struct gpio_dt_spec gpio_usb2_oe;
	struct gpio_dt_spec gpio_usb2_sel;
	struct gpio_dt_spec gpio_usb3_oe;
	struct gpio_dt_spec gpio_usb3_sel;
	struct gpio_dt_spec gpio_vbus_en;
	struct gpio_dt_spec gpio_cc_en;
};

struct pendrive_data_t {
	struct pendrive_cfg_t *cfg;
	struct gpio_callback cc_det_cb;
	struct k_work gpio_wq;
};

static void host_mux_wq_cb(struct k_work *wq)
{
	struct pendrive_data_t *data = CONTAINER_OF(wq, struct pendrive_data_t, gpio_wq);
	struct pendrive_cfg_t *cfg = data->cfg;

	int det = gpio_pin_get_dt(&cfg->gpio_cc_det);
	int pol = gpio_pin_get_dt(&cfg->gpio_cc_pol);
	LOG_INF("Detection: %d, Polarity: %d", det, pol);

	if (det < 0 || pol < 0) {
		LOG_ERR("Error getting detection or polarity pin");
		return;
	}

	int ret = gpio_pin_set_dt(&cfg->gpio_vbus_en, det);
	if (ret < 0) {
		LOG_ERR("Couldn't set VBus");
		return;
	}

	LOG_INF("VBus %s", (det) ? "enabled" : "disabled");
}

static void pendrive_det_cb(const struct device *port,
	struct gpio_callback *cb,
	gpio_port_pins_t pins)
{
	struct pendrive_data_t *data = CONTAINER_OF(cb, struct pendrive_data_t, cc_det_cb);
	k_work_submit(&data->gpio_wq);
}

static int pendrive_init(const struct device *dev)
{
	struct pendrive_cfg_t *cfg = (struct pendrive_cfg_t*)dev->config;
	struct pendrive_data_t *data = (struct pendrive_data_t*)dev->data;
	LOG_INF("Initializing...");

	int ret = 0;

	ret |= gpio_pin_configure_dt(&cfg->gpio_cc_pol, GPIO_INPUT);
	ret |= gpio_pin_configure_dt(&cfg->gpio_cc_det, GPIO_INPUT);

	ret |= gpio_pin_configure_dt(&cfg->gpio_usb2_oe, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&cfg->gpio_usb2_sel, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&cfg->gpio_usb3_oe, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&cfg->gpio_usb3_sel, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&cfg->gpio_vbus_en, GPIO_OUTPUT_INACTIVE);
	ret |= gpio_pin_configure_dt(&cfg->gpio_cc_en, GPIO_OUTPUT_ACTIVE);

	if (ret) {
		LOG_ERR("Failed to initialize GPIOs");
		return -EINVAL;
	}

	k_work_init(&data->gpio_wq, host_mux_wq_cb);
	gpio_init_callback(&data->cc_det_cb, pendrive_det_cb, BIT(cfg->gpio_cc_det.pin));
	ret = gpio_add_callback(cfg->gpio_cc_det.port, &data->cc_det_cb);
	if (ret < 0) {
		LOG_ERR("Failed to add CC detect callback: %d", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&cfg->gpio_cc_det, GPIO_INT_EDGE_BOTH);
	if (ret < 0) {
		LOG_ERR("Failed to configure CC detect interrupt: %d", ret);
		return ret;
	}

	k_work_submit(&data->gpio_wq);
	LOG_INF("Initialized properly");

	return 0;
}

struct pendrive_cfg_t pendrive_cfg_0 = {
	.gpio_cc_pol = GPIO_DT_SPEC_GET(DT_DRV_INST(0), cc_pol_gpios),
	.gpio_cc_det = GPIO_DT_SPEC_GET(DT_DRV_INST(0), cc_det_gpios),

	.gpio_usb2_oe = GPIO_DT_SPEC_GET(DT_DRV_INST(0), usb2_oe_gpios),
	.gpio_usb2_sel = GPIO_DT_SPEC_GET(DT_DRV_INST(0), usb2_sel_gpios),
	.gpio_usb3_oe = GPIO_DT_SPEC_GET(DT_DRV_INST(0), usb3_oe_gpios),
	.gpio_usb3_sel = GPIO_DT_SPEC_GET(DT_DRV_INST(0), usb3_sel_gpios),
	.gpio_vbus_en = GPIO_DT_SPEC_GET(DT_DRV_INST(0), vbus_en_gpios),
	.gpio_cc_en = GPIO_DT_SPEC_GET(DT_DRV_INST(0), cc_en_gpios),
};

struct pendrive_data_t pendrive_data_0 = {
	.cfg = &pendrive_cfg_0,
};

DEVICE_DT_INST_DEFINE(0, pendrive_init, NULL, &pendrive_data_0, &pendrive_cfg_0, POST_KERNEL,
		      CONFIG_GPIO_INIT_PRIORITY, NULL);

static int cmd_vbus(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_error(sh, "Usage: pendrive vbus on|off");
		return -EINVAL;
	}

	if (strcmp(argv[1], "on") == 0) {
		gpio_pin_set_dt(&pendrive_cfg_0.gpio_vbus_en, 1);
	} else if (strcmp(argv[1], "off") == 0) {
		gpio_pin_set_dt(&pendrive_cfg_0.gpio_vbus_en, 0);
	} else {
		shell_error(sh, "Usage: pendrive vbus on|off");
		return -EINVAL;
	}

	return 0;
}

static int cmd_cc(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_error(sh, "Usage: pendrive cc on|off");
		return -EINVAL;
	}

	if (strcmp(argv[1], "on") == 0) {
		gpio_pin_set_dt(&pendrive_cfg_0.gpio_cc_en, 1);
	} else if (strcmp(argv[1], "off") == 0) {
		gpio_pin_set_dt(&pendrive_cfg_0.gpio_cc_en, 0);
	} else {
		shell_error(sh, "Usage: pendrive cc on|off");
		return -EINVAL;
	}

	return 0;
}

static int cmd_mux(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_error(sh, "Usage: pendrive mux host|dut|off");
		return -EINVAL;
	}

	int oe = (strcmp(argv[1], "off") != 0);
	if (oe) {
		gpio_pin_set_dt(&pendrive_cfg_0.gpio_usb2_oe, 0);
		gpio_pin_set_dt(&pendrive_cfg_0.gpio_usb3_oe, 0);
		bypass_set_mode(BYPASS_MODE_H2H);

		k_sleep(K_MSEC(100));
	}

	if (strcmp(argv[1], "host") == 0) {
		gpio_pin_set_dt(&pendrive_cfg_0.gpio_usb2_sel, 0);
		gpio_pin_set_dt(&pendrive_cfg_0.gpio_usb3_sel, 1);
		shell_print(sh, "Pendrive connected to host");
	} else if (strcmp(argv[1], "dut") == 0) {
		gpio_pin_set_dt(&pendrive_cfg_0.gpio_usb2_sel, 1);
		gpio_pin_set_dt(&pendrive_cfg_0.gpio_usb3_sel, 0);
		shell_print(sh, "Pendrive connected to DUT");
	} else if (strcmp(argv[1], "off") == 0) {
		shell_print(sh, "Pendrive disconnected");
	} else {
		shell_error(sh, "Usage: pendrive mux host|dut|off");
		return -EINVAL;
	}

	gpio_pin_set_dt(&pendrive_cfg_0.gpio_usb2_oe, oe);
	gpio_pin_set_dt(&pendrive_cfg_0.gpio_usb3_oe, oe);

	return 0;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	int vbus_en = gpio_pin_get_dt(&pendrive_cfg_0.gpio_vbus_en);
	int cc_en = gpio_pin_get_dt(&pendrive_cfg_0.gpio_cc_en);

	int det = gpio_pin_get_dt(&pendrive_cfg_0.gpio_cc_det);
	int pol = gpio_pin_get_dt(&pendrive_cfg_0.gpio_cc_pol);

	int usb2sel = gpio_pin_get_dt(&pendrive_cfg_0.gpio_usb2_sel);
	int usb2oe = gpio_pin_get_dt(&pendrive_cfg_0.gpio_usb2_oe);
	int usb3sel = gpio_pin_get_dt(&pendrive_cfg_0.gpio_usb3_sel);
	int usb3oe = gpio_pin_get_dt(&pendrive_cfg_0.gpio_usb3_oe);

	shell_print(sh, "Pendrive:");
	shell_print(sh, "- vbus: %d", vbus_en);
	shell_print(sh, "- cc_en: %d", cc_en);
	shell_print(sh, "- detection: %d", det);
	shell_print(sh, "- polarity: %d", pol);

	shell_print(sh, "- usb 2.0 sel: %d", usb2sel);
	shell_print(sh, "- usb 2.0 oe: %d", usb2oe);
	shell_print(sh, "- usb 3.0 sel: %d", usb3sel);
	shell_print(sh, "- usb 3.0 oe: %d", usb3oe);

	return 0;
}

static int cmd_test_set(const struct shell *sh, size_t argc, char **argv)
{
	struct gpio_dt_spec *gpio;
	int val;

	if (argc != 3) {
		shell_error(sh, "Usage: pendrive tmp_set usb2sel|usb3sel|usb2oe|usb3oe|vbus|cc 0|1");
		return -EINVAL;
	}

	if (strcmp(argv[1], "usb2sel") == 0)
		gpio = &pendrive_cfg_0.gpio_usb2_sel;
	else if (strcmp(argv[1], "usb3sel") == 0)
		gpio = &pendrive_cfg_0.gpio_usb3_sel;
	else if (strcmp(argv[1], "usb2oe") == 0)
		gpio = &pendrive_cfg_0.gpio_usb2_oe;
	else if (strcmp(argv[1], "usb3oe") == 0)
		gpio = &pendrive_cfg_0.gpio_usb3_oe;
	else if (strcmp(argv[1], "vbus") == 0)
		gpio = &pendrive_cfg_0.gpio_vbus_en;
	else if (strcmp(argv[1], "cc") == 0)
		gpio = &pendrive_cfg_0.gpio_cc_en;
	else {
		shell_error(sh, "Usage: pendrive tmp_set usb2sel|usb3sel|usb2oe|usb3oe|vbus|cc 0|1");
		return -EINVAL;
	}

	if (strcmp(argv[2], "0") == 0)
		val = 0;
	else if (strcmp(argv[2], "1") == 0)
		val = 1;
	else {
		shell_error(sh, "Usage: pendrive tmp_set usb2sel|usb3sel|usb2oe|usb3oe|vbus|cc 0|1");
		return -EINVAL;
	}

	int rv = gpio_pin_set_dt(gpio, val);
	shell_print(sh, "Set result: %d [%s = %d]", rv, argv[1], val);

	return rv;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_pendrive_cmds,
	SHELL_CMD(vbus, NULL, "Set VBUS", cmd_vbus),
	SHELL_CMD(cc, NULL, "Set CC detection", cmd_cc),
	SHELL_CMD(mux, NULL, "Set muxes", cmd_mux),
	SHELL_CMD(tmp_set, NULL, "[TEST] set pin", cmd_test_set),
	SHELL_CMD(status, NULL, "Check status", cmd_status),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(pendrive, &sub_pendrive_cmds, "Commands to manipulate USB-C pendrive port", NULL);
