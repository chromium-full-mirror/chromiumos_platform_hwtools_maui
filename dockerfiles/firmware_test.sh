#!/bin/bash
# Copyright 2025 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

# ! This file is used as entrypoint in the Dockerfile.firmware container !
# ! Do not execute manually or outside of the docker container           !

# shellcheck disable=SC1091

set -e

if [ ! -d /repo ] || [ ! -d /zephyr ]; then
	echo "Do not execute this file outside of docker container!"
	exit 1
fi

export HOME=/tmp
git config --global --add safe.directory /zephyr/zephyr
source /zephyr/zephyr/zephyr-env.sh
cd /repo/firmware

# Run west twister
# $@ passes any arguments from the docker run command to west twister
# Default to running all tests if no arguments are provided
if [ $# -eq 0 ]; then
    west twister -x=DTS_ROOT=/repo/firmware -T tests -v -O tests/twister_outputs/twister-out
else
    west twister -x=DTS_ROOT=/repo/firmware "$@" -O tests/twister_outputs/twister-out
fi
