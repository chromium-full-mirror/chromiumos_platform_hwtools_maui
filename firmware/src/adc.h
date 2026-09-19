/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#ifndef ADC_H_
#define ADC_H_

/**
 * Logical ADC channels, as the union of every channel any Maui board exposes.
 *
 * A given board only wires up a subset: it declares the ones it has in its
 * boards/<board>/adc.dtsi, and the rest are simply absent. For example the
 * IT82002 build has no BSL strap and reads temperature from an I2C TMP110, so
 * ADC_CH_BSL and ADC_CH_TEMP_SENSOR do not exist there. Reading an absent
 * channel returns -ENOTSUP, and the shell hides it.
 */
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
 * @brief Set up every ADC channel this board declares.
 *
 * Channels the board does not declare are skipped silently; they are a board
 * property, not a failure.
 *
 * @return 0 if every declared channel was set up, -EIO if any of them failed.
 */
int adcs_init(void);

/**
 * @brief Read voltage on specified ADC channel
 *
 * @param ch ADC channel that should be measured
 * @return Value in millivolts, -ENOTSUP if the channel does not exist on this
 *         board, or another negative errno on failure.
 */
int adc_ch_read(enum adc_type ch);

#endif /* ADC_H_ */
