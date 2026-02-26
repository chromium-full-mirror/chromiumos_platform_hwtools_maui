/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * TI TPS6699X PDC FW update code
 */

#include "tps6699x.h"
#include "tps6699x_reg.h"

#include <stdlib.h>
#include <string.h>

#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/shell/shell.h>
#include <zephyr/sys/base64.h>

LOG_MODULE_REGISTER(tps6699x_fwup, CONFIG_LOG_DEFAULT_LEVEL);

#define TPS_4CC_MAX_DURATION K_MSEC(1200)
#define TPS_4CC_POLL_DELAY K_USEC(200)
#define TPS_RESET_DELAY K_MSEC(2000)
#define TPS_TFUS_BOOTLOADER_ENTRY_DELAY K_MSEC(500)

/* The string length of a base64 encoded message including padding. */
#define BASE64_LENGTH(n) ((4 * ((n) / 3) + 3) & ~3)
#define LEN_TFUi sizeof(tfu_initiate)
#define LEN_TFUd sizeof(tfu_download)
#define LEN_STREAM (66) /* Broadcast addr + 64-byte data */
#define BASE64_LEN_TFUi BASE64_LENGTH(LEN_TFUi)
#define BASE64_LEN_TFUd BASE64_LENGTH(LEN_TFUd)
#define BASE64_LEN_STREAM BASE64_LENGTH(LEN_STREAM)

/* LCOV_EXCL_START - non-shipping code */
static struct {
	const struct device *pdc_dev;
	size_t bytes_streamed;
} ctx;

struct tfu_packet_generic {
	uint16_t num_blocks;
	uint16_t data_block_size;
	uint16_t timeout_secs;
	uint16_t broadcast_address;
} __attribute__((__packed__));

typedef struct tfu_packet_generic tfu_initiate;
typedef struct tfu_packet_generic tfu_download;
/*
 * Complete uses custom values for switch/copy instead of true false.
 * Write these values to the register instead of true/false.
 */
#define DO_SWITCH 0xAC
#define DO_COPY 0xAC
struct tfu_complete {
	uint8_t do_switch;
	uint8_t do_copy;
} __attribute__((__packed__));

struct tfu_query {
	uint8_t bank;
	uint8_t cmd;
} __attribute__((__packed__));

struct tfu_query_output {
	uint8_t result;
	uint8_t tfu_state;
	uint8_t complete_image;
	uint16_t blocks_written;
	uint8_t header_block_status;
	uint8_t per_block_status[12];
	uint8_t num_header_bytes_written;
	uint8_t num_data_bytes_written;
	uint8_t num_appconfig_bytes_written;
} __attribute__((__packed__));

static int do_reset_pdc(const struct device *dev)
{
	return tps_cmd_gaid(dev, true);
}

static int tfus_run(const struct device *dev)
{
	int ret;

	union reg_command cmd = {
		.command = COMMAND_TASK_TFUS,
	};

	/* Make three attempts to run the TFUs command to start FW update. */
	for (int attempts = 0; attempts < 3; attempts++) {
		ret = tps_rw_command_for_i2c1(dev, &cmd, I2C_MSG_WRITE);
		if (ret == 0) {
			break;
		}

		k_sleep(K_MSEC(100));
	}

	if (ret) {
		LOG_ERR("Cannot write TFUs command (%d)", ret);
		return ret;
	}

	/* Wait 500ms for entry to bootloader mode, per datasheet */
	k_sleep(TPS_TFUS_BOOTLOADER_ENTRY_DELAY);

	/* Allow up to an additional 200ms */
	int64_t timeout = k_uptime_get() + 200;

	while (1) {
		/* Check mode register for "F211" value */
		union reg_mode mode;

		ret = tps_rd_mode(dev, &mode);

		if (ret == 0) {
			/* Got a mode result */
			if (memcmp("F211", mode.data, sizeof(mode.data)) == 0) {
				return 0;
			}

			/* Wrong mode, continue re-trying */
			LOG_ERR("TFUs failed! Mode is '%c%c%c%c'", mode.data[0],
				mode.data[1], mode.data[2], mode.data[3]);
		} else {
			/* I2C error, continue re-trying */
			LOG_ERR("Cannot read mode reg (%d)", ret);
		}

		if (k_uptime_get() > timeout) {
			return -ETIMEDOUT;
		}

		k_sleep(K_MSEC(50));
	}
}

static int pdc_tps6699x_fwup_start(const struct device *dev)
{
	int rv;

	if (ctx.pdc_dev) {
		LOG_ERR("FWUP session already in progress");
		return -EBUSY;
	}

	/* Enter bootloader mode */
	rv = tfus_run(dev);
	if (rv) {
		LOG_ERR("Cannot enter bootloader mode (%d)", rv);
		return rv;
	}

	/* Ready for FW transfer */
	ctx.pdc_dev = dev;
	ctx.bytes_streamed = 0;

	return 0;
}

static int pdc_tps6699x_fwup_send_initiate(uint8_t *buffer, size_t buffer_len)
{
	union reg_data cmd_data;
	union reg_data rbuf;
	int rv;

	if (ctx.pdc_dev == NULL) {
		LOG_ERR("No FWUP session in progress");
		return -ENODEV;
	}

	if (buffer == NULL || buffer_len != LEN_TFUi) {
		LOG_ERR("Given data does not match TFUi format");
		return -EINVAL;
	}

	memcpy(cmd_data.data, buffer, buffer_len);
	rv = run_task_sync(ctx.pdc_dev, COMMAND_TASK_TFUI, &cmd_data,
			   buffer_len, 1, rbuf.raw_value);
	if (rv < 0 || rbuf.data[0] != 0) {
		LOG_ERR("Failed to run TFUi. rv=%d, rbuf[0]=%u", rv,
			rbuf.data[0]);
		return rv;
	}

	/* Reset bytes written so we can track data we're streaming next */
	ctx.bytes_streamed = 0;

	return 0;
}

static int pdc_tps6699x_fwup_send_block(uint8_t *buffer, size_t buffer_len)
{
	union reg_data cmd_data;
	union reg_data rbuf;
	int rv;

	if (ctx.pdc_dev == NULL) {
		LOG_ERR("No FWUP session in progress");
		return -ENODEV;
	}

	if (buffer == NULL || buffer_len != LEN_TFUd) {
		LOG_ERR("Given data does not match TFUd format");
		return -EINVAL;
	}

	memcpy(cmd_data.data, buffer, buffer_len);
	rv = run_task_sync(ctx.pdc_dev, COMMAND_TASK_TFUD, &cmd_data,
			   buffer_len, 1, rbuf.raw_value);
	if (rv < 0 || rbuf.data[0] != 0) {
		LOG_ERR("Failed to run TFUd. rv=%d, rbuf[0]=%u", rv,
			rbuf.data[0]);
		return rv;
	}

	/* Reset bytes written so we can track data we're streaming next */
	ctx.bytes_streamed = 0;

	return 0;
}

static int pdc_tps6699x_fwup_stream(uint8_t *buffer, size_t buffer_len)
{
	int rv;

	if (ctx.pdc_dev == NULL) {
		LOG_ERR("No FWUP session in progress");
		return -ENODEV;
	}

	if (buffer == NULL) {
		LOG_ERR("Given data does not match streaming format");
		return -EINVAL;
	}

	uint16_t broadcast_address = *(uint16_t *)buffer;
	uint8_t *data = &buffer[2];
	size_t data_len = buffer_len - 2;

	rv = tps_stream_data(ctx.pdc_dev, broadcast_address, data, data_len);
	ctx.bytes_streamed += data_len;

	return ctx.bytes_streamed;
}

static int pdc_tps6699x_tfuq(void)
{
	union reg_data cmd_data;
	union reg_data output;
	struct tfu_query *tfuq = (struct tfu_query *)cmd_data.data;
	int rv;

	tfuq->bank = 0;
	tfuq->cmd = 0;

	rv = run_task_sync(ctx.pdc_dev, COMMAND_TASK_TFUQ, &cmd_data,
			   sizeof(struct tfu_query),
			   sizeof(struct tfu_query_output), output.data);
	if (rv < 0) {
		LOG_ERR("TFUq - Firmware update query failed (%d)", rv);
		return rv;
	}

	LOG_HEXDUMP_INF(output.data, sizeof(struct tfu_query_output),
			"TFUq raw data");

	return 0;
}

static int pdc_tps6699x_fwup_abort(void)
{
	int rv;
	union reg_data data;

	if (ctx.pdc_dev) {
		LOG_INF("TFU in progress - run TFUe to reset to normal firmware.");

		rv = run_task_sync(ctx.pdc_dev, COMMAND_TASK_TFUE, NULL, 0, 1,
				   data.raw_value);
		LOG_INF("TFUe rv=%d, result data byte=0x%02x", rv,
			data.data[0]);

		rv = do_reset_pdc(ctx.pdc_dev);
		if (rv) {
			LOG_ERR("PDC reset failed: %d", rv);
			LOG_ERR("Power cycle your board (battery cutoff and "
				"all external power)");

			/* Continue even if failed */
		}
	}

	/* Reset session state */
	LOG_INF("Ending PDC FWUP session");
	memset(&ctx, 0, sizeof(ctx));

	return 0;
}

static int pdc_tps6699x_fwup_complete(void)
{
	union reg_data cmd_data;
	union reg_data rbuf;
	int rv;

	if (ctx.pdc_dev == NULL) {
		/* Need to start a FWUP session first */
		LOG_ERR("No FWUP session in progress");
		return -ENODEV;
	}

	/* Always dump TFUq before attempting completion. Failure here should
	 * result in an abort.
	 */
	if (pdc_tps6699x_tfuq() != 0) {
		return pdc_tps6699x_fwup_abort();
	}

	/* Finish update with a TFU copy. */
	struct tfu_complete tfuc;
	tfuc.do_switch = 0;
	tfuc.do_copy = DO_COPY;

	LOG_INF("Running TFUc [Switch: 0x%02x, Copy: 0x%02x]", tfuc.do_switch,
		tfuc.do_copy);
	memcpy(cmd_data.data, &tfuc, sizeof(tfuc));
	rv = run_task_sync(ctx.pdc_dev, COMMAND_TASK_TFUC, &cmd_data,
			   sizeof(tfuc), 4, rbuf.data);

	if (rv < 0 || rbuf.data[0] != 0) {
		LOG_ERR("Failed 4cc task with result %d, rbuf.data[0] = %d", rv,
			rbuf.data[0]);
		return rv;
	}

	uint8_t s1 = rbuf.data[1];
	uint8_t s2 = rbuf.data[2];
	uint8_t s3 = rbuf.data[3];

	LOG_INF("TFUq bytes [Success: 0x%02x, State: 0x%02x, Complete: 0x%02x]",
		s1, s2, s3);

	/* Wait TPS_RESET_DELAY for reset to complete. */
	k_sleep(TPS_RESET_DELAY);

	LOG_INF("PDC FWUP successful");

	/* Reset session state */
	memset(&ctx, 0, sizeof(ctx));
	return 0;
}
static int cmd_pdc_tps_fwup_start(const struct shell *sh, size_t argc,
				  char **argv)
{
	int rv;
	const struct device *dev;

	dev = DEVICE_DT_GET_ONE(ti_tps6699x);

	if (!device_is_ready(dev)) {
		shell_error(sh, "TPS6699x device not ready");
		return -ENODEV;
	}

	shell_print(sh, "Using TPS6699x device: %s", dev->name);

	rv = pdc_tps6699x_fwup_start(dev);
	if (rv) {
		shell_error(sh, "TPS_FWUP: Cannot start: %d", rv);
		return rv;
	}

	shell_info(sh, "TPS_FWUP: Started");
	return 0;
}

static int cmd_pdc_tps_fwup_send_initiate(const struct shell *sh, size_t argc,
					  char **argv)
{
	uint8_t decode_buffer[BASE64_LEN_TFUi];
	size_t decoded_byte_count = 0;
	int rv;

	rv = base64_decode(decode_buffer, sizeof(decode_buffer),
			   &decoded_byte_count, argv[1], strlen(argv[1]));

	if (rv) {
		shell_error(sh, "TPS_FWUP: Base64 format error: %d", rv);
		return rv;
	}

	rv = pdc_tps6699x_fwup_send_initiate(decode_buffer, decoded_byte_count);
	if (rv < 0) {
		shell_error(sh, "TPS_FWUP: Initiate (TFUi) error: %d", rv);
		return rv;
	}

	shell_info(sh, "TPS_FWUP: Send Initiate complete");
	return 0;
}

static int cmd_pdc_tps_fwup_send_block(const struct shell *sh, size_t argc,
				       char **argv)
{
	uint8_t decode_buffer[BASE64_LEN_TFUd];
	size_t decoded_byte_count = 0;
	int rv;

	rv = base64_decode(decode_buffer, sizeof(decode_buffer),
			   &decoded_byte_count, argv[1], strlen(argv[1]));

	if (rv) {
		shell_error(sh, "TPS_FWUP: Base64 format error: %d", rv);
		return rv;
	}

	rv = pdc_tps6699x_fwup_send_block(decode_buffer, decoded_byte_count);
	if (rv < 0) {
		shell_error(sh, "TPS_FWUP: Data block (TFUd) error: %d", rv);
		return rv;
	}

	shell_info(sh, "TPS_FWUP: Send Block complete");
	return 0;
}

static int cmd_pdc_tps_fwup_stream(const struct shell *sh, size_t argc,
				   char **argv)
{
	uint8_t decode_buffer[BASE64_LEN_STREAM];
	size_t decoded_byte_count = 0;
	int rv;

	rv = base64_decode(decode_buffer, sizeof(decode_buffer),
			   &decoded_byte_count, argv[1], strlen(argv[1]));

	if (rv) {
		shell_error(sh, "TPS_FWUP: Base64 format error: %d", rv);
		return rv;
	}

	rv = pdc_tps6699x_fwup_stream(decode_buffer, decoded_byte_count);
	if (rv < 0) {
		shell_error(sh, "TPS_FWUP: Streaming error: %d", rv);
		return rv;
	}

	shell_print(sh, "TPS_FWUP: Stream - bytes written: %d", rv);

	return 0;
}

static int cmd_pdc_tps_fwup_complete(const struct shell *sh, size_t argc,
				     char **argv)
{
	int rv;

	rv = pdc_tps6699x_fwup_complete();
	if (rv) {
		shell_error(sh, "TPS_FWUP: Cannot finish update: %d", rv);
		return rv;
	}

	shell_info(sh, "TPS_FWUP: Success");
	return 0;
}

static int cmd_pdc_tps_fwup_abort(const struct shell *sh, size_t argc,
				  char **argv)
{
	return pdc_tps6699x_fwup_abort();
}

SHELL_STATIC_SUBCMD_SET_CREATE(
	sub_pdc_tps_fwup_cmds,
	SHELL_CMD_ARG(start, NULL,
		      "Prepare the PDC for firmware download\n"
		      "Usage: pdc_tps_fwup start",
		      cmd_pdc_tps_fwup_start, 1, 0),
	SHELL_CMD_ARG(send_initiate, NULL,
		      "Send TFUi command with data to initiate update\n"
		      "Usage: pdc_tps_fwup send_initiate <base64>",
		      cmd_pdc_tps_fwup_send_initiate, 2, 0),
	SHELL_CMD_ARG(send_block, NULL,
		      "Send TFUd command with data to transfer block data\n"
		      "Usage: pdc_tps_fwup send_block <base64>",
		      cmd_pdc_tps_fwup_send_block, 2, 0),
	SHELL_CMD_ARG(stream, NULL,
		      "Stream data for TFUi or TFUd after sending the command\n"
		      "Usage: pdc_tps_fwup stream <base64>",
		      cmd_pdc_tps_fwup_stream, 2, 0),
	SHELL_CMD_ARG(complete, NULL,
		      "Finalize the FW update and restart PD subsystem\n"
		      "Usage: pdc_tps_fwup complete",
		      cmd_pdc_tps_fwup_complete, 1, 0),
	SHELL_CMD_ARG(abort, NULL,
		      "Recover from a failed or interrupted update session\n"
		      "Usage: pdc_tps_fwup abort",
		      cmd_pdc_tps_fwup_abort, 1, 0),
	SHELL_SUBCMD_SET_END);
SHELL_CMD_REGISTER(pdc_tps_fwup, &sub_pdc_tps_fwup_cmds,
		   "TI PDC firmware update commands", NULL);
