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

#define LED_SM_LOOP_PERIOD K_MSEC(100)

#define LED_SM_IDX_INIT_STATE 0
#define LED_SM_IDX_PERIOD 1

#define LED_SM_INIT_STATE_OFF 0
#define LED_SM_INIT_STATE_ON 1

#define LED_SM_PERIOD_NONE 0
#define LED_SM_PERIOD_100_MS 1
#define LED_SM_PERIOD_500_MS 5

static const struct device *led_dev = DEVICE_DT_GET(LED_CONTROLLER_NODE);

static const uint32_t led_indices[LED_COUNT] = {
	[LED_GREEN] = DT_NODE_CHILD_IDX(GREEN_LED_NODE),
	[LED_AMBER] = DT_NODE_CHILD_IDX(AMBER_LED_NODE),
};

static bool led_states[LED_COUNT];
static enum led_override_mode led_override_modes[LED_COUNT];

static struct k_timer leds_timer;
static enum led_pattern led_current_pattern[LED_COUNT] = {
	[LED_GREEN] = LED_PATTERN_BLINK_SLOW,
	[LED_AMBER] = LED_PATTERN_OFF,
};

static int led_sm_cnt = 0;

static const uint8_t led_pattern_config[LED_PATTERN_COUNT][2] = {
	[LED_PATTERN_OFF] = { LED_SM_INIT_STATE_OFF, LED_SM_PERIOD_NONE },
	[LED_PATTERN_SOLID_ON] = { LED_SM_INIT_STATE_ON, LED_SM_PERIOD_NONE },
	[LED_PATTERN_BLINK_SLOW] = { LED_SM_INIT_STATE_ON,
				     LED_SM_PERIOD_500_MS },
	[LED_PATTERN_BLINK_FAST] = { LED_SM_INIT_STATE_ON,
				     LED_SM_PERIOD_100_MS },
};

static void led_apply_state(enum led_id id, int sm_cnt)
{
	bool should_be_on = false;
	enum led_override_mode mode;
	enum led_pattern pattern;

	unsigned int key = irq_lock();
	mode = led_override_modes[id];
	pattern = led_current_pattern[id];
	irq_unlock(key);

	if (mode != LED_OVERRIDE_NONE) {
		switch (mode) {
		case LED_OVERRIDE_ON:
			should_be_on = true;
			break;
		case LED_OVERRIDE_OFF:
			should_be_on = false;
			break;
		case LED_OVERRIDE_BLINK:
			should_be_on =
				((sm_cnt / LED_SM_PERIOD_500_MS) % 2 == 0);
			break;
		default:
			return;
		}
	} else {
		int period = led_pattern_config[pattern][LED_SM_IDX_PERIOD];
		if (period == LED_SM_PERIOD_NONE) {
			should_be_on =
				(led_pattern_config[pattern]
						   [LED_SM_IDX_INIT_STATE] ==
				 LED_SM_INIT_STATE_ON);
		} else {
			should_be_on = ((sm_cnt / period) % 2 == 0);
		}
	}

	if (should_be_on) {
		led_enable(id);
	} else {
		led_disable(id);
	}
}

void timer_expiry_cb(struct k_timer *)
{
	unsigned int key = irq_lock();
	int sm_cnt = led_sm_cnt;
	led_sm_cnt = (led_sm_cnt + 1) % 10;
	irq_unlock(key);

	for (unsigned id = 0; id < LED_COUNT; id++) {
		led_apply_state(id, sm_cnt);
	}
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
		led_override_modes[i] = LED_OVERRIDE_NONE;
	}

	k_timer_init(&leds_timer, timer_expiry_cb, NULL);
	k_timer_start(&leds_timer, K_NO_WAIT, LED_SM_LOOP_PERIOD);

	LOG_INF("LEDs initialized");
	return 0;
}

int led_enable(enum led_id id)
{
	if (id >= LED_COUNT) {
		return -EINVAL;
	}

	unsigned int key = irq_lock();
	if (led_states[id]) {
		irq_unlock(key);
		return 0;
	}
	led_states[id] = true;
	irq_unlock(key);

	return led_on(led_dev, led_indices[id]);
}

int led_disable(enum led_id id)
{
	if (id >= LED_COUNT) {
		return -EINVAL;
	}

	unsigned int key = irq_lock();
	if (!led_states[id]) {
		irq_unlock(key);
		return 0;
	}
	led_states[id] = false;
	irq_unlock(key);

	return led_off(led_dev, led_indices[id]);
}

int led_toggle(enum led_id id)
{
	if (id >= LED_COUNT) {
		return -EINVAL;
	}

	unsigned int key = irq_lock();
	bool is_on = led_states[id];
	irq_unlock(key);

	if (is_on) {
		return led_disable(id);
	} else {
		return led_enable(id);
	}
}

int led_set_pattern(enum led_id id, enum led_pattern pattern)
{
	if (id >= LED_COUNT || pattern >= LED_PATTERN_COUNT) {
		return -EINVAL;
	}

	unsigned int key = irq_lock();
	led_current_pattern[id] = pattern;
	int sm_cnt = led_sm_cnt;
	irq_unlock(key);

	led_apply_state(id, sm_cnt);

	return 0;
}

enum led_pattern led_get_pattern(enum led_id id)
{
	if (id >= LED_COUNT) {
		return LED_PATTERN_OFF;
	}
	return led_current_pattern[id];
}

int led_set_override(enum led_id id, enum led_override_mode mode)
{
	if (id >= LED_COUNT) {
		return -EINVAL;
	}

	if (mode == LED_OVERRIDE_NONE) {
		return led_clear_override(id);
	}

	unsigned int key = irq_lock();
	led_override_modes[id] = mode;
	int sm_cnt = led_sm_cnt;
	irq_unlock(key);

	led_apply_state(id, sm_cnt);

	return 0;
}

int led_clear_override(enum led_id id)
{
	if (id >= LED_COUNT) {
		return -EINVAL;
	}

	unsigned int key = irq_lock();
	led_override_modes[id] = LED_OVERRIDE_NONE;
	int sm_cnt = led_sm_cnt;
	irq_unlock(key);

	led_apply_state(id, sm_cnt);

	return 0;
}

bool led_is_overridden(enum led_id id)
{
	if (id >= LED_COUNT) {
		return false;
	}
	return led_override_modes[id] != LED_OVERRIDE_NONE;
}

const char *led_pattern_to_str(enum led_pattern pattern)
{
	switch (pattern) {
	case LED_PATTERN_OFF:
		return "OFF";
	case LED_PATTERN_SOLID_ON:
		return "SOLID_ON";
	case LED_PATTERN_BLINK_SLOW:
		return "BLINK_SLOW";
	case LED_PATTERN_BLINK_FAST:
		return "BLINK_FAST";
	default:
		return "UNKNOWN";
	}
}
