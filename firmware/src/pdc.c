/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "pdc.h"

#include <stdio.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(pdc);

#if IS_ENABLED(CONFIG_PDC_DEBUG)

const struct device *pdc_uart = DEVICE_DT_GET(DT_NODELABEL(uart1));

void PDC_Uart_CB(const struct device *dev, void *user_data)
{
	static char v[16];
	static char msg[sizeof(v) * 2 + 5 + 1] = "PDC: ";
	unsigned v_size = 0;
	int pos = 5;

	v_size = uart_fifo_read(dev, v, sizeof(v));

	for(unsigned a = 0; a < v_size; a++)
	{
		pos += sprintf(msg + pos, "%02x", v[a]);
	}

	LOG_INF("%s", msg);
}
#endif

int pdc_init()
{
#if IS_ENABLED(CONFIG_PDC_DEBUG)
	if (!device_is_ready(pdc_uart)) {
		LOG_ERR("UART1 not ready~!");
		return -EIO;
	}

	if (uart_irq_callback_user_data_set(pdc_uart, PDC_Uart_CB, NULL) < 0) {
		LOG_ERR("UART1 no cb~!");
		return -ENOTSUP;
	}

	uart_irq_rx_enable(pdc_uart);
	LOG_INF("PDC debug enabled");
#endif

	return 0;
}