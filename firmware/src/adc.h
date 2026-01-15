/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef ADC_H_
#define ADC_H_

enum adc_type {
	ADC_CH_USB_HOST_CC1 = 0,
	ADC_CH_USB_HOST_CC2,
	ADC_CH_TEMP_SENSOR,
	ADC_CH_SBU1_DET,
	ADC_CH_SBU2_DET,
	ADC_CH_BSL,
	ADC_CH_PPVAR_USB_CHG_VBUS,
	ADC_CH_PPVAR_USB_DUT_VBUS,
	ADC_CH_PP5000_USB_HOST_VBUS,
	ADC_CH_PP5000,

	ADC_CH_COUNT
};

/**
 * @brief Initialize the ADC channels.
 *
 * @return 0
 * In case of errors, prints devices names that weren't initialized.
 */
int adcs_init();

/**
 * @brief Read voltage on specified ADC channel
 *
 * @param ch ADC channel that should be measured
 * @return Value in millivolts, or negative value in case of error
 */
int adc_ch_read(enum adc_type ch);

#endif /* ADC_H_ */
