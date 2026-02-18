#!/bin/bash
# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# Shared configuration and functions for Maui release scripts.

# GCS Bucket
GCS_BUCKET="gs://maui-firmware"

# Root directory of the repository
REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
export REPO_ROOT

# Function to manage GCS channel tags
# Arguments:
#   1: COMP_DIR (e.g., "mcu-fw", "pdc-fw", "docker")
#   2: VERSION (the actual version string)
#   3: TAG (the channel name, e.g., "stable")
#   4: TAG_SUFFIX (e.g., ".mcufw", ".pdcfw", ".dockertag")
manage_gcs_tag() {
    local comp_dir="$1"
    local version="$2"
    local tag="$3"
    local suffix="$4"
    local marker="${tag}${suffix}"

    if [ "$tag" == "$version" ] || [ -z "$tag" ]; then
        return
    fi

    echo "Managing $tag channel tag migration for $comp_dir..."

    # Search for existing markers across the bucket and remove them
    gsutil ls "$GCS_BUCKET/$comp_dir/*/$marker" 2>/dev/null | while read -r m; do
        echo "Removing old $tag tag: $m"
        gsutil rm "$m"
    done

    echo "Creating new $tag tag for version $version..."
    echo -n "" | gsutil cp - "$GCS_BUCKET/$comp_dir/$version/$marker"
}
