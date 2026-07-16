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

# Workaround for upstream bug in TI's Zephyr fork (device/mspm0g5187-support branch):
# The TI I2C driver (i2c_mspm0.c) was updated to use UniComm I2C FIFO macro names
# (DL_I2CC_*), which only exist in the driverlib headers for newer UniComm MCUs
# (e.g. MSPM0G5117). Standard MSPM0 MCUs (e.g. MSPM0G3507) use standard I2C and
# only define DL_I2C_* macros. We map the UniComm macro names to standard I2C macro
# names here for maui_proto_3507 so both targets build cleanly without patching Zephyr.
if(BOARD STREQUAL "maui_proto_3507")
	zephyr_compile_definitions(
		DL_I2CC_TX_FIFO_LEVEL_1_4_EMPTY=DL_I2C_TX_FIFO_LEVEL_BYTES_1
		DL_I2CC_RX_FIFO_LEVEL_1_4_FULL=DL_I2C_RX_FIFO_LEVEL_BYTES_1
	)
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
