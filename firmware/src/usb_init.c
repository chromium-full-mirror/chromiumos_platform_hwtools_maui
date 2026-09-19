/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "nvs_storage.h"
#include "usb_init.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(usb_init, LOG_LEVEL_INF);

#if IS_ENABLED(CONFIG_USB_DEVICE_STACK_NEXT)
#include <zephyr/usb/usbd.h>

/* USB VID/PID per Maui IT82002 Design Doc MAUI-FW-COM-4 (0x18D1:0x5085) */
#define USB_VID_GOOGLE 0x18D1
#define USB_PID_MAUI 0x5085

/* Global buffer holding persistent serial number read from NVS */
static char serial_str[NVS_SERIAL_MAX_LEN] = NVS_DEFAULT_SERIAL;

/* Instantiate USB device context */
USBD_DEVICE_DEFINE(sample_usbd, DEVICE_DT_GET(DT_NODELABEL(zephyr_udc0)),
		   USB_VID_GOOGLE, USB_PID_MAUI);

USBD_DESC_LANG_DEFINE(sample_lang);
USBD_DESC_MANUFACTURER_DEFINE(sample_mfr, "Google LLC");
USBD_DESC_PRODUCT_DEFINE(sample_product, "Maui Diagnostic Controller");

static struct usbd_desc_node sample_sn = {
	.str = {
		.utype = USBD_DUT_STRING_SERIAL_NUMBER,
		.ascii7 = true,
	},
	.ptr = serial_str,
	.bLength = 2 + (sizeof(NVS_DEFAULT_SERIAL) - 1) * 2,
	.bDescriptorType = USB_DESC_STRING,
};

USBD_DESC_CONFIG_DEFINE(fs_cfg_desc, "FS Configuration");
USBD_CONFIGURATION_DEFINE(sample_fs_config, 0, 125, &fs_cfg_desc);

int init_usb(void)
{
	int ret;

#ifdef CONFIG_MAUI_NVS_STORAGE
	/* Read persistent serial number from NVS before descriptor
	 * registration. Boards without NVS keep the NVS_DEFAULT_SERIAL compiled
	 * in above.
	 */
	(void)nvs_storage_init();
	(void)nvs_storage_get_serial(serial_str, sizeof(serial_str));
#endif
	sample_sn.bLength = 2 + strlen(serial_str) * 2;
	LOG_INF("USB Device Serial Number: %s", serial_str);

	usbd_add_descriptor(&sample_usbd, &sample_lang);
	usbd_add_descriptor(&sample_usbd, &sample_mfr);
	usbd_add_descriptor(&sample_usbd, &sample_product);
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

	/* Wait 250ms after UDC init to ensure host registers disconnect on warm
	 * reboots */
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
