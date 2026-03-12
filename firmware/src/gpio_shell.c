/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "gpio.h"

#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_DECLARE(gpio);

static void gpio_print(const struct shell *sh, enum gpio_t gpio_id, int val)
{
	char type = (gpio_get_init_flags(gpio_id) & GPIO_OUTPUT) ? 'O' : 'I';
	shell_print(sh, "[%25s] %c = %d", gpio_get_name(gpio_id), type, val);
}

static int cmd_get(const struct shell *sh, size_t argc, char **argv)
{
	if (argc > 2) {
		shell_print(sh, "Usage: gpio get [gpio]");
		return -EINVAL;
	}

	int rv;
	char *name = NULL;

	if (argc == 2)
		name = argv[1];

	for (unsigned a = 0; a < GPIO_COUNT; a++) {
		if (name && strcmp(gpio_get_name(a), name))
			continue;

		rv = gpio_get(a);
		if (rv < 0) {
			shell_error(sh, "Couldn't get value of GPIO");
			return rv;
		}

		gpio_print(sh, a, rv);
	}

	return 0;
}

static int cmd_set(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 3) {
		shell_print(sh, "Usage: gpio set <gpio> 0|1");
		return -EINVAL;
	}

	char *name = argv[1];
	int rv = 0;
	int val = shell_strtol(argv[2], 10, &rv);

	if (rv) {
		shell_error(sh, "Invalid parameter: %s", argv[2]);
		return -EINVAL;
	}

	for (unsigned a = 0; a < GPIO_COUNT; a++) {
		if (strcmp(gpio_get_name(a), name))
			continue;

		rv = gpio_set(a, val);
		if (rv < 0) {
			shell_error(sh, "Couldn't set value of GPIO");
			return val;
		}

		gpio_print(sh, a, val);
		return 0;
	}

	return -EINVAL;
}

static int cmd_toggle(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2)
		return -1;

	char *name = argv[1];
	int rv;
	int val;

	for (unsigned a = 0; a < GPIO_COUNT; a++) {
		if (strcmp(gpio_get_name(a), name))
			continue;

		rv = gpio_get(a);
		if (rv < 0) {
			shell_error(sh, "Couldn't get value of GPIO");
			return rv;
		}

		val = !rv;
		rv = gpio_set(a, val);
		if (rv < 0) {
			shell_error(sh, "Couldn't set value of GPIO");
			return rv;
		}

		gpio_print(sh, a, val);
		return 0;
	}

	return -EINVAL;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_gpio_cmds,
	SHELL_CMD(get, NULL, "Get value of one or more GPIOs", cmd_get),
	SHELL_CMD(set, NULL, "Set value of GPIO", cmd_set),
	SHELL_CMD(toggle, NULL, "Toggle value of GPIO", cmd_toggle),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(gpio, &sub_gpio_cmds, "Commands to manipulate GPIO", NULL);
