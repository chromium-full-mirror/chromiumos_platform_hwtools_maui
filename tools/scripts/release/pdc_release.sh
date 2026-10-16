#!/bin/bash
# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# PDC Firmware Release Script

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=/dev/null
source "${SCRIPT_DIR}/common.sh"

cd "${REPO_ROOT}"

COMP_DIR="pdc-fw"
TAG_SUFFIX=".pdcfw"

FILE=""
VERSION=""
TAG=""
DEVICES=""

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --file)
            FILE="$2"
            shift 2
            ;;
        --version)
            VERSION="$2"
            shift 2
            ;;
        --device)
            DEVICES="$2"
            shift 2
            ;;
        --tag)
            TAG="$2"
            shift 2
            ;;
        *)
            echo "Unknown argument: $1"
            echo "Usage: $0 --file <path> --version <ver_string> [--device <maui_proto|maui_v11|maui_v12|maui_v20,...>] [--tag <channel>]"
            exit 1
            ;;
    esac
done

if [ -z "$FILE" ] || [ -z "$VERSION" ]; then
    echo "Error: --file and --version are required."
    exit 1
fi

echo "Starting PDC firmware release for version: $VERSION"

# 1. Upload to GCS
FILENAME=$(basename "$FILE")
echo "Uploading firmware to GCS..."
gsutil cp "$FILE" "$GCS_BUCKET/$COMP_DIR/$VERSION/$FILENAME"

# 2. Handle Tag Migration (per-device if --device specified, otherwise legacy)
if [ -n "$TAG" ]; then
    if [ -n "$DEVICES" ]; then
        IFS=',' read -ra DEV_ARRAY <<< "$DEVICES"
        for dev in "${DEV_ARRAY[@]}"; do
            manage_gcs_tag "$COMP_DIR" "$VERSION" "${TAG}.${dev}" "$TAG_SUFFIX"
        done
    else
        manage_gcs_tag "$COMP_DIR" "$VERSION" "$TAG" "$TAG_SUFFIX"
    fi
fi

echo "Release successful!"
echo "Firmware: $GCS_BUCKET/$COMP_DIR/$VERSION/$FILENAME"
