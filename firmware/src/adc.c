/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include <zephyr/kernel.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/shell/shell.h>
#include <m0p/dl_factoryregion.h>
#include "adc.h"

// Datasheet for the family specifies this as <-2.1, -1.7>, with
// typical -1.8 mV/'C
#define MSPM0G350X_TSC -1.8
// Value -56 is approximation of (1.0 / MSPM0G350X_TSC * 100)
#define ONE_BY_MSPM0G350X_TSC_INT_APROX -56
// Datasheet for the family specifies this as <27, 33>, with typical 30
#define MSPM0G350X_TSTRIM 30

LOG_MODULE_REGISTER(adc);

#define DT_ADC_NODE_FROM_NODELABEL(node) ADC_DT_SPEC_STRUCT( \
	DT_PARENT(DT_NODELABEL(node)), \
	DT_PROP_BY_IDX(DT_NODELABEL(node), reg, 0))

static const struct adc_dt_spec adc_channels[] = {
	[ADC_CH_USB_HOST_CC1] =
		DT_ADC_NODE_FROM_NODELABEL(adc_usb_host_cc1),
	[ADC_CH_USB_HOST_CC2] =
		DT_ADC_NODE_FROM_NODELABEL(adc_usb_host_cc2),
	[ADC_CH_TEMP_SENSOR] =
		DT_ADC_NODE_FROM_NODELABEL(adc_temp_sensor),
	[ADC_CH_SBU1_DET] =
		DT_ADC_NODE_FROM_NODELABEL(adc_sbu1_det),
	[ADC_CH_SBU2_DET] =
		DT_ADC_NODE_FROM_NODELABEL(adc_sbu2_det),
	[ADC_CH_BSL] =
		DT_ADC_NODE_FROM_NODELABEL(adc_bsl),
	[ADC_CH_PPVAR_USB_CHG_VBUS] =
		DT_ADC_NODE_FROM_NODELABEL(adc_ppvar_usb_chg_vbus),
	[ADC_CH_PPVAR_USB_DUT_VBUS] =
		DT_ADC_NODE_FROM_NODELABEL(adc_ppvar_usb_dut_vbus),
	[ADC_CH_PP5000_USB_HOST_VBUS] =
		DT_ADC_NODE_FROM_NODELABEL(adc_pp5000_usb_host_vbus),
	[ADC_CH_PP5000] =
		DT_ADC_NODE_FROM_NODELABEL(adc_pp5000),
};

char* adc_channel_to_str(enum adc_type ch)
{
	switch(ch) {
	case ADC_CH_USB_HOST_CC1:
		return "USB_HOST_CC1";
	case ADC_CH_USB_HOST_CC2:
		return "USB_HOST_CC2";
	case ADC_CH_TEMP_SENSOR:
		return "TEMP_SENSOR";
	case ADC_CH_SBU1_DET:
		return "SBU1_DET";
	case ADC_CH_SBU2_DET:
		return "SBU2_DET";
	case ADC_CH_BSL:
		return "BSL";
	case ADC_CH_PPVAR_USB_CHG_VBUS:
		return "PPVAR_USB_CHG_VBUS";
	case ADC_CH_PPVAR_USB_DUT_VBUS:
		return "PPVAR_USB_DUT_VBUS";
	case ADC_CH_PP5000_USB_HOST_VBUS:
		return "PP5000_USB_HOST_VBUS";
	case ADC_CH_PP5000:
		return "PP5000";
	default:
		return "[INVALID]";
	}
}

int adcs_init()
{
	int err;

	for(unsigned a = 0; a < ADC_CH_COUNT; a++) {
		if(!adc_channels[a].dev) {
			LOG_ERR("Skipping channel %s", adc_channel_to_str(a));
			continue;
		}

		LOG_DBG("Initializing ADC channel %s", adc_channel_to_str(a));

		if (!adc_is_ready_dt(&adc_channels[a])) {
			LOG_ERR("ADC controller for channel %s is not ready",
				adc_channel_to_str(a));
			continue;
		}

		err = adc_channel_setup_dt(&adc_channels[a]);
		if (err < 0) {
			LOG_ERR("Could not setup channel %s (%d)",
				adc_channel_to_str(a), err);
			continue;
		}
	}

	LOG_INF("Initialized ADC channels");
	return 0;
}

int adc_ch_read(enum adc_type ch)
{
	int err;
	uint16_t buf;
	int32_t val_mv;

	struct adc_sequence sequence = {
		.buffer = &buf,
		.buffer_size = sizeof(buf),
	};

	err = adc_sequence_init_dt(&adc_channels[ch], &sequence);
	if (err < 0) {
		LOG_ERR("Couldn't initialize sequence for ADC channel: %s",
			adc_channel_to_str(ch));
		return err;
	}

	err = adc_read_dt(&adc_channels[ch], &sequence);
	if (err < 0) {
		LOG_ERR("Couldn't read value of ADC channel: %s",
			adc_channel_to_str(ch));
		return err;
	}

	val_mv = (int32_t)buf;
	err = adc_raw_to_millivolts_dt(&adc_channels[ch], &val_mv);
	if (err < 0) {
		LOG_ERR("Couldn't convert value of ADC channel: %s",
			adc_channel_to_str(ch));
		return err;
	}

	return val_mv;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	int mv;

	shell_print(sh, "ADC values:");
	for(unsigned a = 0; a < ADC_CH_COUNT; a++)
	{
		mv = adc_ch_read(a);
		shell_print(sh, "[%20s] =% 5d mV", adc_channel_to_str(a), mv);
	}

	return 0;
}

static int cmd_read(const struct shell *sh, size_t argc, char **argv)
{
	if(argc != 2) {
		shell_print(sh, "Usage: adc read <channel_name>");
		shell_print(sh, "Available channels:");
		for(unsigned a = 0; a < ADC_CH_COUNT; a++)
		{
			shell_print(sh, "- %s", adc_channel_to_str(a));
		}

		return -EINVAL;
	}

	for(unsigned a = 0; a < ADC_CH_COUNT; a++)
	{
		if(strcmp(adc_channel_to_str(a), argv[1]))
			continue;

		shell_print(sh, "[%s] = %d mV",
			adc_channel_to_str(a), adc_ch_read(a));

		return 0;
	}

	shell_print(sh, "Invalid channel name");
	return -EINVAL;
}

static int cmd_temp(const struct shell *sh, size_t argc, char **argv)
{
	int mv = adc_ch_read(ADC_CH_TEMP_SENSOR);
	int sense0 = DL_FactoryRegion_getTemperatureVoltage();

#ifdef CONFIG_REQUIRES_FLOAT_PRINTF
	float vTrim = (3.3f / 4096.0f) * ((float)sense0 - 0.5f);
	float temp = (1.0f / (MSPM0G350X_TSC / 1000.0f)) *
		(mv / 1000.0f - vTrim) + MSPM0G350X_TSTRIM;
	shell_print(sh, "Temperature = %f 'C", (float)temp);
#else
	// The calculations are done in 1/100 of mV
	int vTrim = (sense0 * 10 - 5) * 33000 / 4096;
	int temp = ONE_BY_MSPM0G350X_TSC_INT_APROX * (mv * 100 - vTrim) / 100
		+ MSPM0G350X_TSTRIM * 100;

	shell_print(sh, "Temperature = %d.%02d 'C", temp / 100, temp % 100);
#endif

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_adc_cmds,
	SHELL_CMD(status, NULL, "Read all ADC channels", cmd_status),
	SHELL_CMD(read, NULL, "Read one channel", cmd_read),
	SHELL_SUBCMD_SET_END
);

SHELL_CMD_REGISTER(adc, &sub_adc_cmds, "Commands to manipulate ADC", NULL);
SHELL_CMD_REGISTER(temp, NULL, "Read internal temperature sensor", cmd_temp);
