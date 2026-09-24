/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * RPC transport core: CDC-ACM <-> COBS <-> Nanopb <-> handler dispatch.
 *
 * RX: the UART callback copies bytes straight into a large ring buffer, so the
 * host can keep streaming (e.g. the next flash chunk) while a handler is busy.
 * When the ring fills up, RX interrupts are paused and USB flow control
 * back-pressures the host; the RPC thread re-enables them once a few KB are
 * free again.
 *
 * TX: responses and events share one encoder behind a mutex. Every frame
 * starts with a delimiter as well as ending with one, so if a frame is ever
 * cut short (host stopped reading) the next frame still decodes cleanly. A
 * response may wait for the host to read; an event never blocks its producer
 * and is dropped instead (the host sees a gap in Event.event_seq).
 */

#include "rpc.h"

#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/data/cobs.h>
#include <zephyr/device.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/ring_buffer.h>

#include <pb_decode.h>
#include <pb_encode.h>

LOG_MODULE_REGISTER(maui_rpc, CONFIG_MAUI_RPC_LOG_LEVEL);

#define RPC_UART_NODE DT_NODELABEL(cdc_acm_uart1)
BUILD_ASSERT(DT_NODE_HAS_STATUS_OKAY(RPC_UART_NODE),
	     "CONFIG_MAUI_RPC needs a 'cdc_acm_uart1' CDC-ACM node");

static const struct device *const rpc_dev = DEVICE_DT_GET(RPC_UART_NODE);

#define RX_CLAIM_MAX 256
/* Resume a paused RX only once this much is free, to avoid IRQ churn. */
#define RX_RESUME_SPACE MIN(4096, CONFIG_MAUI_RPC_RX_BUF_SIZE / 2)

/*
 * The firmware only ever sends Responses and Events, never the (large)
 * Requests, so the TX buffer is sized for those plus the Envelope overhead
 * (seq varint + oneof tag + length).
 */
#define TX_PB_SIZE (MAX(maui_rpc_Response_size, maui_rpc_Event_size) + 16)
/* Worst-case COBS size of n bytes, with leading and trailing delimiters. */
#define COBS_FRAME_MAX(n) ((n) + (n) / 254 + 3)

BUILD_ASSERT(COBS_FRAME_MAX(maui_rpc_Event_size + 16) <=
		     CONFIG_MAUI_RPC_TX_BUF_SIZE,
	     "CONFIG_MAUI_RPC_TX_BUF_SIZE must hold one full event frame");

RING_BUF_DECLARE(rx_ring, CONFIG_MAUI_RPC_RX_BUF_SIZE);
RING_BUF_DECLARE(tx_ring, CONFIG_MAUI_RPC_TX_BUF_SIZE);

static K_SEM_DEFINE(rx_sem, 0, 1);
static K_SEM_DEFINE(tx_space_sem, 0, 1);
static K_MUTEX_DEFINE(tx_lock);
static atomic_t rx_paused;
static atomic_t event_seq;
static atomic_t events_dropped;

/* RPC-thread-only state. */
static struct cobs_decoder rx_decoder;
static uint8_t rx_frame[maui_rpc_Envelope_size];
static size_t rx_frame_len;
static bool rx_discard;
static maui_rpc_Envelope rx_env;
static maui_rpc_Envelope resp_env;

/* Protected by tx_lock. */
static struct cobs_encoder tx_encoder;
static uint8_t tx_pb[TX_PB_SIZE];
static maui_rpc_Envelope event_env;

/* ------------------------------------------------------------------------ */
/* UART                                                                     */
/* ------------------------------------------------------------------------ */

static void rpc_uart_cb(const struct device *dev, void *user_data)
{
	ARG_UNUSED(user_data);

	while (uart_irq_update(dev) && uart_irq_is_pending(dev)) {
		if (uart_irq_rx_ready(dev)) {
			uint8_t *dst;
			uint32_t space =
				ring_buf_put_claim(&rx_ring, &dst, UINT32_MAX);

			if (space == 0) {
				/* Let USB NAK the host until we catch up. */
				uart_irq_rx_disable(dev);
				atomic_set(&rx_paused, 1);
			} else {
				int n = uart_fifo_read(dev, dst, space);

				ring_buf_put_finish(&rx_ring, MAX(n, 0));
				if (n > 0) {
					k_sem_give(&rx_sem);
				}
			}
		}

		if (uart_irq_tx_ready(dev)) {
			uint8_t *src;
			uint32_t len =
				ring_buf_get_claim(&tx_ring, &src, UINT32_MAX);

			if (len == 0) {
				uart_irq_tx_disable(dev);
			} else {
				int n = uart_fifo_fill(dev, src, len);

				ring_buf_get_finish(&tx_ring, MAX(n, 0));
				if (n > 0) {
					k_sem_give(&tx_space_sem);
				}
			}
		}
	}
}

static bool host_connected(void)
{
	uint32_t dtr = 0;

	/* If the driver cannot tell, assume a host is there. */
	return uart_line_ctrl_get(rpc_dev, UART_LINE_CTRL_DTR, &dtr) != 0 ||
	       dtr != 0;
}

/* Queue raw bytes for transmission. Caller holds tx_lock. */
static int tx_write(const uint8_t *buf, size_t len)
{
	while (len > 0) {
		uint32_t n = ring_buf_put(&tx_ring, buf, len);

		uart_irq_tx_enable(rpc_dev);
		if (n > 0) {
			buf += n;
			len -= n;
			continue;
		}

		if (k_sem_take(&tx_space_sem,
			       K_MSEC(CONFIG_MAUI_RPC_TX_TIMEOUT_MS)) != 0) {
			/* Host stopped reading. Drop what is queued; the
			 * next frame's leading delimiter terminates whatever
			 * part of this one already reached the host.
			 */
			uart_irq_tx_disable(rpc_dev);
			ring_buf_reset(&tx_ring);
			return -ETIMEDOUT;
		}
	}
	return 0;
}

static int cobs_tx_cb(const uint8_t *buf, size_t len, void *user_data)
{
	ARG_UNUSED(user_data);
	return tx_write(buf, len);
}

/* Serialize an Envelope into tx_pb. Caller holds tx_lock. */
static int encode_locked(const maui_rpc_Envelope *env, size_t *len)
{
	pb_ostream_t os = pb_ostream_from_buffer(tx_pb, sizeof(tx_pb));

	if (!pb_encode(&os, maui_rpc_Envelope_fields, env)) {
		LOG_ERR("encode failed: %s", PB_GET_ERROR(&os));
		return -EINVAL;
	}
	*len = os.bytes_written;
	return 0;
}

/* COBS-frame and queue the first len bytes of tx_pb. Caller holds tx_lock. */
static int frame_locked(size_t len)
{
	static const uint8_t delim;
	int ret;

	ret = tx_write(&delim, 1);
	if (ret == 0) {
		ret = cobs_encoder_init(&tx_encoder, cobs_tx_cb, NULL,
					COBS_FLAG_TRAILING_DELIMITER);
	}
	if (ret == 0) {
		ret = cobs_encoder_write(&tx_encoder, tx_pb, len);
	}
	if (ret >= 0) {
		ret = cobs_encoder_close(&tx_encoder);
	}
	return ret < 0 ? ret : 0;
}

static int send_response(const maui_rpc_Envelope *env)
{
	size_t len;
	int ret;

	if (!host_connected()) {
		return -ENOTCONN;
	}

	k_mutex_lock(&tx_lock, K_FOREVER);
	ret = encode_locked(env, &len);
	if (ret == 0) {
		ret = frame_locked(len);
	}
	k_mutex_unlock(&tx_lock);
	return ret;
}

/* ------------------------------------------------------------------------ */
/* Public helpers                                                           */
/* ------------------------------------------------------------------------ */

int rpc_send_event(const maui_rpc_Event *evt)
{
	uint32_t seq = (uint32_t)atomic_inc(&event_seq) + 1;
	size_t len;
	int ret;

	if (!host_connected()) {
		return -ENOTCONN;
	}

	/* Never wait: a busy or full TX path drops the event. */
	if (k_mutex_lock(&tx_lock, K_NO_WAIT) != 0) {
		atomic_inc(&events_dropped);
		return -ENOBUFS;
	}

	event_env = (maui_rpc_Envelope)maui_rpc_Envelope_init_zero;
	event_env.which_kind = maui_rpc_Envelope_event_tag;
	event_env.kind.event = *evt;
	event_env.kind.event.timestamp_ms = (uint64_t)k_uptime_get();
	event_env.kind.event.event_seq = seq;

	ret = encode_locked(&event_env, &len);
	if (ret == 0 && ring_buf_space_get(&tx_ring) < COBS_FRAME_MAX(len)) {
		atomic_inc(&events_dropped);
		ret = -ENOBUFS;
	}
	if (ret == 0) {
		ret = frame_locked(len);
	}
	k_mutex_unlock(&tx_lock);
	return ret;
}

void rpc_set_error(maui_rpc_Response *resp, const char *fmt, ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(resp->error, sizeof(resp->error), fmt, ap);
	va_end(ap);
}

maui_rpc_Status rpc_error(maui_rpc_Response *resp, int err, const char *fmt,
			  ...)
{
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(resp->error, sizeof(resp->error), fmt, ap);
	va_end(ap);
	resp->error_code = err;
	return rpc_status_from_errno(err);
}

maui_rpc_Status rpc_status_from_errno(int err)
{
	switch (err) {
	case 0:
		return maui_rpc_Status_STATUS_OK;
	case -EINVAL:
		return maui_rpc_Status_STATUS_INVALID_ARGUMENT;
	case -EALREADY:
	case -EPERM:
		return maui_rpc_Status_STATUS_BAD_STATE;
	case -EIO:
	case -ENODEV:
		return maui_rpc_Status_STATUS_IO_ERROR;
	case -EBADMSG:
		return maui_rpc_Status_STATUS_VERIFY_FAILED;
	case -ENOTSUP:
		return maui_rpc_Status_STATUS_UNSUPPORTED;
	case -EFBIG:
	case -ENOSPC:
		return maui_rpc_Status_STATUS_TOO_LARGE;
	case -EBUSY:
		return maui_rpc_Status_STATUS_BUSY;
	case -ETIMEDOUT:
		return maui_rpc_Status_STATUS_TIMEOUT;
	case -ENOENT:
		return maui_rpc_Status_STATUS_NOT_FOUND;
	case -ECANCELED:
	case -ENOTCONN:
		return maui_rpc_Status_STATUS_ABORTED;
	default:
		return maui_rpc_Status_STATUS_ERROR;
	}
}

uint32_t rpc_events_dropped(void)
{
	return (uint32_t)atomic_get(&events_dropped);
}

/* ------------------------------------------------------------------------ */
/* Dispatch                                                                 */
/* ------------------------------------------------------------------------ */

static void dispatch(const maui_rpc_Request *req, maui_rpc_Response *resp)
{
	STRUCT_SECTION_FOREACH(rpc_handler, h)
	{
		if (h->tag != req->which_msg) {
			continue;
		}

		resp->status = h->fn(req, resp);
		if (resp->status != maui_rpc_Status_STATUS_OK &&
		    resp->error[0] == '\0') {
			rpc_set_error(resp, "%s failed", h->name);
		}
		LOG_DBG("%s -> %d", h->name, resp->status);
		return;
	}

	resp->status = maui_rpc_Status_STATUS_UNSUPPORTED;
	rpc_set_error(resp, "unknown request %u", req->which_msg);
}

static void handle_frame(void)
{
	pb_istream_t is;
	int ret;

	if (rx_frame_len == 0) {
		return; /* Empty frame: host resync. */
	}

	is = pb_istream_from_buffer(rx_frame, rx_frame_len);
	if (!pb_decode(&is, maui_rpc_Envelope_fields, &rx_env)) {
		LOG_WRN("decode failed: %s", PB_GET_ERROR(&is));
		return;
	}
	if (rx_env.which_kind != maui_rpc_Envelope_request_tag) {
		LOG_WRN("ignoring non-request envelope (%u)",
			rx_env.which_kind);
		return;
	}
	if (rx_env.seq == 0) {
		/* seq 0 is reserved for Events; the host could not match
		 * the response, so do not act on the request either.
		 */
		LOG_WRN("ignoring request with seq 0");
		return;
	}

	resp_env = (maui_rpc_Envelope)maui_rpc_Envelope_init_zero;
	resp_env.seq = rx_env.seq;
	resp_env.which_kind = maui_rpc_Envelope_response_tag;
	dispatch(&rx_env.kind.request, &resp_env.kind.response);
	ret = send_response(&resp_env);
	if (ret < 0) {
		LOG_WRN("response to seq %u not sent (%d)", rx_env.seq, ret);
	}
}

static int cobs_rx_cb(const uint8_t *buf, size_t len, void *user_data)
{
	ARG_UNUSED(user_data);

	if (buf == NULL) {
		/* Delimiter: one complete frame. */
		handle_frame();
		rx_frame_len = 0;
		return 0;
	}

	if (rx_frame_len + len > sizeof(rx_frame)) {
		return -ENOMEM;
	}
	memcpy(&rx_frame[rx_frame_len], buf, len);
	rx_frame_len += len;
	return 0;
}

/*
 * Feed received bytes to the COBS decoder one delimiter-terminated segment at
 * a time, so a malformed or oversized frame costs only that frame: we drop
 * everything up to the next 0x00 and carry on. (The Zephyr decoder resets its
 * own state on every error.)
 */
static void rx_feed(const uint8_t *data, size_t len)
{
	while (len > 0) {
		const uint8_t *delim = memchr(data, 0x00, len);
		size_t seg = delim ? (size_t)(delim - data) + 1 : len;

		if (rx_discard) {
			if (delim) {
				rx_discard = false;
				rx_frame_len = 0;
			}
		} else {
			int ret = cobs_decoder_write(&rx_decoder, data, seg);

			if (ret < 0) {
				LOG_WRN("dropping malformed frame (%d)", ret);
				rx_frame_len = 0;
				rx_discard = (delim == NULL);
			}
		}

		data += seg;
		len -= seg;
	}
}

static void rx_maybe_resume(void)
{
	if (atomic_get(&rx_paused) &&
	    ring_buf_space_get(&rx_ring) >= RX_RESUME_SPACE &&
	    atomic_cas(&rx_paused, 1, 0)) {
		uart_irq_rx_enable(rpc_dev);
	}
}

static void rpc_thread_fn(void *p1, void *p2, void *p3)
{
	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	(void)cobs_decoder_init(&rx_decoder, cobs_rx_cb, NULL,
				COBS_FLAG_TRAILING_DELIMITER);

	for (;;) {
		uint8_t *data;
		uint32_t len;

		k_sem_take(&rx_sem, K_FOREVER);

		/* Claim small pieces: the claimed bytes stay reserved until
		 * ring_buf_get_finish(), and a handler may run for a long time
		 * inside rx_feed(), during which the host should be able to
		 * keep filling the rest of the ring.
		 */
		while ((len = ring_buf_get_claim(&rx_ring, &data,
						 RX_CLAIM_MAX)) > 0) {
			rx_feed(data, len);
			ring_buf_get_finish(&rx_ring, len);
			rx_maybe_resume();
		}
		rx_maybe_resume();
	}
}

K_THREAD_DEFINE(maui_rpc_thread, CONFIG_MAUI_RPC_STACK_SIZE, rpc_thread_fn,
		NULL, NULL, NULL, CONFIG_MAUI_RPC_THREAD_PRIORITY, 0, 0);

/* Two handlers for one tag would silently shadow each other. */
static int check_unique_tags(void)
{
	STRUCT_SECTION_FOREACH(rpc_handler, a)
	{
		STRUCT_SECTION_FOREACH(rpc_handler, b)
		{
			if (a < b && a->tag == b->tag) {
				LOG_ERR("RPC handlers %s and %s share tag %u",
					a->name, b->name, a->tag);
				return -EEXIST;
			}
		}
	}
	return 0;
}

int rpc_init(void)
{
	int ret;

	ret = check_unique_tags();
	__ASSERT(ret == 0, "duplicate RPC handler tag");
	if (ret < 0) {
		return ret;
	}

	if (!device_is_ready(rpc_dev)) {
		LOG_ERR("RPC CDC-ACM interface not ready");
		return -ENODEV;
	}

	ret = uart_irq_callback_user_data_set(rpc_dev, rpc_uart_cb, NULL);
	if (ret < 0) {
		LOG_ERR("failed to set UART callback (%d)", ret);
		return ret;
	}
	uart_irq_rx_enable(rpc_dev);

	LOG_INF("RPC endpoint ready");
	return 0;
}
