#!/bin/bash
# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# This script automates the release of Maui host tools.
# It builds the Docker image, tags it, and uploads it to GCS.

set -e

# Change to the root directory of the repo
cd "$(dirname "$0")/.."

# GCS Bucket
GCS_BUCKET="gs://maui-firmware"
DEFAULT_IMAGE="maui-utils"

# Default tag to the dynamic version
./tools/version.sh
# We need to get the version from the generated file
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
echo "Target GCS Tag: $TAG"

# 1. Build the Docker image
echo "Building Docker image..."
docker build -t "$DEFAULT_IMAGE:latest" -f dockerfiles/Dockerfile.utils .
docker tag "$DEFAULT_IMAGE:latest" "$DEFAULT_IMAGE:$VERSION"

# 2. Package the image
echo "Saving Docker image to tarball..."
# Always save the versioned tag, as that's what image manager expects after loading.
docker save -o maui-utils.tar "$DEFAULT_IMAGE:$VERSION"

# 3. Upload to GCS
echo "Uploading image to GCS..."
# Always use VERSION for the directory as per project conventions.
gsutil cp maui-utils.tar "$GCS_BUCKET/docker/$VERSION/maui-utils.tar"

# 4. Handle Channel Tag Migration (stable, alpha, prev, etc.)
# Any tag that is not the version itself acts as a channel/pointer.
if [ "$TAG" != "$VERSION" ]; then
    echo "Managing $TAG channel tag migration..."
    MARKER="${TAG}.dockertag"
    # Search for existing marker across the bucket to ensure it only points to one version.
    EXISTING_MARKERS=$(gsutil ls "$GCS_BUCKET/docker/*/$MARKER" 2>/dev/null || true)

    if [ -n "$EXISTING_MARKERS" ]; then
        echo "Found existing $TAG tags, removing them:"
        echo "$EXISTING_MARKERS"
        for m in $EXISTING_MARKERS; do
            gsutil rm "$m"
        done
    fi

    echo "Creating new $TAG tag for version $VERSION..."
    touch "$MARKER"
    gsutil cp "$MARKER" "$GCS_BUCKET/docker/$VERSION/$MARKER"
    rm "$MARKER"
fi

echo "Release successful!"
echo "Image: $GCS_BUCKET/docker/$VERSION/maui-utils.tar"
if [ "$TAG" != "$VERSION" ]; then
    echo "Channel '$TAG' now points to version $VERSION"
fi
