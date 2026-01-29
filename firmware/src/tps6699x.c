/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */
#include "tps6699x.h"
#include "tps6699x_reg.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
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

/** Split streaming transfers down into chunks of this size for more manageable
 *  I2C write lengths.
 *  Reduced to 24 to accommodate driver overhead (limit 32 bytes).
 */
#define TPS_STREAM_CHUNK_SIZE (24)

LOG_MODULE_REGISTER(tps6699x, CONFIG_LOG_DEFAULT_LEVEL);

struct tps6699x_config {
	struct i2c_dt_spec i2c;
};

struct tps6699x_data {
	struct k_mutex lock;
};

static const struct tps6699x_config tps6699x_config = {
	.i2c = I2C_DT_SPEC_INST_GET(0),
};

static struct tps6699x_data tps6699x_data;

#if IS_ENABLED(CONFIG_PDC_DEBUG)

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

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_pdc_cmds,
	SHELL_CMD_ARG(sbud, NULL, "Send SBUd command <0|1>",
		      cmd_pdc_sbud_handler, 2, 0),
	SHELL_CMD_ARG(sbdf, NULL, "Send SBDF command <0|1>",
		      cmd_pdc_sbdf_handler, 2, 0),
	SHELL_CMD_ARG(info, NULL, "Read and print chip info",
		      cmd_pdc_info_handler, 1, 0),
	SHELL_SUBCMD_SET_END);

SHELL_CMD_REGISTER(pdc, &sub_pdc_cmds, "PDC commands", NULL);

static int tps6699x_init(const struct device *dev)
{
	const struct tps6699x_config *cfg = dev->config;
	struct tps6699x_data *data = dev->data;

	if (!i2c_is_ready_dt(&cfg->i2c)) {
		LOG_ERR("I2C bus not ready");
		return -ENODEV;
	}

	k_mutex_init(&data->lock);

#if IS_ENABLED(CONFIG_PDC_DEBUG)
	tps_uart_init();
#endif

	LOG_INF("TPS6699X initialized");
	return 0;
}

DEVICE_DT_INST_DEFINE(0, tps6699x_init, NULL, &tps6699x_data, &tps6699x_config,
		      POST_KERNEL, CONFIG_APPLICATION_INIT_PRIORITY, NULL);
