/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/*
 * Machine-to-machine RPC over the second USB CDC-ACM interface ("Maui RPC").
 *
 * The core (rpc.c) owns the transport: it frames/deframes COBS, runs Nanopb,
 * and dispatches each Request to the handler registered for its oneof tag.
 * It knows nothing about individual features. Each functional domain lives in
 * its own src/rpc/rpc_<domain>.c and registers its calls with
 * RPC_HANDLER_DEFINE(), so adding a feature never touches the core.
 *
 * See firmware/proto/maui_rpc.proto for the wire protocol and the field-number
 * layout per domain.
 */

#ifndef MAUI_RPC_H_
#define MAUI_RPC_H_

#include "proto/maui_rpc.pb.h"

#include <stdint.h>

#include <zephyr/sys/iterable_sections.h>

/**
 * Request handler.
 *
 * Called on the RPC thread, one request at a time, so handlers of the same
 * domain are serialized with each other. Long operations are fine (the host
 * waits for the response) but should publish progress Events if they take
 * more than about a second.
 *
 * @param req  Decoded request; req->which_msg is the handler's tag.
 * @param resp Response to fill. It arrives zeroed with status STATUS_OK. Set
 *             resp->which_msg and the payload when the call returns data.
 * @return     Status placed in the response. On failure, return
 *             rpc_error(resp, -errno, ...) to report the status, the errno
 *             and a short reason together.
 */
typedef maui_rpc_Status (*rpc_handler_fn)(const maui_rpc_Request *req,
					  maui_rpc_Response *resp);

struct rpc_handler {
	/* maui_rpc_Request_<name>_tag */
	pb_size_t tag;
	const char *name;
	rpc_handler_fn fn;
};

/**
 * Register @p _fn as the handler for Request oneof tag @p _tag.
 *
 * Example:
 *   RPC_HANDLER_DEFINE(ping, maui_rpc_Request_ping_tag, handle_ping);
 */
#define RPC_HANDLER_DEFINE(_name, _tag, _fn)                           \
	static const STRUCT_SECTION_ITERABLE(rpc_handler,              \
					     _rpc_handler_##_name) = { \
		.tag = (_tag),                                         \
		.name = #_name,                                        \
		.fn = (_fn),                                           \
	}

/**
 * Start the RPC transport. Call after USB is enabled. Returns -ENODEV when the
 * board has no "Maui RPC" CDC-ACM interface.
 */
int rpc_init(void);

/**
 * Publish an unsolicited Event to the host.
 *
 * Safe to call from any thread (not from ISRs) and never blocks, so producers
 * can publish unconditionally. The core fills in timestamp_ms and event_seq.
 * Returns -ENOTCONN while no host has the RPC port open, and -ENOBUFS (event
 * dropped, visible to the host as an event_seq gap) when the TX path is busy.
 */
int rpc_send_event(const maui_rpc_Event *evt);

/** Number of events dropped with -ENOBUFS since boot. */
uint32_t rpc_events_dropped(void);

/** Store a printf-style reason in resp->error (truncated to fit). */
void rpc_set_error(maui_rpc_Response *resp, const char *fmt, ...);

/**
 * Fail a request: store the reason in resp->error, the negative errno in
 * resp->error_code, and return the matching Status.
 */
maui_rpc_Status rpc_error(maui_rpc_Response *resp, int err, const char *fmt,
			  ...);

/** Map a negative errno to the closest protocol Status. */
maui_rpc_Status rpc_status_from_errno(int err);

#endif /* MAUI_RPC_H_ */
