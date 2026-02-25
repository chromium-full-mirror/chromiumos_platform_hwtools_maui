/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "adc.h"
#include "gpio.h"

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(host_mux);

/* Mux Selection States */
#define MUX_SEL_NORMAL 0
#define MUX_SEL_FLIP 1

/* Internal Driver States */
enum host_mux_state {
	STATE_INITIAL = 0,
	STATE_DISCONNECTED,
	STATE_CONNECTED_NORMAL,
	STATE_CONNECTED_FLIP,
};

struct host_mux_data {
	struct k_timer drv_timer;
	struct k_work drv_wq;

	enum host_mux_state current_state; /* Currently applied state */
	enum host_mux_state candidate_state; /* State being debounced */
	int count;
};

static struct host_mux_data host_mux;

/*
 * Logic:
 * - Normal Orientation: CC1 > Thresh, CC2 < Thresh
 * - Flipped Orientation: CC2 > Thresh, CC1 < Thresh
 * - Disconnected: Both < Thresh
 * - Invalid/Other: Both > Thresh (should not happen in valid connection) ->
 * Treat as Disconnected or Maintain state? Treating as disconnected seems
 * safest to avoid shorting.
 */
static enum host_mux_state determine_state(int cc1_mv, int cc2_mv, int thresh)
{
	bool cc1_active = (cc1_mv > thresh);
	bool cc2_active = (cc2_mv > thresh);

	if (cc1_active && !cc2_active) {
		return STATE_CONNECTED_NORMAL;
	} else if (!cc1_active && cc2_active) {
		return STATE_CONNECTED_FLIP;
	} else {
		/* Both inactive or both active (invalid) */
		return STATE_DISCONNECTED;
	}
}

static void host_mux_apply_state(enum host_mux_state state)
{
	switch (state) {
	case STATE_INITIAL:
		gpio_set(GPIO_USB3_MUX_HOST_OE_R_L, 0); /* Disable Output */
		gpio_set(GPIO_HUB_RESET_L, 0); /* Take Hub out of reset */
		LOG_INF("Initial (Waiting for Host)");
		break;
	case STATE_CONNECTED_NORMAL:
		gpio_set(GPIO_USB3_MUX_HOST_SEL_R, MUX_SEL_NORMAL);
		gpio_set(GPIO_USB3_MUX_HOST_OE_R_L, 1); /* Enable Output */
		gpio_set(GPIO_HUB_RESET_L, 0); /* Take Hub out of reset */
		LOG_INF("Connected Normal (CC1)");
		break;
	case STATE_CONNECTED_FLIP:
		gpio_set(GPIO_USB3_MUX_HOST_SEL_R, MUX_SEL_FLIP);
		gpio_set(GPIO_USB3_MUX_HOST_OE_R_L, 1); /* Enable Output */
		gpio_set(GPIO_HUB_RESET_L, 0); /* Take Hub out of reset */
		LOG_INF("Connected Flip (CC2)");
		break;
	case STATE_DISCONNECTED:
	default:
		gpio_set(GPIO_USB3_MUX_HOST_OE_R_L, 0); /* Disable Output */
		gpio_set(GPIO_HUB_RESET_L, 1); /* Keep Hub in reset */
		LOG_INF("Disconnected");
		break;
	}
}

static void host_mux_wq_cb(struct k_work *wq)
{
	struct host_mux_data *data =
		CONTAINER_OF(wq, struct host_mux_data, drv_wq);

	/* Read CC voltage levels */
	int cc1 = adc_ch_read(ADC_CH_USB_HOST_CC1);
	int cc2 = adc_ch_read(ADC_CH_USB_HOST_CC2);

	if (cc1 < 0 || cc2 < 0) {
		LOG_ERR("Error reading CC values: %d %d", cc1, cc2);
		return;
	}

	enum host_mux_state new_sample_state =
		determine_state(cc1, cc2, CONFIG_HOST_MUX_HIGH_THRESH);

	if (new_sample_state == data->candidate_state) {
		/* Sample matches candidate, continue debounce */
		data->count++;
	} else {
		/* Sample differs, start new debounce candidate */
		data->candidate_state = new_sample_state;
		data->count = 1;
	}

	/* Check for debounce completion */
	/* 5 samples * 20ms = 80-100ms */
	if (data->count >= 5) {
		if (data->candidate_state != data->current_state) {
			data->current_state = data->candidate_state;
			host_mux_apply_state(data->current_state);
		}
		/* Clamp count to avoid overflow if running for long time,
		   but keep it >= 5 to show stability */
		data->count = 5;
	}
}

static void host_mux_timer_cb(struct k_timer *timer)
{
	struct host_mux_data *data =
		CONTAINER_OF(timer, struct host_mux_data, drv_timer);
	k_work_submit(&data->drv_wq);
}

static int host_mux_init(void)
{
	struct host_mux_data *data = &host_mux;

	data->current_state = STATE_INITIAL;
	data->candidate_state = STATE_INITIAL;
	/* Start with initial state applied */
	host_mux_apply_state(STATE_INITIAL);

	k_work_init(&data->drv_wq, host_mux_wq_cb);
	k_timer_init(&data->drv_timer, host_mux_timer_cb, NULL);
	k_timer_start(&data->drv_timer, K_MSEC(CONFIG_HOST_MUX_INIT_DELAY),
		      K_MSEC(CONFIG_HOST_MUX_PERIOD));

	return 0;
}

SYS_INIT(host_mux_init, POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY);
