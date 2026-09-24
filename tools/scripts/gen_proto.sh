#!/bin/bash
# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Regenerate tools/maui_libs/proto/maui_rpc_pb2.py from
# firmware/proto/maui_rpc.proto.
#
# The firmware generates its Nanopb code at build time; the host module is
# checked in so maui-utils needs no protoc. grpcio-tools is pinned to the
# release matching the protobuf runtime in dockerfiles/requirements.utils.compiled
# (gencode must not be newer than the runtime). Runs in a throwaway container.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/../.." && pwd)"
IMAGE="${IMAGE:-python:3.13-slim}"
GRPCIO_TOOLS_VERSION="1.84.0"
PROTOBUF_VERSION="7.36.2"

docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp \
	-v "${REPO_DIR}:/repo" "${IMAGE}" bash -c "
		set -e
		python3 -m venv /tmp/venv
		/tmp/venv/bin/pip install -q \
			grpcio-tools==${GRPCIO_TOOLS_VERSION} \
			protobuf==${PROTOBUF_VERSION}
		/tmp/venv/bin/python -m grpc_tools.protoc \
			-I/repo/firmware/proto \
			--python_out=/repo/tools/maui_libs/proto \
			/repo/firmware/proto/maui_rpc.proto
	"

echo "Generated tools/maui_libs/proto/maui_rpc_pb2.py"
