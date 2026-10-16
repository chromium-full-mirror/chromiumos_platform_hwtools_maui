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
#   2: VERSION_PATH (e.g., "v20_0325_100W" or "mspm0/0.60-042f858")
#   3: TAG (the channel/device tag name, e.g., "stable" or "stable.maui_v12")
#   4: TAG_SUFFIX (e.g., ".mcufw", ".pdcfw", ".dockertag")
manage_gcs_tag() {
    local comp_dir="$1"
    local version_path="$2"
    local tag="$3"
    local suffix="$4"
    local marker="${tag}${suffix}"

    if [ "$tag" == "$version_path" ] || [ -z "$tag" ]; then
        return
    fi

    echo "Managing $marker channel tag migration for $comp_dir..."

    # Search for existing markers across all subdirectories and remove them
    gsutil ls "$GCS_BUCKET/$comp_dir/**/$marker" 2>/dev/null | while read -r m; do
        echo "Removing old $marker tag: $m"
        gsutil rm "$m"
    done

    echo "Creating new $marker tag at $comp_dir/$version_path/..."
    echo -n "" | gsutil cp - "$GCS_BUCKET/$comp_dir/$version_path/$marker"
}
