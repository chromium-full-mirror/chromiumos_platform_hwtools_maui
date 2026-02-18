#!/bin/bash
# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# MCU Firmware Release Script

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=/dev/null
source "${SCRIPT_DIR}/common.sh"

cd "${REPO_ROOT}"

COMP_DIR="mcu-fw"
TAG_SUFFIX=".mcufw"

# 1. Generate version
./firmware/version.sh
VERSION=$(grep "APP_VERSION_STRING" firmware/src/app_version.h | cut -d'"' -f2)
TAG="$VERSION"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
        --tag)
            TAG="$2"
            shift 2
            ;;
        *)
            echo "Unknown argument: $1"
            echo "Usage: $0 [--tag <name>]"
            exit 1
            ;;
    esac
done

echo "Starting MCU firmware release for version: $VERSION"

# 2. Build the firmware
echo "Building firmware using Docker..."
docker build -t maui-builder -f dockerfiles/Dockerfile.fw_builder .
docker run --rm -v "$(pwd):/repo" maui-builder:latest

ARTIFACT="firmware/build_docker/zephyr/zephyr.txt"
if [ ! -f "$ARTIFACT" ]; then
    echo "Error: Build failed, artifact not found at $ARTIFACT"
    exit 1
fi

# 3. Upload to GCS
echo "Uploading firmware to GCS..."
gsutil cp "$ARTIFACT" "$GCS_BUCKET/$COMP_DIR/$VERSION/zephyr.txt"

# 4. Handle Tag Migration
manage_gcs_tag "$COMP_DIR" "$VERSION" "$TAG" "$TAG_SUFFIX"

echo "Release successful!"
echo "Firmware: $GCS_BUCKET/$COMP_DIR/$VERSION/zephyr.txt"
