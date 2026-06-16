/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "tps6699x.h"
#include "tps6699x_reg.h"
#include "bypass.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/util.h>

#define DT_DRV_COMPAT ti_tps6699x

#if DT_NUM_INST_STATUS_OKAY(DT_DRV_COMPAT) != 1
#error Invalid number of ti_tps6699x instances
#endif

#define TPS_4CC_POLL_DELAY K_USEC(200)
#define TPS_RESET_DELAY K_MSEC(2000)

/** Split streaming transfers down into chunks of this size for more manageable
 *  I2C write lengths.
 *  Reduced to 24 to accommodate driver overhead (limit 32 bytes).
 */
#define TPS_STREAM_CHUNK_SIZE (24)

LOG_MODULE_REGISTER(tps6699x, CONFIG_LOG_DEFAULT_LEVEL);

/**
 * @brief Debug Accessory Advertisement Mode
 */
enum tps_debug_accessory_mode {
	DEBUG_ACCESSORY_AUTO = -1,
	DEBUG_ACCESSORY_OFF = 0,
	DEBUG_ACCESSORY_ON = 1,
};

struct tps6699x_config {
	struct i2c_dt_spec i2c;
#ifdef CONFIG_TPS6699X_INTERRUPTS
	struct gpio_dt_spec irq_gpio;
#endif
};

struct tps6699x_data {
	struct k_mutex lock;
#ifdef CONFIG_TPS6699X_INTERRUPTS
	struct k_work irq_work;
	struct k_work_delayable re_enable_work;
	struct gpio_callback irq_cb;
#endif
	const struct device *dev;
	int forced_debug_accessory_mode;
};

static const struct tps6699x_config tps6699x_config = {
	.i2c = I2C_DT_SPEC_INST_GET(0),
#ifdef CONFIG_TPS6699X_INTERRUPTS
	.irq_gpio = GPIO_DT_SPEC_INST_GET(0, irq_gpios),
#endif
};

static struct tps6699x_data tps6699x_data;

#ifdef CONFIG_PDC_DEBUG

const struct device *tps_uart = DEVICE_DT_GET(DT_NODELABEL(uart1));

void TPS_Uart_CB(const struct device *dev, void *user_data)
{
	static char v[16];
	static char msg[sizeof(v) * 2 + 5 + 1] = "PDC: ";
	unsigned v_size = 0;
	int pos = 5;

	v_size = uart_fifo_read(dev, v, sizeof(v));

	for (unsigned a = 0; a < v_size; a++) {
		pos += sprintf(msg + pos, "%02x", v[a]);
	}

	LOG_INF("%s", msg);
}

static int tps_uart_init()
{
	if (!device_is_ready(tps_uart)) {
		LOG_ERR("UART1 not ready~!");
		return -EIO;
	}

	if (uart_irq_callback_user_data_set(tps_uart, TPS_Uart_CB, NULL) < 0) {
		LOG_ERR("UART1 no cb~!");
		return -ENOTSUP;
	}

	uart_irq_rx_enable(tps_uart);
	LOG_INF("PDC debug enabled");

	return 0;
}
#endif

static int tps_read_reg(const struct i2c_dt_spec *i2c, enum tps6699x_reg reg,
			uint8_t *buf, uint8_t len)
{
	uint8_t byte_cnt;

	/* TPS Read Protocol
	 *   1. Write of register to be read
	 *   2. Read byte count
	 *   3. Read register contents
	 */
	struct i2c_msg msg[] = {
		{
			.buf = (uint8_t *)&reg,
			.len = 1,
			.flags = I2C_MSG_WRITE,
		},
		{
			.buf = &byte_cnt,
			.len = 1,
			.flags = I2C_MSG_READ | I2C_MSG_RESTART,
		},
		{
			.buf = buf,
			.len = len,
			.flags = I2C_MSG_READ | I2C_MSG_STOP,
		},
	};

	return i2c_transfer_dt(i2c, msg, ARRAY_SIZE(msg));
}

static int tps_write_reg(const struct i2c_dt_spec *i2c, enum tps6699x_reg reg,
			 uint8_t *buf, uint8_t len)
{
	/* TPS Write Protocol
	 *   1. Write Register
	 *   2. Write Byte Count
	 *   3. Write data
	 */
	struct i2c_msg msg[] = {
		{
			.buf = (uint8_t *)&reg,
			.len = 1,
			.flags = I2C_MSG_WRITE,
		},
		{
			.buf = &len,
			.len = 1,
			.flags = I2C_MSG_WRITE,
		},
		{
			.buf = buf,
			.len = len,
			.flags = I2C_MSG_WRITE | I2C_MSG_STOP,
		},
	};

	return i2c_transfer_dt(i2c, msg, ARRAY_SIZE(msg));
}

static int tps_xfer_reg(const struct i2c_dt_spec *i2c, enum tps6699x_reg reg,
			uint8_t *buf, uint8_t len, int flag)
{
	if (!i2c || (!buf && len > 0)) {
		return -EINVAL;
	}

	if (flag == I2C_MSG_READ) {
		return tps_read_reg(i2c, reg, buf, len);
	} else {
		return tps_write_reg(i2c, reg, buf, len);
	}
}

int tps_rd_mode(const struct device *dev, union reg_mode *buf)
{
	const struct tps6699x_config *config = dev->config;
	struct tps6699x_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = tps_xfer_reg(&config->i2c, REG_MODE, buf->raw_value,
			   sizeof(union reg_mode), I2C_MSG_READ);
	k_mutex_unlock(&data->lock);

	return ret;
}

int tps_rw_command_for_i2c1(const struct device *dev, union reg_command *buf,
			    int flag)
{
	const struct tps6699x_config *config = dev->config;
	struct tps6699x_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = tps_xfer_reg(&config->i2c, REG_COMMAND_FOR_I2C1, buf->raw_value,
			   sizeof(union reg_command), flag);
	k_mutex_unlock(&data->lock);

	return ret;
}

int tps_rw_data_for_cmd1(const struct device *dev, union reg_data *buf,
			 size_t len, int flag)
{
	const struct tps6699x_config *config = dev->config;
	struct tps6699x_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = tps_xfer_reg(&config->i2c, REG_DATA_FOR_CMD1, buf->raw_value, len,
			   flag);
	k_mutex_unlock(&data->lock);

	return ret;
}

int tps_stream_data(const struct device *dev, const uint8_t broadcast_address,
		    const uint8_t *buf, size_t buf_len)
{
	const struct tps6699x_config *config = dev->config;
	struct tps6699x_data *data = dev->data;
	struct i2c_msg msg;
	int rv = 0;

	/* Create new i2c target for transfer. */
	const struct i2c_dt_spec stream_i2c = {
		.bus = config->i2c.bus,
		.addr = (uint16_t)broadcast_address,
	};

	k_mutex_lock(&data->lock, K_FOREVER);

	/* Perform the transfer in chunks */
	for (int chunk_offset = 0; chunk_offset < buf_len;
	     chunk_offset += TPS_STREAM_CHUNK_SIZE) {
		/* Set up I2C write */
		msg.buf = (uint8_t *)buf + chunk_offset;
		msg.len = MIN(TPS_STREAM_CHUNK_SIZE, buf_len - chunk_offset);
		msg.flags = I2C_MSG_WRITE | I2C_MSG_STOP;

		rv = i2c_transfer_dt(&stream_i2c, &msg, 1);
		if (rv) {
			LOG_ERR("Streaming data block failed (ret=%d, "
				"offset_into_block=%d, total_block_size=%u,"
				"chunk_size=%d)",
				rv, chunk_offset, buf_len,
				TPS_STREAM_CHUNK_SIZE);
			break;
		}
	}

	k_mutex_unlock(&data->lock);

	if (rv == 0) {
		LOG_DBG("  Block complete (%u)", buf_len);
	}
	return rv;
}

int tps_rd_version(const struct device *dev, union reg_version *buf)
{
	const struct tps6699x_config *config = dev->config;
	struct tps6699x_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = tps_xfer_reg(&config->i2c, REG_VERSION, buf->raw_value,
			   sizeof(union reg_version), I2C_MSG_READ);
	k_mutex_unlock(&data->lock);

	return ret;
}

int tps_rd_customer_use(const struct device *dev, union reg_customer_use *buf)
{
	const struct tps6699x_config *config = dev->config;
	struct tps6699x_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = tps_xfer_reg(&config->i2c, REG_CUSTOMER_USE, buf->raw_value,
			   sizeof(union reg_customer_use), I2C_MSG_READ);
	k_mutex_unlock(&data->lock);

	return ret;
}

/**
 * @brief Convert a 4CC command/task enum to a NUL-terminated printable string
 *
 * @param task The 4CC task enum
 * @param str_out Pointer to a char array capable of holding 5 characters where
 *        the output will be written to.
 */
static void command_task_to_string(enum command_task task, char str_out[5])
{
	if (task == 0) {
		const char no_task[5] = "0000";
		strncpy(str_out, no_task, strlen(no_task));
		return;
	}

	str_out[0] = (((uint32_t)task) >> 0);
	str_out[1] = (((uint32_t)task) >> 8);
	str_out[2] = (((uint32_t)task) >> 16);
	str_out[3] = (((uint32_t)task) >> 24);
	str_out[4] = '\0';
}

int run_task_sync(const struct device *dev, enum command_task task,
		  union reg_data *cmd_data, size_t write_len, size_t read_len,
		  uint8_t *user_buf)
{
	const struct tps6699x_config *config = dev->config;
	struct tps6699x_data *data = dev->data;
	int64_t timeout;
	union reg_command cmd;
	int rv;
	char task_str[5];

	command_task_to_string(task, task_str);

	k_mutex_lock(&data->lock, K_FOREVER);

	/* Set up self-contained synchronous command call */
	if (cmd_data) {
		rv = tps_xfer_reg(&config->i2c, REG_DATA_FOR_CMD1,
				  cmd_data->raw_value, write_len,
				  I2C_MSG_WRITE);
		if (rv) {
			LOG_ERR("Cannot set command data for '%s' (%d)",
				task_str, rv);
			goto unlock_exit;
		}
	}

	cmd.command = task;

	rv = tps_xfer_reg(&config->i2c, REG_COMMAND_FOR_I2C1, cmd.raw_value,
			  sizeof(union reg_command), I2C_MSG_WRITE);
	if (rv) {
		LOG_ERR("Cannot set command for '%s' (%d)", task_str, rv);
		goto unlock_exit;
	}

	/* Poll for successful completion */
	timeout = k_uptime_get() + 1200;

	while (1) {
		k_sleep(TPS_4CC_POLL_DELAY);

		rv = tps_xfer_reg(&config->i2c, REG_COMMAND_FOR_I2C1,
				  cmd.raw_value, sizeof(union reg_command),
				  I2C_MSG_READ);
		if (rv) {
			LOG_ERR("Cannot poll command status for '%s' (%d)",
				task_str, rv);
			goto unlock_exit;
		}

		if (cmd.command == 0) {
			/* Command complete */
			break;
		} else if (cmd.command == 0x444d4321) {
			/* Unknown command ("!CMD") */
			LOG_ERR("Command '%s' is invalid", task_str);
			rv = -1;
			goto unlock_exit;
		}

		if (k_uptime_get() > timeout) {
			LOG_ERR("Command '%s' timed out", task_str);
			rv = -ETIMEDOUT;
			goto unlock_exit;
		}
	}

	LOG_INF("Command '%s' finished...", task_str);

	/* Read out success code */
	static union reg_data cmd_data_check;
	/* Ensure we read at least 1 byte for status check */
	size_t actual_read_len = (read_len > 0) ? read_len : 1;

	rv = tps_xfer_reg(&config->i2c, REG_DATA_FOR_CMD1,
			  cmd_data_check.raw_value, actual_read_len,
			  I2C_MSG_READ);
	if (rv) {
		LOG_ERR("Cannot get command result status for '%s' (%d)",
			task_str, rv);
		goto unlock_exit;
	}

	/* Data byte offset 0 is the return error code */
	if (cmd_data_check.data[0] != 0) {
		LOG_ERR("Command '%s' failed. Chip says %02x", task_str,
			cmd_data_check.data[0]);
		rv = -EIO; /* Or specific error code */
		goto unlock_exit;
	}

	LOG_INF("Command '%s' succeeded!!", task_str);

	/* Provide response data to user if a buffer is provided */
	if (user_buf != NULL) {
		memcpy(user_buf, cmd_data_check.data, actual_read_len);
	}

unlock_exit:
	k_mutex_unlock(&data->lock);
	return rv;
}

int tps_cmd_sbud(const struct device *dev, uint8_t en)
{
	union reg_data cmd_data;
	int ret;

	memset(&cmd_data, 0, sizeof(cmd_data));
	cmd_data.data[0] = en;

	ret = run_task_sync(dev, COMMAND_TASK_SBUD, &cmd_data, 1, 0, NULL);

	return ret;
}

int tps_cmd_sbdf(const struct device *dev, uint8_t flip)
{
	union reg_data cmd_data;
	int ret;

	memset(&cmd_data, 0, sizeof(cmd_data));
	cmd_data.data[0] = flip;

	ret = run_task_sync(dev, COMMAND_TASK_SBDF, &cmd_data, 1, 0, NULL);

	return ret;
}

int tps_cmd_data_role_swap(const struct device *dev, enum tps_pd_data_role role)
{
	enum command_task task = (role == TPS_PD_DATA_ROLE_DFP) ?
					 COMMAND_TASK_SWDF :
					 COMMAND_TASK_SWUF;

	return run_task_sync(dev, task, NULL, 0, 0, NULL);
}

int tps_cmd_disc(const struct device *dev, uint8_t delay)
{
	union reg_data cmd_data;
	int ret;

	memset(&cmd_data, 0, sizeof(cmd_data));
	cmd_data.data[0] = delay;

	ret = run_task_sync(dev, COMMAND_TASK_DISC, &cmd_data, 1, 0, NULL);

	return ret;
}

int tps_cmd_gaid(const struct device *dev, bool switch_banks)
{
	union reg_data cmd_data;
	union gaid_params params;
	int ret;

	params.switch_banks = switch_banks ? GAID_MAGIC_VALUE : 0;
	params.copy_banks = 0;

	memcpy(cmd_data.data, &params, sizeof(params));

	ret = run_task_sync(dev, COMMAND_TASK_GAID, &cmd_data, sizeof(params),
			    1, NULL);

	if (ret == 0) {
		k_sleep(TPS_RESET_DELAY);
	}

	return ret;
}

int tps_discover_identity(const struct device *dev,
			  union reg_rx_identity_sop *rx_id)
{
	const struct tps6699x_config *config = dev->config;
	struct tps6699x_data *data = dev->data;
	int rv;

	/* Issue the AMDS task to start discovery */
	rv = run_task_sync(dev, COMMAND_TASK_AMDS, NULL, 0, 0, NULL);
	if (rv) {
		return rv;
	}

	/* Read the result from the register */
	k_mutex_lock(&data->lock, K_FOREVER);
	rv = tps_read_reg(&config->i2c, REG_RECEIVED_SOP_IDENTITY_DATA_OBJECT,
			  rx_id->raw_value, sizeof(union reg_rx_identity_sop));
	k_mutex_unlock(&data->lock);

	return rv;
}

int tps_rd_status(const struct device *dev, union reg_status *buf)
{
	const struct tps6699x_config *config = dev->config;
	struct tps6699x_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = tps_xfer_reg(&config->i2c, REG_STATUS, buf->raw_value,
			   sizeof(union reg_status), I2C_MSG_READ);
	k_mutex_unlock(&data->lock);

	return ret;
}

int tps_rd_port_config(const struct device *dev, union reg_port_config *buf)
{
	const struct tps6699x_config *config = dev->config;
	struct tps6699x_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = tps_xfer_reg(&config->i2c, REG_PORT_CONFIGURATION, buf->raw_value,
			   sizeof(union reg_port_config), I2C_MSG_READ);
	k_mutex_unlock(&data->lock);

	return ret;
}

int tps_wr_port_config(const struct device *dev, union reg_port_config *buf)
{
	const struct tps6699x_config *config = dev->config;
	struct tps6699x_data *data = dev->data;
	int ret;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = tps_xfer_reg(&config->i2c, REG_PORT_CONFIGURATION, buf->raw_value,
			   sizeof(union reg_port_config), I2C_MSG_WRITE);
	k_mutex_unlock(&data->lock);

	return ret;
}

void tps_decode_identity(const union reg_rx_identity_sop *rx_id,
			 struct tps_pd_identity *id)
{
	memset(id, 0, sizeof(*id));

	if (rx_id->response_type != 1) { /* Not ACK */
		return;
	}

	if (rx_id->num_valid_vdos > 0) {
		/* ID Header VDO is at index 0 */
		uint32_t id_header = rx_id->vdo[0];

		id->usb_host = (id_header >> 31) & 1;
		id->usb_device = (id_header >> 30) & 1;
		id->vid = id_header & 0xFFFF;
	}

	if (rx_id->num_valid_vdos > 2) {
		/* Product VDO is at index 2 */
		uint32_t product_vdo = rx_id->vdo[2];

		id->pid = (product_vdo >> 16) & 0xFFFF;
	}
}

#ifdef CONFIG_TPS6699X_INTERRUPTS
static void tps_handle_new_contract(struct tps6699x_data *data)
{
	const struct device *dev = data->dev;
	int rv;
	union reg_rx_identity_sop rx_id;
	struct tps_pd_identity id;
	union reg_status status;

	LOG_INF("Handling New Contract As Provider");

	/* Issue the AMDs 4CC command to force discovery */
	rv = tps_discover_identity(dev, &rx_id);
	if (rv) {
		LOG_ERR("Failed to discover identity: %d", rv);
		return;
	}

	tps_decode_identity(&rx_id, &id);

	/* Read current status to check data role */
	rv = tps_rd_status(dev, &status);
	if (rv) {
		LOG_ERR("Failed to read status: %d", rv);
		return;
	}

	/*
	 * For Pixel Phone, Maui should stay as DFP.
	 * For Chromebook, Maui should swap to UFP if possible.
	 * DFP/UFP roles need to be aligned with Mux direction.
	 */
	if (id.usb_host == 1 && id.usb_device == 1) {
		LOG_INF("Detected Android Phone");

		if (status.data_role == TPS_PD_DATA_ROLE_UFP) {
			LOG_INF("Swapping to DFP for Phone");
			tps_cmd_data_role_swap(dev, TPS_PD_DATA_ROLE_DFP);
		}

		bypass_set_mode(BYPASS_MODE_DIRECT);
	} else if (id.usb_host == 1 && id.usb_device == 0) {
		LOG_INF("Detected Chromebook");

		if (status.data_role == TPS_PD_DATA_ROLE_DFP) {
			LOG_INF("Swapping to UFP for Chromebook");
			tps_cmd_data_role_swap(dev, TPS_PD_DATA_ROLE_UFP);
		}

		bypass_set_mode(BYPASS_MODE_H2H);
	} else {
		LOG_INF("Detected Unknown Device (Host=%d, Dev=%d)",
			id.usb_host, id.usb_device);

		bypass_set_mode(BYPASS_MODE_H2H);
	}
}

static void tps_handle_device_incompatible(struct tps6699x_data *data)
{
	const struct device *dev = data->dev;
	union reg_port_config port_cfg;
	int rv;

	if (data->forced_debug_accessory_mode != DEBUG_ACCESSORY_AUTO) {
		return;
	}

	LOG_INF("Handling Device Incompatible Error");

	/* Read Port Configuration (0x28) */
	rv = tps_rd_port_config(dev, &port_cfg);
	if (rv) {
		LOG_ERR("Failed to read Port Config: %d", rv);
		return;
	}

	if (port_cfg.debug_accessory_ad_enable == 0) {
		return;
	}

	/* Disable Debug Accessory Advertisement by clearing bit 6 */
	port_cfg.debug_accessory_ad_enable = 0;

	/* Write back Port Configuration (0x28) */
	rv = tps_wr_port_config(dev, &port_cfg);
	if (rv) {
		LOG_ERR("Failed to write Port Config: %d", rv);
		return;
	}

	LOG_INF("Debug Accessory Advertisement disabled on Port A");
}

static void tps_re_enable_work_handler(struct k_work *work)
{
	struct k_work_delayable *dwork = k_work_delayable_from_work(work);
	struct tps6699x_data *data =
		CONTAINER_OF(dwork, struct tps6699x_data, re_enable_work);
	const struct device *dev = data->dev;
	union reg_status status;
	union reg_port_config port_cfg;
	int rv;

	/* Read Status (0x1A) to check plug presence */
	rv = tps_rd_status(dev, &status);
	if (rv) {
		LOG_ERR("Failed to read Status: %d", rv);
		return;
	}

	if (status.plug_present != 0) {
		/* Device re-connected or never disconnected, nothing to do */
		return;
	}

	/* Reset Mux to default Bridge Mode */
	bypass_set_mode(BYPASS_MODE_H2H);

	/* Read Port Configuration (0x28) */
	rv = tps_rd_port_config(dev, &port_cfg);
	if (rv) {
		LOG_ERR("Failed to read Port Config: %d", rv);
		return;
	}

	if (port_cfg.debug_accessory_ad_enable == 1) {
		/* Already enabled, nothing to do */
		return;
	}

	/* Re-enable Debug Accessory Advertisement by setting bit 6 */
	port_cfg.debug_accessory_ad_enable = 1;

	/* Write back Port Configuration (0x28) */
	rv = tps_wr_port_config(dev, &port_cfg);
	if (rv) {
		LOG_ERR("Failed to write Port Config: %d", rv);
		return;
	}

	LOG_INF("Debug Accessory Advertisement re-enabled on Port A");
}

static void tps_handle_plug_insert_or_removal(struct tps6699x_data *data)
{
	const struct device *dev = data->dev;
	union reg_status status;
	int rv;

	if (data->forced_debug_accessory_mode != DEBUG_ACCESSORY_AUTO) {
		return;
	}

	/* Read Status (0x1A) to check plug presence */
	rv = tps_rd_status(dev, &status);
	if (rv) {
		LOG_ERR("Failed to read Status: %d", rv);
		return;
	}

	LOG_INF("Handling Plug Insert or Removal (plug_present=%d)",
		status.plug_present);

	/* If plug is not present (disconnection), reset debug accessory
	 * advertisement to default (enabled) */
	if (status.plug_present == 0) {
		LOG_INF("Disconnection detected, scheduling Port A config reset");

		/* Schedule re-enabling after a delay to avoid loops and
		 * debounce. 500ms is chosen to be longer than a typical port
		 * reset duration.
		 */
		k_work_reschedule(&data->re_enable_work, K_MSEC(500));
	} else {
		LOG_INF("Connection detected, canceling any pending Port A config reset");
		/* Cancel any pending re-enable if a device is connected */
		k_work_cancel_delayable(&data->re_enable_work);
	}
}

static void tps_irq_work_handler(struct k_work *work)
{
	struct tps6699x_data *data =
		CONTAINER_OF(work, struct tps6699x_data, irq_work);
	const struct device *dev = data->dev;
	const struct tps6699x_config *config = dev->config;
	union reg_interrupt irq_evt1 = { 0 };
	int rv;
	int i;
	bool irq1_pending = false;

	/* Read the pending interrupt events */
	k_mutex_lock(&data->lock, K_FOREVER);
	rv = tps_read_reg(&config->i2c, REG_INTERRUPT_EVENT_FOR_I2C1,
			  irq_evt1.raw_value, sizeof(union reg_interrupt));
	k_mutex_unlock(&data->lock);
	if (rv) {
		LOG_ERR("Read IRQ1 events failed (%d)", rv);
		return;
	}

	/* Check for pending events in I2C1 */
	for (i = 0; i < sizeof(union reg_interrupt); i++) {
		if (irq_evt1.raw_value[i]) {
			irq1_pending = true;
		}
	}

	if (irq1_pending) {
		LOG_DBG("IRQ1 Event: 0x%02x%02x%02x%02x...",
			irq_evt1.raw_value[0], irq_evt1.raw_value[1],
			irq_evt1.raw_value[2], irq_evt1.raw_value[3]);

		/* Process I2C1 Interrupts */
		if (irq_evt1.new_contract_as_producer ||
		    irq_evt1.new_contract_as_consumer) {
			tps_handle_new_contract(data);
		}
		if (irq_evt1.device_incompatible_error) {
			tps_handle_device_incompatible(data);
		}
		if (irq_evt1.plug_insert_or_removal) {
			tps_handle_plug_insert_or_removal(data);
		}

		/* Clear handled interrupts */
		k_mutex_lock(&data->lock, K_FOREVER);
		tps_write_reg(&config->i2c, REG_INTERRUPT_CLEAR_FOR_I2C1,
			      irq_evt1.raw_value, sizeof(union reg_interrupt));
		k_mutex_unlock(&data->lock);
	}
}

static void tps_irq_handler(const struct device *dev, struct gpio_callback *cb,
			    uint32_t pins)
{
	struct tps6699x_data *data =
		CONTAINER_OF(cb, struct tps6699x_data, irq_cb);
	k_work_submit(&data->irq_work);
}
#endif

static int cmd_pdc_sbud_handler(const struct shell *sh, size_t argc,
				char **argv)
{
	const struct device *dev = DEVICE_DT_GET_ONE(ti_tps6699x);
	int rv;
	long val;
	char *endptr;

	if (!device_is_ready(dev)) {
		shell_error(sh, "TPS6699x device not ready");
		return -ENODEV;
	}

	val = strtol(argv[1], &endptr, 0);
	if (*endptr != '\0' || (val != 0 && val != 1)) {
		shell_error(sh, "Invalid argument. Must be 0 or 1.");
		return -EINVAL;
	}

	shell_print(sh, "Sending SBUd command with data: %ld", val);

	rv = tps_cmd_sbud(dev, (uint8_t)val);
	if (rv) {
		shell_error(sh, "SBUd command failed: %d", rv);
		return rv;
	}

	shell_print(sh, "SBUd command successful");
	return 0;
}

static int cmd_pdc_sbdf_handler(const struct shell *sh, size_t argc,
				char **argv)
{
	const struct device *dev = DEVICE_DT_GET_ONE(ti_tps6699x);
	int rv;
	long val;
	char *endptr;

	if (!device_is_ready(dev)) {
		shell_error(sh, "TPS6699x device not ready");
		return -ENODEV;
	}

	val = strtol(argv[1], &endptr, 0);
	if (*endptr != '\0' || (val != 0 && val != 1)) {
		shell_error(sh, "Invalid argument. Must be 0 or 1.");
		return -EINVAL;
	}

	shell_print(sh, "Sending SBDF command with data: %ld", val);

	rv = tps_cmd_sbdf(dev, (uint8_t)val);
	if (rv) {
		shell_error(sh, "SBDF command failed: %d", rv);
		return rv;
	}

	shell_print(sh, "SBDF command successful");
	return 0;
}

static int cmd_pdc_dr_swap_handler(const struct shell *sh, size_t argc,
				   char **argv)
{
	const struct device *dev = DEVICE_DT_GET_ONE(ti_tps6699x);
	int rv;
	enum tps_pd_data_role role;

	if (!device_is_ready(dev)) {
		shell_error(sh, "TPS6699x device not ready");
		return -ENODEV;
	}

	if (strcmp(argv[1], "dfp") == 0) {
		role = TPS_PD_DATA_ROLE_DFP;
	} else if (strcmp(argv[1], "ufp") == 0) {
		role = TPS_PD_DATA_ROLE_UFP;
	} else {
		shell_error(sh, "Invalid argument. Use dfp|ufp");
		return -EINVAL;
	}

	shell_print(sh, "Sending DR_Swap command to become %s", argv[1]);

	rv = tps_cmd_data_role_swap(dev, role);
	if (rv) {
		shell_error(sh, "DR_Swap command failed: %d", rv);
		return rv;
	}

	shell_print(sh, "DR_Swap command successful");
	return 0;
}

static int cmd_pdc_status_handler(const struct shell *sh, size_t argc,
				  char **argv)
{
	const struct device *dev = DEVICE_DT_GET_ONE(ti_tps6699x);
	union reg_status status;
	int rv;

	if (!device_is_ready(dev)) {
		shell_error(sh, "TPS6699x device not ready");
		return -ENODEV;
	}

	rv = tps_rd_status(dev, &status);
	if (rv) {
		shell_error(sh, "Failed to read Status: %d", rv);
		return rv;
	}

	shell_print(sh, "PDC Status:");
	shell_print(sh, "  Plug Present:     %s",
		    status.plug_present ? "Yes" : "No");

	if (status.plug_present) {
		shell_print(sh, "  Connection State: %d",
			    status.connection_state);
		shell_print(sh, "  Orientation:      %s",
			    status.plug_orientation ? "Flipped (CC2)" :
						      "Normal (CC1)");
		shell_print(sh, "  Power Role:       %s",
			    status.port_role ? "Source" : "Sink");
		shell_print(sh, "  Data Role:        %s",
			    status.data_role ? "DFP (Host)" : "UFP (Device)");
		shell_print(sh, "  EPR Mode Active:  %s",
			    status.epr_mode_is_active ? "Yes" : "No");
	}

	return 0;
}

static int cmd_pdc_info_handler(const struct shell *sh, size_t argc,
				char **argv)
{
	int rv;
	const struct device *dev;
	union reg_mode mode;
	union reg_version version;
	union reg_customer_use customer_use;

	dev = DEVICE_DT_GET_ONE(ti_tps6699x);

	if (!device_is_ready(dev)) {
		shell_error(sh, "TPS6699x device not ready");
		return -ENODEV;
	}

	shell_print(sh, "Using TPS6699x device: %s", dev->name);

	rv = tps_rd_mode(dev, &mode);
	if (rv) {
		shell_error(sh, "Failed to read Mode: %d", rv);
		return rv;
	}
	shell_print(sh, "Mode: %c%c%c%c", mode.data[0], mode.data[1],
		    mode.data[2], mode.data[3]);

	rv = tps_rd_version(dev, &version);
	if (rv) {
		shell_error(sh, "Failed to read Version: %d", rv);
		return rv;
	}
	shell_print(sh, "Version: %08x", version.version);

	rv = tps_rd_customer_use(dev, &customer_use);
	if (rv) {
		shell_error(sh, "Failed to read Customer Version: %d", rv);
		return rv;
	}
	shell_print(sh, "App Config Version: %08x",
		    customer_use.app_config_version);
	shell_print(sh, "FW Version: %08x", customer_use.fw_version);

	return 0;
}

static int cmd_pdc_discovery_handler(const struct shell *sh, size_t argc,
				     char **argv)
{
	const struct device *dev = DEVICE_DT_GET_ONE(ti_tps6699x);
	int rv;
	union reg_rx_identity_sop rx_id;
	struct tps_pd_identity id;

	if (!device_is_ready(dev)) {
		shell_error(sh, "TPS6699x device not ready");
		return -ENODEV;
	}

	shell_print(sh, "Starting PD Discovery...");

	rv = tps_discover_identity(dev, &rx_id);
	if (rv) {
		shell_error(sh, "Discovery failed: %d", rv);
		return rv;
	}

	/* 1. Print Raw Data */
	shell_print(sh, "Raw Identity Data:");
	shell_print(sh, "  Response Type: %d", rx_id.response_type);
	shell_print(sh, "  Num VDOs: %d", rx_id.num_valid_vdos);

	for (int i = 0; i < rx_id.num_valid_vdos && i < 6; i++) {
		shell_print(sh, "  VDO%d: 0x%08x", i, rx_id.vdo[i]);
	}

	if (rx_id.response_type != 1) {
		shell_print(sh, "  (No ACK response)");
		return 0;
	}

	/* 2. Print Decoded Values */
	tps_decode_identity(&rx_id, &id);

	shell_print(sh, "Decoded Identity:");
	shell_print(sh, "  USB Host: %d", id.usb_host);
	shell_print(sh, "  USB Dev : %d", id.usb_device);
	shell_print(sh, "  VID     : 0x%04x", id.vid);
	shell_print(sh, "  PID     : 0x%04x", id.pid);

	return 0;
}

static int cmd_pdc_debug_accessory_handler(const struct shell *sh, size_t argc,
					   char **argv)
{
	const struct device *dev = DEVICE_DT_GET_ONE(ti_tps6699x);
	struct tps6699x_data *data = dev->data;
	union reg_port_config port_cfg;
	int rv;

	if (!device_is_ready(dev)) {
		shell_error(sh, "TPS6699x device not ready");
		return -ENODEV;
	}

	if (strcmp(argv[1], "auto") == 0) {
		data->forced_debug_accessory_mode = DEBUG_ACCESSORY_AUTO;
		rv = tps_rd_port_config(dev, &port_cfg);
		if (rv) {
			shell_error(sh, "Read failed: %d", rv);
			return rv;
		}
		port_cfg.debug_accessory_ad_enable = 1;
		rv = tps_wr_port_config(dev, &port_cfg);
		if (rv) {
			shell_error(sh, "Write failed: %d", rv);
			return rv;
		}
		shell_print(sh, "Debug Accessory set to auto mode");
	} else if (strcmp(argv[1], "on") == 0) {
		data->forced_debug_accessory_mode = DEBUG_ACCESSORY_ON;
		rv = tps_rd_port_config(dev, &port_cfg);
		if (rv) {
			shell_error(sh, "Read failed: %d", rv);
			return rv;
		}
		port_cfg.debug_accessory_ad_enable = 1;
		rv = tps_wr_port_config(dev, &port_cfg);
		if (rv) {
			shell_error(sh, "Write failed: %d", rv);
			return rv;
		}
		shell_print(sh, "Debug Accessory forced ON");
	} else if (strcmp(argv[1], "off") == 0) {
		data->forced_debug_accessory_mode = DEBUG_ACCESSORY_OFF;
		rv = tps_rd_port_config(dev, &port_cfg);
		if (rv) {
			shell_error(sh, "Read failed: %d", rv);
			return rv;
		}
		port_cfg.debug_accessory_ad_enable = 0;
		rv = tps_wr_port_config(dev, &port_cfg);
		if (rv) {
			shell_error(sh, "Write failed: %d", rv);
			return rv;
		}
		shell_print(sh, "Debug Accessory forced OFF");
	} else {
		shell_error(sh, "Invalid argument. Use auto|on|off");
		return -EINVAL;
	}

	return 0;
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_pdc_cmds,
	SHELL_CMD_ARG(sbud, NULL, "Send SBUd command <0|1>",
		      cmd_pdc_sbud_handler, 2, 0),
	SHELL_CMD_ARG(sbdf, NULL, "Send SBDF command <0|1>",
		      cmd_pdc_sbdf_handler, 2, 0),
	SHELL_CMD_ARG(dr_swap, NULL, "Send DR_Swap command <dfp|ufp>",
		      cmd_pdc_dr_swap_handler, 2, 0),
	SHELL_CMD_ARG(status, NULL, "Read and print current status",
		      cmd_pdc_status_handler, 1, 0),
	SHELL_CMD_ARG(info, NULL, "Read and print chip info",
		      cmd_pdc_info_handler, 1, 0),
	SHELL_CMD_ARG(discovery, NULL,
		      "Trigger PD Discovery and print Identity",
		      cmd_pdc_discovery_handler, 1, 0),
	SHELL_CMD_ARG(debug_accessory, NULL,
		      "Toggle Debug Accessory mode <auto|on|off>",
		      cmd_pdc_debug_accessory_handler, 2, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(pdc, &sub_pdc_cmds, "PDC commands", NULL);

#ifdef CONFIG_TPS6699X_INTERRUPTS
static int tps_init_interrupt(const struct device *dev)
{
	const struct tps6699x_config *cfg = dev->config;
	struct tps6699x_data *data = dev->data;
	int ret;

	/* Robustness delay: give PDC time to start its bootloader */
	k_msleep(500);

	/* Initialize IRQ work */
	k_work_init(&data->irq_work, tps_irq_work_handler);

	/* Initialize re-enable work */
	k_work_init_delayable(&data->re_enable_work,
			      tps_re_enable_work_handler);

	/* Configure IRQ GPIO */
	if (!gpio_is_ready_dt(&cfg->irq_gpio)) {
		LOG_ERR("IRQ GPIO not ready");
		return -ENODEV;
	}

	ret = gpio_pin_configure_dt(&cfg->irq_gpio, GPIO_INPUT);
	if (ret < 0) {
		LOG_ERR("Failed to configure IRQ GPIO: %d", ret);
		return ret;
	}

	gpio_init_callback(&data->irq_cb, tps_irq_handler,
			   BIT(cfg->irq_gpio.pin));
	ret = gpio_add_callback(cfg->irq_gpio.port, &data->irq_cb);
	if (ret < 0) {
		LOG_ERR("Failed to add IRQ callback: %d", ret);
		return ret;
	}

	ret = gpio_pin_interrupt_configure_dt(&cfg->irq_gpio,
					      GPIO_INT_EDGE_TO_ACTIVE);
	if (ret < 0) {
		LOG_ERR("Failed to configure IRQ interrupt: %d", ret);
		return ret;
	}

	/* Check if IRQ is already asserted */
	if (gpio_pin_get_dt(&cfg->irq_gpio) > 0) {
		LOG_INF("IRQ line already active, scheduling work");
		k_work_submit(&data->irq_work);
	}

	/* Unmask "New Contract As Producer/Consumer", "Device Incompatible
	 * Error", and "Plug Insert or Removal" interrupts */
	union reg_interrupt irq_mask = { 0 };

	irq_mask.new_contract_as_producer = 1;
	irq_mask.new_contract_as_consumer = 1;
	irq_mask.device_incompatible_error = 1;
	irq_mask.plug_insert_or_removal = 1;

	k_mutex_lock(&data->lock, K_FOREVER);
	ret = tps_write_reg(&cfg->i2c, REG_INTERRUPT_MASK_FOR_I2C1,
			    irq_mask.raw_value, sizeof(union reg_interrupt));
	k_mutex_unlock(&data->lock);

	if (ret == 0) {
		LOG_INF("TPS6699X Interrupt Unmasked");
	} else {
		LOG_ERR("Failed to write IRQ mask: %d", ret);
	}

	return 0;
}
#endif

static int tps6699x_init(const struct device *dev)
{
	const struct tps6699x_config *cfg = dev->config;
	struct tps6699x_data *data = dev->data;

	data->dev = dev;
	/*
	 * -1 to set auto mode. It means enabled by default
	 * and disabled only if incompatible device connected.
	 * After incompatible device is disconnected mode is back to default.
	 */
	data->forced_debug_accessory_mode = DEBUG_ACCESSORY_AUTO;

	if (!i2c_is_ready_dt(&cfg->i2c)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

	k_mutex_init(&data->lock);

#ifdef CONFIG_TPS6699X_INTERRUPTS
	union reg_status status;
	int ret;

	ret = tps_init_interrupt(dev);
	if (ret < 0) {
		return ret;
	}

	/* Check for pre-existing contract */
	ret = tps_rd_status(dev, &status);
	if (ret) {
		LOG_ERR("Failed to read Status: %d", ret);
		return ret;
	}

	if (status.plug_present) {
		LOG_INF("Detected previous contract");
		tps_handle_new_contract(dev->data);
	}
#endif

#ifdef CONFIG_PDC_DEBUG
	tps_uart_init();
#endif

	LOG_INF("TPS6699X initialized");
	return 0;
}

#ifdef CONFIG_TPS6699X_INTERRUPTS
BUILD_ASSERT(CONFIG_GPIO_INIT_PRIORITY < CONFIG_APPLICATION_INIT_PRIORITY,
	"GPIO subsystem has to be initialized before this driver");
#endif

DEVICE_DT_INST_DEFINE(0, tps6699x_init, NULL, &tps6699x_data, &tps6699x_config,
		      POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, NULL);
