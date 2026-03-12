/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "adc.h"
#include "tps6699x.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

LOG_MODULE_REGISTER(sbu_mux);

#define DT_DRV_COMPAT google_maui_sbu_mux

#if DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) != 1
#error Invalid number of google,maui-sbu-mux instances
#endif

#define SBU_DIRECT 1
#define SBU_FLIP 0

#define MODE_SBU_DISCONNECT 0
#define MODE_SBU_CONNECT 1
#define MODE_SBU_FLIP 2
#define MODE_SBU_OTHER 3

#define GPIO_DT_SPEC_GET_BY_IDX_WITH_COMMA(node_id, prop, idx) \
	GPIO_DT_SPEC_GET_BY_IDX(node_id, prop, idx),

struct sbu_mux_cfg {
#if IS_ENABLED(CONFIG_SBU_MUX_TYPE_PDC)
	const struct device *pdc;
#elif IS_ENABLED(CONFIG_SBU_MUX_TYPE_GPIO)
	struct gpio_dt_spec pin_mux_oe;
	struct gpio_dt_spec pin_mux_sel;
#endif

	int low_thresh;
	int high_thresh;
};

struct sbu_mux_data {
	struct sbu_mux_cfg *cfg;
	struct k_timer drv_timer;
	struct k_work drv_wq;

	int mux_en;
	int count;
	int last;
	int polarity;
};

static void sbu_mux_wq_cb(struct k_work *wq)
{
	struct sbu_mux_data *data =
		CONTAINER_OF(wq, struct sbu_mux_data, drv_wq);
	const struct sbu_mux_cfg *cfg = data->cfg;

	/* Read sbu voltage levels */
	int sbu1 = adc_ch_read(ADC_CH_SBU1_DET);
	int sbu2 = adc_ch_read(ADC_CH_SBU2_DET);

	if (sbu1 < 0 || sbu2 < 0) {
		LOG_ERR("Error reading SBU values: %d %d", sbu1, sbu2);
		return;
	}

	/*
	 * While SBU_MUX is disabled (SuzyQ unplugged), we'll poll the SBU lines
	 * to check if an idling, unconfigured USB device is present.
	 * USB FS pulls one line high for connect request.
	 * If so, and it persists for 500ms, we'll enable the SuzyQ in that
	 * orientation.
	 */
	if ((!data->mux_en) && (sbu1 > cfg->high_thresh) &&
	    (sbu2 < cfg->low_thresh)) {
		/* Check flip connection polarity. */
		if (data->last != MODE_SBU_FLIP) {
			data->last = MODE_SBU_FLIP;
			data->polarity = SBU_FLIP;
			data->count = 0;
		} else {
			data->count++;
		}
	} else if ((!data->mux_en) && (sbu2 > cfg->high_thresh) &&
		   (sbu1 < cfg->low_thresh)) {
		/* Check direct connection polarity. */
		if (data->last != MODE_SBU_CONNECT) {
			data->last = MODE_SBU_CONNECT;
			data->polarity = SBU_DIRECT;
			data->count = 0;
		} else {
			data->count++;
		}
		/*
		 * If SuzyQ is enabled, we'll poll for a persistent no-signal
		 * for 500ms. Since USB is differential, we should never see
		 * GND/GND while the device is connected.
		 * If disconnected, electrically remove SuzyQ.
		 */
	} else if ((data->mux_en) && (sbu1 < cfg->low_thresh) &&
		   (sbu2 < cfg->low_thresh)) {
		/* Check for SBU disconnect if connected. */
		if (data->last != MODE_SBU_DISCONNECT) {
			data->last = MODE_SBU_DISCONNECT;
			data->count = 0;
		} else {
			data->count++;
		}
	} else {
		/* Didn't find anything, reset state. */
		data->last = MODE_SBU_OTHER;
		data->count = 0;
	}

	/*
	 * We have seen a new state continuously for 500ms.
	 * Let's update the mux to enable/disable SuzyQ appropriately.
	 */
	if (data->count > 5) {
		if (data->mux_en) {
			/* Disable mux as it's disconnected now. */
#if IS_ENABLED(CONFIG_SBU_MUX_TYPE_PDC)
			tps_cmd_sbud(cfg->pdc, 0);
#else
			gpio_pin_set_dt(&cfg->pin_mux_oe, 0);
#endif
			data->mux_en = 0;
			k_msleep(10);
			LOG_INF("disconnected");
		} else {
			/* SBU flip = polarity */
#if IS_ENABLED(CONFIG_SBU_MUX_TYPE_PDC)
			tps_cmd_sbdf(cfg->pdc, data->polarity);
			tps_cmd_sbud(cfg->pdc, 1);
#else
			gpio_pin_set_dt(&cfg->pin_mux_sel, data->polarity);
			gpio_pin_set_dt(&cfg->pin_mux_oe, 1);
#endif
			data->mux_en = 1;
			k_msleep(10);
			LOG_INF("connected %s",
				data->polarity ? "flip" : "noflip");
		}
	}
}

static void sbu_mux_timer_cb(struct k_timer *timer)
{
	struct sbu_mux_data *data =
		CONTAINER_OF(timer, struct sbu_mux_data, drv_timer);
	k_work_submit(&data->drv_wq);
}

static int sbu_mux_init(const struct device *dev)
{
	struct sbu_mux_data *data = dev->data;

#if IS_ENABLED(CONFIG_SBU_MUX_TYPE_GPIO)
	const struct sbu_mux_cfg *cfg = dev->config;
	int rv;

	rv = gpio_pin_configure_dt(&cfg->pin_mux_oe, GPIO_OUTPUT_LOW);
	if (rv < 0) {
		LOG_ERR("Couldn't configure mux en pin");
		return rv;
	}

	rv = gpio_pin_configure_dt(&cfg->pin_mux_sel, GPIO_OUTPUT_LOW);
	if (rv < 0) {
		LOG_ERR("Couldn't configure mux polarity pin");
		return rv;
	}
#endif

	k_work_init(&data->drv_wq, sbu_mux_wq_cb);
	k_timer_init(&data->drv_timer, sbu_mux_timer_cb, NULL);
	k_timer_start(&data->drv_timer, K_MSEC(CONFIG_SBU_MUX_INIT_DELAY),
		      K_MSEC(CONFIG_SBU_MUX_PERIOD));

	return 0;
}

#define DRV_INST_CFG_PDC(idx) .pdc = DEVICE_DT_GET(DT_INST_PROP(idx, pdc)),

#define DRV_INST_CFG_GPIO(idx)                                            \
	.pin_mux_oe = GPIO_DT_SPEC_INST_GET_BY_IDX(idx, mux_oe_gpios, 0), \
	.pin_mux_sel = GPIO_DT_SPEC_INST_GET_BY_IDX(idx, mux_sel_gpios, 0),

#define DRV_INST_CFG(idx)                                                    \
	static struct sbu_mux_cfg drv_cfg_##idx = {                          \
		COND_CODE_1(IS_ENABLED(CONFIG_SBU_MUX_TYPE_PDC),             \
			    (DRV_INST_CFG_PDC(idx)), ())                     \
			COND_CODE_1(IS_ENABLED(CONFIG_SBU_MUX_TYPE_GPIO),    \
				    (DRV_INST_CFG_GPIO(idx)), ())            \
				.low_thresh = DT_INST_PROP(idx, low_thresh), \
		.high_thresh = DT_INST_PROP(idx, high_thresh),               \
	};

#define DRV_INST_DATA(idx)                            \
	static struct sbu_mux_data drv_data_##idx = { \
		.cfg = &drv_cfg_##idx,                \
		.mux_en = 0,                          \
		.count = 0,                           \
		.last = 0,                            \
		.polarity = 0,                        \
	};

#define DRV_INST_DEFINE(idx)                                            \
	DRV_INST_CFG(idx)                                               \
	DRV_INST_DATA(idx)                                              \
	DEVICE_DT_INST_DEFINE(idx, sbu_mux_init, NULL, &drv_data_##idx, \
			      &drv_cfg_##idx, POST_KERNEL,              \
			      CONFIG_APPLICATION_INIT_PRIORITY, NULL);

DT_INST_FOREACH_STATUS_OKAY(DRV_INST_DEFINE)
