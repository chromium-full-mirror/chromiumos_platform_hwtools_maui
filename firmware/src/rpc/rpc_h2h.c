/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * RPC "h2h" domain (field numbers 200 - 299): read and program the Prolific
 * PL27A1 Host-to-Host bridge's SPI NOR flash (go/maui-ite-design,
 * "Host-to-Host (H2H) ADB Bridge SPI Flashing").
 *
 * Write session:
 *   H2hBegin  - hold PL27A1 in reset, mux the flash to the EC, check the JEDEC
 *               ID and size, clear block protection, erase the whole chip.
 *   H2hWrite  - strictly sequential chunks, programmed as they arrive. The host
 *               pipelines several writes, so USB transfer of the next chunk
 *               overlaps with programming the current one.
 *   H2hFinish - check the CRC-32 of the received stream, read the flash back
 *               and check it again, release the mux and the PL27A1 reset.
 *
 * Read session:
 *   H2hReadBegin - hold PL27A1 in reset, mux the flash to the EC, check the
 *                  JEDEC ID and size.
 *   H2hRead      - return one chunk from any offset within the flash.
 *   H2hReadEnd   - release the mux and the PL27A1 reset.
 *
 *   H2hAbort  - end either kind of session immediately.
 *
 * The bus is always handed back to the PL27A1: on any error, on abort, when
 * the host closes the port during a long erase/verify, and after
 * CONFIG_MAUI_RPC_H2H_IDLE_TIMEOUT_MS without traffic (host crashed or was
 * unplugged mid-session). While a session is active the 'h2h_flash' shell
 * commands are refused, so they cannot flip the mux under a transfer.
 */

#include "../h2h_flash.h"
#include "rpc.h"

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/crc.h>

LOG_MODULE_REGISTER(rpc_h2h, CONFIG_MAUI_RPC_LOG_LEVEL);

/* Minimum interval between progress events. */
#define PROGRESS_INTERVAL_MS 100

enum session_mode {
	SESSION_WRITE,
	SESSION_READ,
};

static struct {
	bool active;
	enum session_mode mode;
	uint32_t image_size;
	uint32_t image_crc32;
	uint32_t next_offset;
	uint32_t rx_crc32;
	uint32_t flash_size;
	uint32_t bytes_read;
	int64_t start_ms;
	int64_t last_progress_ms;
} session;

static K_MUTEX_DEFINE(session_lock);
static void idle_timeout_fn(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(idle_work, idle_timeout_fn);

/* Caller holds session_lock. */
static void session_end(void)
{
	if (session.active) {
		(void)h2h_bus_release(H2H_OWNER_RPC);
	}
	session.active = false;
	(void)k_work_cancel_delayable(&idle_work);
}

static void session_touch(void)
{
	(void)k_work_reschedule(&idle_work,
				K_MSEC(CONFIG_MAUI_RPC_H2H_IDLE_TIMEOUT_MS));
}

static void idle_timeout_fn(struct k_work *work)
{
	ARG_UNUSED(work);

	k_mutex_lock(&session_lock, K_FOREVER);
	if (session.active) {
		LOG_WRN("H2H session idle for %d ms, releasing bus",
			CONFIG_MAUI_RPC_H2H_IDLE_TIMEOUT_MS);
		session_end();
	}
	k_mutex_unlock(&session_lock);
}

/*
 * Returns -ECANCELED once the host has closed the port, which stops a long
 * erase or verify instead of running it to completion for nobody.
 */
static int publish_progress(maui_rpc_H2hPhase phase, uint32_t done,
			    uint32_t total, bool force)
{
	int64_t now = k_uptime_get();
	maui_rpc_Event evt = maui_rpc_Event_init_zero;

	if (!force && now - session.last_progress_ms < PROGRESS_INTERVAL_MS) {
		return 0;
	}
	session.last_progress_ms = now;

	evt.which_msg = maui_rpc_Event_h2h_progress_tag;
	evt.msg.h2h_progress.phase = phase;
	evt.msg.h2h_progress.done = done;
	evt.msg.h2h_progress.total = total;
	return rpc_send_event(&evt) == -ENOTCONN ? -ECANCELED : 0;
}

static int erase_progress(uint32_t done, uint32_t total, void *user_data)
{
	ARG_UNUSED(user_data);
	return publish_progress(maui_rpc_H2hPhase_H2H_PHASE_ERASE, done, total,
				done == total);
}

static int verify_progress(uint32_t done, uint32_t total, void *user_data)
{
	ARG_UNUSED(user_data);
	return publish_progress(maui_rpc_H2hPhase_H2H_PHASE_VERIFY, done, total,
				done == total);
}

/* Acquire the bus and identify the flash. Caller holds session_lock. */
static maui_rpc_Status probe_flash(maui_rpc_Response *resp, uint32_t *jedec,
				   uint32_t *size)
{
	uint8_t id[3];
	int rc;

	rc = h2h_bus_claim(H2H_OWNER_RPC);
	if (rc == -EBUSY) {
		return rpc_error(resp, rc,
				 "bus held by shell ('h2h_flash release')");
	}
	if (rc == 0) {
		rc = h2h_flash_read_id(id);
	}
	if (rc == 0) {
		rc = h2h_flash_size_from_id(id, size);
	}
	if (rc != 0) {
		return rpc_error(resp, rc, "SPI flash probe failed (%d)", rc);
	}

	*jedec = ((uint32_t)id[0] << 16) | ((uint32_t)id[1] << 8) | id[2];
	return maui_rpc_Status_STATUS_OK;
}

static maui_rpc_Status handle_info(const maui_rpc_Request *req,
				   maui_rpc_Response *resp)
{
	maui_rpc_H2hInfoResponse *out = &resp->msg.h2h_info;
	maui_rpc_Status st;

	ARG_UNUSED(req);

	k_mutex_lock(&session_lock, K_FOREVER);
	if (session.active) {
		k_mutex_unlock(&session_lock);
		rpc_set_error(resp, "H2H session in progress");
		return maui_rpc_Status_STATUS_BUSY;
	}

	st = probe_flash(resp, &out->jedec_id, &out->flash_size);
	(void)h2h_bus_release(H2H_OWNER_RPC);
	k_mutex_unlock(&session_lock);

	if (st == maui_rpc_Status_STATUS_OK) {
		resp->which_msg = maui_rpc_Response_h2h_info_tag;
	}
	return st;
}

static maui_rpc_Status handle_begin(const maui_rpc_Request *req,
				    maui_rpc_Response *resp)
{
	const maui_rpc_H2hBeginRequest *in = &req->msg.h2h_begin;
	maui_rpc_H2hBeginResponse *out = &resp->msg.h2h_begin;
	maui_rpc_Status st;
	int rc;

	k_mutex_lock(&session_lock, K_FOREVER);

	if (session.active) {
		/* A previous host session died mid-way; start over. */
		LOG_WRN("Restarting H2H session");
		session_end();
	}

	if (in->image_size == 0) {
		rpc_set_error(resp, "empty image");
		st = maui_rpc_Status_STATUS_INVALID_ARGUMENT;
		goto out;
	}

	session.start_ms = k_uptime_get();
	st = probe_flash(resp, &out->jedec_id, &out->flash_size);
	if (st != maui_rpc_Status_STATUS_OK) {
		(void)h2h_bus_release(H2H_OWNER_RPC);
		goto out;
	}
	session.active = true;
	session.mode = SESSION_WRITE;

	if (in->image_size > out->flash_size) {
		rpc_set_error(resp, "image %u B > flash %u B", in->image_size,
			      out->flash_size);
		st = maui_rpc_Status_STATUS_TOO_LARGE;
		goto fail;
	}

	rc = h2h_flash_unprotect();
	if (rc == 0) {
		/* Full-chip erase: nothing from a previous image may survive
		 * past the end of the new one.
		 * TODO: measure on real hardware; on a large part erasing
		 * only ROUND_UP(image_size, 64 KB) may be worth the trade.
		 */
		session.last_progress_ms = 0;
		rc = h2h_flash_erase_cb(0, out->flash_size, erase_progress,
					NULL);
	}
	if (rc != 0) {
		st = rpc_error(resp, rc, "unprotect/erase failed (%d)", rc);
		goto fail;
	}

	session.image_size = in->image_size;
	session.image_crc32 = in->image_crc32;
	session.flash_size = out->flash_size;
	session.next_offset = 0;
	session.rx_crc32 = 0;
	out->max_chunk_size = sizeof(req->msg.h2h_write.data.bytes);
	resp->which_msg = maui_rpc_Response_h2h_begin_tag;
	session_touch();

	LOG_INF("H2H flash 0x%06x (%u KB) erased in %u ms, receiving %u B",
		out->jedec_id, out->flash_size / 1024,
		(uint32_t)(k_uptime_get() - session.start_ms), in->image_size);
	goto out;

fail:
	session_end();
out:
	k_mutex_unlock(&session_lock);
	return st;
}

static maui_rpc_Status handle_write(const maui_rpc_Request *req,
				    maui_rpc_Response *resp)
{
	const maui_rpc_H2hWriteRequest *in = &req->msg.h2h_write;
	maui_rpc_Status st = maui_rpc_Status_STATUS_OK;
	int rc;

	k_mutex_lock(&session_lock, K_FOREVER);

	if (!session.active || session.mode != SESSION_WRITE) {
		rpc_set_error(resp, "no active write session");
		st = maui_rpc_Status_STATUS_BAD_STATE;
		goto out;
	}
	if (in->offset != session.next_offset) {
		rpc_set_error(resp, "offset 0x%x, expected 0x%x", in->offset,
			      session.next_offset);
		st = maui_rpc_Status_STATUS_BAD_STATE;
		goto fail;
	}
	if (in->data.size == 0 ||
	    in->data.size > session.image_size - in->offset) {
		rpc_set_error(resp, "bad chunk size %u", in->data.size);
		st = maui_rpc_Status_STATUS_INVALID_ARGUMENT;
		goto fail;
	}

	rc = h2h_flash_write(in->offset, in->data.bytes, in->data.size);
	if (rc != 0) {
		st = rpc_error(resp, rc, "program @0x%x failed (%d)",
			       in->offset, rc);
		goto fail;
	}

	session.rx_crc32 = crc32_ieee_update(session.rx_crc32, in->data.bytes,
					     in->data.size);
	session.next_offset += in->data.size;
	resp->which_msg = maui_rpc_Response_h2h_write_tag;
	session_touch();
	goto out;

fail:
	session_end();
out:
	k_mutex_unlock(&session_lock);
	return st;
}

static maui_rpc_Status handle_finish(const maui_rpc_Request *req,
				     maui_rpc_Response *resp)
{
	maui_rpc_H2hFinishResponse *out = &resp->msg.h2h_finish;
	maui_rpc_Status st = maui_rpc_Status_STATUS_OK;
	uint32_t crc = 0;
	int rc;

	ARG_UNUSED(req);

	k_mutex_lock(&session_lock, K_FOREVER);

	if (!session.active || session.mode != SESSION_WRITE) {
		rpc_set_error(resp, "no active write session");
		st = maui_rpc_Status_STATUS_BAD_STATE;
		goto out;
	}
	if (session.next_offset != session.image_size) {
		rpc_set_error(resp, "incomplete: %u of %u B",
			      session.next_offset, session.image_size);
		st = maui_rpc_Status_STATUS_BAD_STATE;
		goto end;
	}
	if (session.rx_crc32 != session.image_crc32) {
		rpc_set_error(resp, "transfer CRC 0x%08x != 0x%08x",
			      session.rx_crc32, session.image_crc32);
		st = maui_rpc_Status_STATUS_VERIFY_FAILED;
		goto end;
	}

	session.last_progress_ms = 0;
	rc = h2h_flash_crc32_cb(0, session.image_size, &crc, verify_progress,
				NULL);
	if (rc != 0) {
		st = rpc_error(resp, rc, "readback failed (%d)", rc);
		goto end;
	}

	out->flash_crc32 = crc;
	out->duration_ms = (uint32_t)(k_uptime_get() - session.start_ms);
	resp->which_msg = maui_rpc_Response_h2h_finish_tag;

	if (crc != session.image_crc32) {
		rpc_set_error(resp, "flash CRC 0x%08x != 0x%08x", crc,
			      session.image_crc32);
		st = maui_rpc_Status_STATUS_VERIFY_FAILED;
		goto end;
	}

	LOG_INF("H2H flash programmed and verified: %u B, CRC 0x%08x, %u ms",
		session.image_size, crc, out->duration_ms);

end:
	/* Success or failure, give the flash back to the PL27A1. */
	session_end();
out:
	k_mutex_unlock(&session_lock);
	return st;
}

static maui_rpc_Status handle_abort(const maui_rpc_Request *req,
				    maui_rpc_Response *resp)
{
	ARG_UNUSED(req);

	k_mutex_lock(&session_lock, K_FOREVER);
	if (session.active && session.mode == SESSION_WRITE) {
		LOG_WRN("H2H write session aborted by host at %u of %u B",
			session.next_offset, session.image_size);
	} else if (session.active) {
		LOG_WRN("H2H read session aborted by host after %u B",
			session.bytes_read);
	}
	session_end();
	k_mutex_unlock(&session_lock);
	resp->which_msg = maui_rpc_Response_h2h_abort_tag;
	return maui_rpc_Status_STATUS_OK;
}

static maui_rpc_Status handle_read_begin(const maui_rpc_Request *req,
					 maui_rpc_Response *resp)
{
	maui_rpc_H2hReadBeginResponse *out = &resp->msg.h2h_read_begin;
	maui_rpc_Status st;

	ARG_UNUSED(req);

	k_mutex_lock(&session_lock, K_FOREVER);

	if (session.active) {
		/* A previous host session died mid-way; start over. */
		LOG_WRN("Restarting H2H session");
		session_end();
	}

	session.start_ms = k_uptime_get();
	st = probe_flash(resp, &out->jedec_id, &out->flash_size);
	if (st != maui_rpc_Status_STATUS_OK) {
		(void)h2h_bus_release(H2H_OWNER_RPC);
		goto out;
	}

	session.active = true;
	session.mode = SESSION_READ;
	session.flash_size = out->flash_size;
	session.bytes_read = 0;
	out->max_chunk_size = sizeof(resp->msg.h2h_read.data.bytes);
	resp->which_msg = maui_rpc_Response_h2h_read_begin_tag;
	session_touch();

	LOG_INF("H2H flash 0x%06x (%u KB) acquired for reading", out->jedec_id,
		out->flash_size / 1024);
out:
	k_mutex_unlock(&session_lock);
	return st;
}

static maui_rpc_Status handle_read(const maui_rpc_Request *req,
				   maui_rpc_Response *resp)
{
	const maui_rpc_H2hReadRequest *in = &req->msg.h2h_read;
	maui_rpc_H2hReadResponse *out = &resp->msg.h2h_read;
	maui_rpc_Status st = maui_rpc_Status_STATUS_OK;
	int rc;

	k_mutex_lock(&session_lock, K_FOREVER);

	if (!session.active || session.mode != SESSION_READ) {
		rpc_set_error(resp, "no active read session");
		st = maui_rpc_Status_STATUS_BAD_STATE;
		goto out;
	}
	if (in->length == 0 || in->length > sizeof(out->data.bytes) ||
	    in->offset > session.flash_size ||
	    in->length > session.flash_size - in->offset) {
		rpc_set_error(resp, "bad range 0x%x +%u", in->offset,
			      in->length);
		st = maui_rpc_Status_STATUS_INVALID_ARGUMENT;
		goto fail;
	}

	rc = h2h_flash_read(in->offset, out->data.bytes, in->length);
	if (rc != 0) {
		st = rpc_error(resp, rc, "read @0x%x failed (%d)", in->offset,
			       rc);
		goto fail;
	}

	out->offset = in->offset;
	out->data.size = in->length;
	session.bytes_read += in->length;
	resp->which_msg = maui_rpc_Response_h2h_read_tag;
	session_touch();
	goto out;

fail:
	session_end();
out:
	k_mutex_unlock(&session_lock);
	return st;
}

static maui_rpc_Status handle_read_end(const maui_rpc_Request *req,
				       maui_rpc_Response *resp)
{
	maui_rpc_Status st = maui_rpc_Status_STATUS_OK;

	ARG_UNUSED(req);

	k_mutex_lock(&session_lock, K_FOREVER);
	if (!session.active || session.mode != SESSION_READ) {
		rpc_set_error(resp, "no active read session");
		st = maui_rpc_Status_STATUS_BAD_STATE;
		goto out;
	}

	LOG_INF("H2H flash read session done: %u B in %u ms",
		session.bytes_read,
		(uint32_t)(k_uptime_get() - session.start_ms));
	session_end();
	resp->which_msg = maui_rpc_Response_h2h_read_end_tag;
out:
	k_mutex_unlock(&session_lock);
	return st;
}

RPC_HANDLER_DEFINE(h2h_info, maui_rpc_Request_h2h_info_tag, handle_info);
RPC_HANDLER_DEFINE(h2h_begin, maui_rpc_Request_h2h_begin_tag, handle_begin);
RPC_HANDLER_DEFINE(h2h_write, maui_rpc_Request_h2h_write_tag, handle_write);
RPC_HANDLER_DEFINE(h2h_finish, maui_rpc_Request_h2h_finish_tag, handle_finish);
RPC_HANDLER_DEFINE(h2h_abort, maui_rpc_Request_h2h_abort_tag, handle_abort);
RPC_HANDLER_DEFINE(h2h_read_begin, maui_rpc_Request_h2h_read_begin_tag,
		   handle_read_begin);
RPC_HANDLER_DEFINE(h2h_read, maui_rpc_Request_h2h_read_tag, handle_read);
RPC_HANDLER_DEFINE(h2h_read_end, maui_rpc_Request_h2h_read_end_tag,
		   handle_read_end);
