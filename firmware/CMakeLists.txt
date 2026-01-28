# Copyright 2025 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

cmake_minimum_required(VERSION 3.20.0)

list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(BOARD maui_proto)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(maui)

target_sources(app PRIVATE
	drivers/adc_mspm0_adc12_wip.c
	src/main.c src/commands.c src/utils.c src/led.c src/adc.c src/pdc.c)
