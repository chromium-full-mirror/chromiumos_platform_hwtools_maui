/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "usb_init.h"

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(usb_init, LOG_LEVEL_INF);

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK_NEXT)
#include <zephyr/usb/usbd.h>

/* Hardcoded USB Properties */
#define USB_VID_GOOGLE 0x18D1
#define USB_PID_MAUI 0x5085
/* TODO(b/535538021): Store persistent serial number in non-volatile flash and
 * read it at init during USB descriptor configuration.
 */
#define CUSTOM_SERIAL_STR "Uninitialized"

/* Instantiate the macro context pointing to your controller */
USBD_DEVICE_DEFINE(sample_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   USB_VID_GOOGLE, USB_PID_MAUI);

USBD_DESC_LANG_DEFINE(sample_lang);
USBD_DESC_MANUFACTURER_DEFINE(sample_mfr, "Google LLC");
USBD_DESC_PRODUCT_DEFINE(sample_product, "Maui Interface");

/*
 * Use the generic descriptor macro to declare the serial string.
 * This automatically adapts to the underlying Zephyr struct design.
 */
USBD_DESC_STRING_DEFINE(sample_sn, CUSTOM_SERIAL_STR,
			USBD_DUT_STRING_SERIAL_NUMBER);

USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(sample_fs_config, 0, 125, &fs_cfg_desc); /* Default
								      250mA max
								      power */

int init_usb(void)
{
	int ret;

	/* Setup standard descriptors into the device next-gen context */
	usbd_add_descriptor(&sample_usbd, &sample_lang);
	usbd_add_descriptor(&sample_usbd, &sample_mfr);
	usbd_add_descriptor(&sample_usbd, &sample_product);

	/* Inject our macro-built serial number pointer */
	ret = usbd_add_descriptor(&sample_usbd, &sample_sn);
	if (ret) {
		LOG_ERR("Failed to append custom serial descriptor (%d)", ret);
		return ret;
	}

	usbd_add_configuration(&sample_usbd, USBD_SPEED_FS, &sample_fs_config);
	usbd_register_all_classes(&sample_usbd, USBD_SPEED_FS, 1, NULL);

	ret = usbd_device_set_code_triple(&sample_usbd, USBD_SPEED_FS,
					  USB_BCC_MISCELLANEOUS, 0x02, 0x01);
	if (ret) {
		LOG_ERR("Failed to set device code triple (%d)", ret);
		return ret;
	}

	ret = usbd_init(&sample_usbd);
	if (ret) {
		LOG_ERR("Failed to init USB context");
		return ret;
	}

	/* Wait 250ms after UDC init to ensure host registers a physical USB
	 * disconnect on warm reboots */
	k_msleep(250);

	ret = usbd_enable(&sample_usbd);
	if (ret) {
		LOG_ERR("Failed to enable USB stack");
		return ret;
	}

	return 0;
}
#else
int init_usb(void)
{
	return 0;
}
#endif
