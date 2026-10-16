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
TAG=""
DEVICES="maui_proto,maui_v11"

# Parse arguments
while [[ $# -gt 0 ]]; do
    case "$1" in
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
            echo "Usage: $0 [--device <maui_proto,maui_v11|maui_v12|maui_v20>] [--tag <name>]"
            exit 1
            ;;
    esac
done

echo "Starting MCU firmware release for version: $VERSION (devices: $DEVICES)"

IFS=',' read -ra DEV_ARRAY <<< "$DEVICES"
FIRST_DEV="${DEV_ARRAY[0]}"

if [[ "$FIRST_DEV" == "maui_proto" || "$FIRST_DEV" == "maui_v11" ]]; then
    MCU_SUBDIR="mspm0"
    VERSION_PATH="${MCU_SUBDIR}/${VERSION}"

    echo "Building MSPM0 firmware using Docker..."
    docker build -t maui-builder -f dockerfiles/Dockerfile.fw_builder .
    docker run --rm -v "$(pwd):/repo" maui-builder:latest

    ARTIFACT="firmware/build_docker/zephyr/zephyr.txt"
    if [ ! -f "$ARTIFACT" ]; then
        echo "Error: Build failed, artifact not found at $ARTIFACT"
        exit 1
    fi

    echo "Uploading MSPM0 firmware to GCS..."
    gsutil cp "$ARTIFACT" "$GCS_BUCKET/$COMP_DIR/$VERSION_PATH/zephyr.txt"

    if [ -n "$TAG" ]; then
        for dev in "${DEV_ARRAY[@]}"; do
            manage_gcs_tag "$COMP_DIR" "$VERSION_PATH" "${TAG}.${dev}" "$TAG_SUFFIX"
        done
    fi

    echo "Release successful!"
    echo "Firmware: $GCS_BUCKET/$COMP_DIR/$VERSION_PATH/zephyr.txt"

elif [[ "$FIRST_DEV" == "maui_v12" || "$FIRST_DEV" == "maui_v20" ]]; then
    MCU_SUBDIR="$FIRST_DEV"
    VERSION_PATH="${MCU_SUBDIR}/${VERSION}"

    echo "Building ITE IT82002 firmware for $FIRST_DEV using Docker..."
    docker build -t maui-builder-it82002 --build-arg BOARD=it82002 -f dockerfiles/Dockerfile.fw_builder .
    docker run --rm -v "$(pwd):/repo" -w /repo/firmware \
        --entrypoint bash maui-builder-it82002 \
        -c './bootloader/mcuboot/build_bootloaders.sh'

    APP_ARTIFACT="firmware/maui_app_signed.bin"
    RW_ARTIFACT="firmware/maui_rw_bootloader_signed.bin"
    FULL_ARTIFACT="firmware/maui_full_flash_1mb.bin"

    for f in "$APP_ARTIFACT" "$RW_ARTIFACT" "$FULL_ARTIFACT"; do
        if [ ! -f "$f" ]; then
            echo "Error: Build failed, artifact not found at $f"
            exit 1
        fi
    done

    echo "Uploading ITE firmware artifacts to GCS..."
    gsutil cp "$APP_ARTIFACT" "$GCS_BUCKET/$COMP_DIR/$VERSION_PATH/maui_app_signed.bin"
    gsutil cp "$RW_ARTIFACT" "$GCS_BUCKET/$COMP_DIR/$VERSION_PATH/maui_rw_bootloader_signed.bin"
    gsutil cp "$FULL_ARTIFACT" "$GCS_BUCKET/$COMP_DIR/$VERSION_PATH/maui_full_flash_1mb.bin"

    if [ -n "$TAG" ]; then
        for dev in "${DEV_ARRAY[@]}"; do
            manage_gcs_tag "$COMP_DIR" "$VERSION_PATH" "${TAG}.${dev}" "$TAG_SUFFIX"
        done
    fi

    echo "Release successful!"
    echo "Firmware dir: $GCS_BUCKET/$COMP_DIR/$VERSION_PATH/"
else
    echo "Error: Unsupported device type '$FIRST_DEV'."
    exit 1
fi
