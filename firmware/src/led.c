/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "led.h"

#include <zephyr/devicetree.h>
#include <zephyr/drivers/led.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(leds);

/*
 * The LEDs are defined as children of the "gpio-leds" compatible node.
 * We identify the controller by the parent of one of the LEDs.
 */
#define GREEN_LED_NODE DT_NODELABEL(ec_led_green)
#define AMBER_LED_NODE DT_NODELABEL(ec_led_amber)
#define LED_CONTROLLER_NODE DT_NODELABEL(leds)

#define LED_SM_LOOP_PERIOD	K_MSEC(100)

#define LED_SM_IDX_INIT_STATE	0
#define LED_SM_IDX_PERIOD	1

#define LED_SM_INIT_STATE_OFF	0
#define LED_SM_INIT_STATE_ON	1

#define LED_SM_PERIOD_NONE	0
#define LED_SM_PERIOD_100_MS	1
#define LED_SM_PERIOD_500_MS	5

static const struct device *led_dev = DEVICE_DT_GET(LED_CONTROLLER_NODE);

static const uint32_t led_indices[LED_COUNT] = {
	[LED_GREEN] = DT_NODE_CHILD_IDX(GREEN_LED_NODE),
	[LED_AMBER] = DT_NODE_CHILD_IDX(AMBER_LED_NODE),
};

static bool led_states[LED_COUNT];

static struct k_timer leds_timer;
static enum led_state led_current_state[LED_COUNT] = {
	[LED_GREEN] = LED_STATE_SYS_GOOD,
	[LED_AMBER] = LED_STATE_PD_NO_POWER
};

static int led_sm_cnt = 0;

static uint8_t led_sm[LED_COUNT][LED_STATE_COUNT][2] = {
	[LED_GREEN] = {
		[LED_STATE_SYS_GOOD]	= {
			LED_SM_INIT_STATE_ON, LED_SM_PERIOD_500_MS
		},
		[LED_STATE_SYS_ERROR]	= {
			LED_SM_INIT_STATE_ON, LED_SM_PERIOD_100_MS
		},
	},
	[LED_AMBER] = {
		[LED_STATE_PD_NO_POWER]	= {
			LED_SM_INIT_STATE_OFF, LED_SM_PERIOD_NONE
		},
		[LED_STATE_PD_CONTRACT]	= {
			LED_SM_INIT_STATE_ON, LED_SM_PERIOD_NONE
		},
	}
};

void timer_expiry_cb(struct k_timer *)
{
	for (unsigned id = 0; id < LED_COUNT; id++) {
		int state = led_current_state[id];
		int period = led_sm[id][state][LED_SM_IDX_PERIOD];
		if (!period)
			continue;

		if (led_sm_cnt % period == 0) {
			led_toggle(id);
		}
	}

	led_sm_cnt = (led_sm_cnt + 1) % 10;
}

int leds_init(void)
{
	if (!device_is_ready(led_dev)) {
		LOG_ERR("LED device %s not ready", led_dev->name);
		return -ENODEV;
	}

	/* Initialize LEDs to off */
	for (int i = 0; i < LED_COUNT; i++) {
		led_disable(i);
	}

	k_timer_init(&leds_timer, timer_expiry_cb, NULL);
	k_timer_start(&leds_timer, K_NO_WAIT, LED_SM_LOOP_PERIOD);

	LOG_INF("LEDs initialized");
	return 0;
}

int led_enable(enum led_color color)
{
	if (color >= LED_COUNT) {
		return -EINVAL;
	}

	int ret = led_on(led_dev, led_indices[color]);
	if (ret == 0) {
		led_states[color] = true;
	}
	return ret;
}

int led_disable(enum led_color color)
{
	if (color >= LED_COUNT) {
		return -EINVAL;
	}

	int ret = led_off(led_dev, led_indices[color]);
	if (ret == 0) {
		led_states[color] = false;
	}
	return ret;
}

int led_toggle(enum led_color color)
{
	if (color >= LED_COUNT) {
		return -EINVAL;
	}

	if (led_states[color]) {
		return led_disable(color);
	} else {
		return led_enable(color);
	}
}

int led_set_state(enum led_color color, enum led_state state)
{
	led_current_state[color] = state;

	if (led_sm[color][state][LED_SM_IDX_INIT_STATE])
		led_enable(color);
	else
		led_disable(color);

	return 0;
}
