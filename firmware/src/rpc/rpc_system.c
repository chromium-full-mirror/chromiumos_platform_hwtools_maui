/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

/* RPC "system" domain (field numbers 100 - 199). */

#include "app_version.h"
#include "rpc.h"

#include <string.h>

#include <zephyr/sys/util.h>

static maui_rpc_Status handle_ping(const maui_rpc_Request *req,
				   maui_rpc_Response *resp)
{
	maui_rpc_PingResponse *out = &resp->msg.ping;

	resp->which_msg = maui_rpc_Response_ping_tag;
	out->token = req->msg.ping.token;
	out->protocol_version =
		maui_rpc_ProtocolVersion_PROTOCOL_VERSION_CURRENT;
	strncpy(out->fw_version, APP_VERSION_STRING,
		sizeof(out->fw_version) - 1);

	/* Capability discovery: every request tag with a registered handler. */
	STRUCT_SECTION_FOREACH(rpc_handler, h)
	{
		if (out->supported_requests_count >=
		    ARRAY_SIZE(out->supported_requests)) {
			break;
		}
		out->supported_requests[out->supported_requests_count++] =
			h->tag;
	}

	return maui_rpc_Status_STATUS_OK;
}

RPC_HANDLER_DEFINE(ping, maui_rpc_Request_ping_tag, handle_ping);
