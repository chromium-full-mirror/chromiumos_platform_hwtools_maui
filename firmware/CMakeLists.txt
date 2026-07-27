# Copyright 2025 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

cmake_minimum_required(VERSION 3.20.0)

list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
set(BOARD maui_proto_3507)

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(maui)

# Safely override missing IOMUX definition for 48-pin package in the standard GPIO driver (only for maui_proto_5117)
if(BOARD STREQUAL "maui_proto_5117")
	zephyr_compile_definitions(IOMUX_PINCM60=IOMUX_PINCM59)
endif()


if(CONFIG_ADC_MSPM0_ADC12_WIP)
	target_sources(app PRIVATE drivers/adc_mspm0_adc12_wip.c)
endif()

if(CONFIG_ADC)
	target_sources(app PRIVATE src/adc.c)
endif()


target_sources(app PRIVATE
	src/bypass.c
	src/commands.c
	src/gpio.c
	src/gpio_shell.c
	src/host_mux.c
	src/hwrev.c
	src/led.c
	src/main.c
	src/sbu_mux.c
	src/tps6699x.c
	src/tps6699x_fwup.c
	src/usb_init.c
	src/utils.c
)
