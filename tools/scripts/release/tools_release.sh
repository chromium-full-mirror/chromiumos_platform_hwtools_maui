#!/bin/bash
# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Host Tools Release Script

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
# shellcheck source=/dev/null
source "${SCRIPT_DIR}/common.sh"

cd "${REPO_ROOT}"

COMP_DIR="docker"
TAG_SUFFIX=".dockertag"
DEFAULT_IMAGE="maui-utils"

# 1. Generate version
./tools/version.sh
VERSION=$(grep "__version__" tools/maui_libs/version.py | cut -d'"' -f2)
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

echo "Starting release for version: $VERSION"

# 2. Build the Docker image
echo "Building Docker image..."
docker build -t "$DEFAULT_IMAGE:latest" -f dockerfiles/Dockerfile.utils .
docker tag "$DEFAULT_IMAGE:latest" "$DEFAULT_IMAGE:$VERSION"

# 3. Package the image
echo "Saving Docker image to tarball..."
docker save -o maui-utils.tar "$DEFAULT_IMAGE:$VERSION"

# 4. Upload to GCS
echo "Uploading image to GCS..."
gsutil cp maui-utils.tar "$GCS_BUCKET/$COMP_DIR/$VERSION/maui-utils.tar"
rm maui-utils.tar

# 5. Handle Tag Migration
manage_gcs_tag "$COMP_DIR" "$VERSION" "$TAG" "$TAG_SUFFIX"

echo "Release successful!"
echo "Image: $GCS_BUCKET/$COMP_DIR/$VERSION/maui-utils.tar"
