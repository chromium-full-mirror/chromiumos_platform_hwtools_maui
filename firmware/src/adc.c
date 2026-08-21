/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "adc.h"

#include <errno.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>
#include <zephyr/shell/shell.h>

#if defined(CONFIG_SOC_FAMILY_TI_MSPM0) || defined(CONFIG_SOC_MSPM0G3507) || \
	defined(CONFIG_SOC_MSPM0G5117)
#include <m0p/dl_factoryregion.h>
#endif

// Datasheet for the family specifies this as <-2.1, -1.7>, with
// typical -1.8 mV/'C
#define MSPM0G350X_TSC -1.8
// Value -56 is approximation of (1.0 / MSPM0G350X_TSC * 100)
#define ONE_BY_MSPM0G350X_TSC_INT_APROX -56
// Datasheet for the family specifies this as <27, 33>, with typical 30
#define MSPM0G350X_TSTRIM 30

/*
 * Temperature sensor factory calibration reference voltage:
 * - MSPM0G3507 (Datasheet 8.13): The unit-specific calibration value
 * (TEMP_SENSE0) represents the 12-bit ADC result measured with the 3.3V VDD
 * reference (VRSEL=0h).
 * - MSPM0G5117 (Datasheet 8.14): The unit-specific calibration value
 * (TEMP_SENSE0) represents the 12-bit ADC result measured with the 1.4-V
 * internal VREF (VRSEL=2h, BUFCONFIG=1h).
 */
#if defined(CONFIG_SOC_MSPM0G5117) || defined(CONFIG_BOARD_MAUI_PROTO_5117)
#define FACTORY_TRIM_VREF_MV 1400
#else
#define FACTORY_TRIM_VREF_MV 3300
#endif

LOG_MODULE_REGISTER(adc);

#define DT_ADC_NODE_FROM_NODELABEL(node)                                   \
	COND_CODE_1(DT_NODE_EXISTS(DT_NODELABEL(node)),                    \
		    (ADC_DT_SPEC_STRUCT(DT_PARENT(DT_NODELABEL(node)),     \
					DT_PROP_BY_IDX(DT_NODELABEL(node), \
						       reg, 0))),          \
		    ({ 0 }))

static const struct adc_dt_spec adc_channels[] = {
	[ADC_CH_USB_HOST_CC1] = DT_ADC_NODE_FROM_NODELABEL(adc_usb_host_cc1),
	[ADC_CH_USB_HOST_CC2] = DT_ADC_NODE_FROM_NODELABEL(adc_usb_host_cc2),
	[ADC_CH_TEMP_SENSOR] = DT_ADC_NODE_FROM_NODELABEL(adc_temp_sensor),
	[ADC_CH_SBU1_DET] = DT_ADC_NODE_FROM_NODELABEL(adc_sbu1_det),
	[ADC_CH_SBU2_DET] = DT_ADC_NODE_FROM_NODELABEL(adc_sbu2_det),
	[ADC_CH_BSL] = DT_ADC_NODE_FROM_NODELABEL(adc_bsl),
	[ADC_CH_PPVAR_USB_CHG_VBUS] =
		DT_ADC_NODE_FROM_NODELABEL(adc_ppvar_usb_chg_vbus),
	[ADC_CH_PPVAR_USB_DUT_VBUS] =
		DT_ADC_NODE_FROM_NODELABEL(adc_ppvar_usb_dut_vbus),
	[ADC_CH_PP5000_USB_HOST_VBUS] =
		DT_ADC_NODE_FROM_NODELABEL(adc_pp5000_usb_host_vbus),
	[ADC_CH_PP5000] = DT_ADC_NODE_FROM_NODELABEL(adc_pp5000),
};

/*
 * A channel is "present" only if the board's adc.dtsi declares a node for it.
 * DT_ADC_NODE_FROM_NODELABEL() expands to {0} when it does not, leaving a NULL
 * device. Boards intentionally populate different subsets of enum adc_type:
 * the IT82002 has no BSL strap and measures temperature over I2C, so it wires
 * up 8 of the 10 logical channels.
 */
static bool adc_ch_present(enum adc_type ch)
{
	return ch < ADC_CH_COUNT && adc_channels[ch].dev != NULL;
}

static const char *adc_channel_name(enum adc_type ch)
{
	switch (ch) {
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

int adcs_init(void)
{
	unsigned int present = 0;
	unsigned int ready = 0;
	int err;

	for (unsigned int a = 0; a < ADC_CH_COUNT; a++) {
		if (!adc_ch_present(a)) {
			/* Expected on boards that do not wire this signal. */
			LOG_DBG("Channel %s not present on this board",
				adc_channel_name(a));
			continue;
		}
		present++;

		LOG_DBG("Initializing ADC channel %s", adc_channel_name(a));

		if (!adc_is_ready_dt(&adc_channels[a])) {
			LOG_ERR("ADC controller for channel %s is not ready",
				adc_channel_name(a));
			continue;
		}

		err = adc_channel_setup_dt(&adc_channels[a]);
		if (err < 0) {
			LOG_ERR("Could not setup channel %s (%d)",
				adc_channel_name(a), err);
			continue;
		}
		ready++;
	}

	LOG_INF("Initialized %u/%u ADC channels", ready, present);

	/* Only channels this board actually declares count as failures. */
	return (ready == present) ? 0 : -EIO;
}

/*
 * Voltage divider for USB VBUS channels:
 * VBUS -> 100k (R_top) -> ADC pin -> 12k (R_bot) -> GND
 * V_VBUS = V_ADC * (100k + 12k) / 12k
 */
#define VBUS_DIVIDER_R_TOP_KOHM 100
#define VBUS_DIVIDER_R_BOT_KOHM 12

int adc_ch_read(enum adc_type ch)
{
	int err;
	uint16_t buf;
	int32_t val_mv;

	if (!adc_ch_present(ch)) {
		return -ENOTSUP;
	}

	struct adc_sequence sequence = {
		.buffer = &buf,
		.buffer_size = sizeof(buf),
	};

	err = adc_sequence_init_dt(&adc_channels[ch], &sequence);
	if (err < 0) {
		LOG_ERR("Couldn't initialize sequence for ADC channel: %s",
			adc_channel_name(ch));
		return err;
	}

	err = adc_read_dt(&adc_channels[ch], &sequence);
	if (err < 0) {
		LOG_ERR("Couldn't read value of ADC channel: %s",
			adc_channel_name(ch));
		return err;
	}

	val_mv = (int32_t)buf;
	err = adc_raw_to_millivolts_dt(&adc_channels[ch], &val_mv);
	if (err < 0) {
		LOG_ERR("Couldn't convert value of ADC channel: %s",
			adc_channel_name(ch));
		return err;
	}

	if (ch == ADC_CH_PPVAR_USB_CHG_VBUS ||
	    ch == ADC_CH_PPVAR_USB_DUT_VBUS) {
		val_mv = (val_mv * (VBUS_DIVIDER_R_TOP_KOHM +
				    VBUS_DIVIDER_R_BOT_KOHM) +
			  (VBUS_DIVIDER_R_BOT_KOHM / 2)) /
			 VBUS_DIVIDER_R_BOT_KOHM;
	}

	return val_mv;
}

static int cmd_status(const struct shell *sh, size_t argc, char **argv)
{
	ARG_UNUSED(argc);
	ARG_UNUSED(argv);

	shell_print(sh, "ADC values:");
	for (unsigned int a = 0; a < ADC_CH_COUNT; a++) {
		int mv;

		if (!adc_ch_present(a)) {
			continue;
		}

		mv = adc_ch_read(a);
		if (mv < 0) {
			shell_print(sh, "[%20s] = error (%d)",
				    adc_channel_name(a), mv);
		} else {
			shell_print(sh, "[%20s] =% 5d mV", adc_channel_name(a),
				    mv);
		}
	}

	return 0;
}

static int cmd_read(const struct shell *sh, size_t argc, char **argv)
{
	if (argc != 2) {
		shell_print(sh, "Usage: adc read <channel_name>");
		shell_print(sh, "Available channels:");
		for (unsigned int a = 0; a < ADC_CH_COUNT; a++) {
			if (adc_ch_present(a)) {
				shell_print(sh, "- %s", adc_channel_name(a));
			}
		}

		return -EINVAL;
	}

	for (unsigned int a = 0; a < ADC_CH_COUNT; a++) {
		int mv;

		if (!adc_ch_present(a) ||
		    strcmp(adc_channel_name(a), argv[1]) != 0) {
			continue;
		}

		mv = adc_ch_read(a);
		if (mv < 0) {
			shell_error(sh, "Failed to read %s (%d)",
				    adc_channel_name(a), mv);
			return mv;
		}

		shell_print(sh, "[%s] = %d mV", adc_channel_name(a), mv);

		return 0;
	}

	shell_error(sh, "Unknown channel '%s' (not available on this board)",
		    argv[1]);
	return -EINVAL;
}

static int cmd_temp(const struct shell *sh, size_t argc, char **argv)
{
#if DT_HAS_ALIAS(temp_sensor) && IS_ENABLED(CONFIG_SENSOR)
	const struct device *dev = DEVICE_DT_GET(DT_ALIAS(temp_sensor));
	struct sensor_value val;
	int rc;

	if (!device_is_ready(dev)) {
		shell_error(sh, "Temperature sensor device %s is not ready",
			    dev->name);
		return -ENODEV;
	}

	rc = sensor_sample_fetch(dev);
	if (rc < 0) {
		shell_error(sh, "Failed to fetch sample from %s (%d)",
			    dev->name, rc);
		return rc;
	}

	rc = sensor_channel_get(dev, SENSOR_CHAN_AMBIENT_TEMP, &val);
	if (rc < 0) {
		shell_error(sh, "Failed to get ambient temp from %s (%d)",
			    dev->name, rc);
		return rc;
	}

	shell_print(sh, "Temperature = %d.%02d 'C", val.val1,
		    abs(val.val2) / 10000);
	return 0;
#elif defined(CONFIG_SOC_FAMILY_TI_MSPM0) || defined(CONFIG_SOC_MSPM0G3507) || \
	defined(CONFIG_SOC_MSPM0G5117)
	int mv = adc_ch_read(ADC_CH_TEMP_SENSOR);
	int sense0 = DL_FactoryRegion_getTemperatureVoltage();

#ifdef CONFIG_REQUIRES_FLOAT_PRINTF
	float vTrim = ((float)FACTORY_TRIM_VREF_MV / 1000.0f / 4096.0f) *
		      ((float)sense0 - 0.5f);
	float temp =
		(1.0f / (MSPM0G350X_TSC / 1000.0f)) * (mv / 1000.0f - vTrim) +
		MSPM0G350X_TSTRIM;
	shell_print(sh, "Temperature = %f 'C", (float)temp);
#else
	// The calculations are done in 1/100 of mV
	int vTrim = (sense0 * 10 - 5) * (FACTORY_TRIM_VREF_MV * 10) / 4096;
	int temp = ONE_BY_MSPM0G350X_TSC_INT_APROX * (mv * 100 - vTrim) / 100 +
		   MSPM0G350X_TSTRIM * 100;

	shell_print(sh, "Temperature = %d.%02d 'C", temp / 100, temp % 100);
#endif
	return 0;
#else
	shell_error(sh, "No temperature sensor configured");
	return -ENOTSUP;
#endif
}

SHELL_STATIC_SUBCMD_SET_CREATE(sub_adc_cmds,
			       SHELL_CMD(status, NULL, "Read all ADC channels",
					 cmd_status),
			       SHELL_CMD(read, NULL, "Read one channel",
					 cmd_read),
			       SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(adc, &sub_adc_cmds, "Commands to manipulate ADC", NULL);
SHELL_CMD_REGISTER(temp, NULL, "Read internal temperature sensor", cmd_temp);
