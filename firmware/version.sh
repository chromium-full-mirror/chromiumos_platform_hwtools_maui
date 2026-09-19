#!/bin/bash
# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# This script generates app_version.h based on git information and VERSION file.

set -e

cd "$(dirname "$0")" # Change to firmware directory

# Get version from VERSION file
if [ -f "VERSION" ]; then
    VERSION_MAJOR=$(grep "VERSION_MAJOR =" VERSION | cut -d' ' -f3)
else
    VERSION_MAJOR=0
fi

if [ -z "$VERSION_MAJOR" ]; then
    VERSION_MAJOR=0
fi

# Ensure git works inside Docker container even when repo is owned by host user
git config --global --add safe.directory '*' 2>/dev/null || true

# Get git info
if git rev-parse --git-dir > /dev/null 2>&1; then
    GIT_COMMIT_COUNT=$(git rev-list --count HEAD 2>/dev/null || echo "0")
    GIT_DESCRIBE=$(git describe --always --dirty 2>/dev/null || echo "unknown")
    GIT_BRANCH=$(git rev-parse --abbrev-ref HEAD 2>/dev/null || echo "unknown")
else
    GIT_COMMIT_COUNT=0
    GIT_DESCRIBE="unknown"
    GIT_BRANCH="unknown"
fi

BUILD_TIMESTAMP=$(date -u +"%Y-%m-%d %H:%M:%S UTC")
APP_VERSION="${VERSION_MAJOR}.${GIT_COMMIT_COUNT}-${GIT_DESCRIBE} (${GIT_BRANCH}, built ${BUILD_TIMESTAMP})"

cat <<EOF > src/app_version.h
#ifndef APP_VERSION_H_
#define APP_VERSION_H_

#define APP_VERSION_STRING "${APP_VERSION}"

#endif /* APP_VERSION_H_ */
EOF
