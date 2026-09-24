# Copyright 2025 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

cmake_minimum_required(VERSION 3.20.0)

list(APPEND BOARD_ROOT ${CMAKE_CURRENT_SOURCE_DIR})
if(NOT DEFINED BOARD)
	set(BOARD maui_proto_3507)
endif()

# Application-only devicetree additions live in boards/<board>.overlay. Zephyr
# applies that file automatically only when DTC_OVERLAY_FILE is unset; the
# MCUboot build flow sets it (app_mcuboot.overlay), so add it explicitly then.
set(MAUI_APP_OVERLAY ${CMAKE_CURRENT_SOURCE_DIR}/boards/${BOARD}.overlay)
if(DEFINED DTC_OVERLAY_FILE AND EXISTS ${MAUI_APP_OVERLAY})
	list(APPEND EXTRA_DTC_OVERLAY_FILE ${MAUI_APP_OVERLAY})
endif()

find_package(Zephyr REQUIRED HINTS $ENV{ZEPHYR_BASE})
project(maui)

# Machine-to-machine RPC (second CDC-ACM interface). The core lives in
# src/rpc/rpc.c; each functional domain adds its own src/rpc/rpc_<domain>.c.
if(CONFIG_MAUI_RPC)
	list(APPEND CMAKE_MODULE_PATH ${ZEPHYR_BASE}/modules/nanopb)
	include(nanopb)
	zephyr_nanopb_sources(app proto/maui_rpc.proto)
	zephyr_linker_sources(ROM_SECTIONS src/rpc/rpc_handlers.ld)
	target_sources(app PRIVATE
		src/rpc/rpc.c
		src/rpc/rpc_system.c
	)
endif()

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

# IT82002-only subsystems: BBRAM-backed boot health and the NVS key-value
# store. Gated so the MSPM0 boards, which have neither, do not build them.
if(CONFIG_MAUI_HEALTH)
	target_sources(app PRIVATE src/health.c)
endif()

if(CONFIG_MAUI_NVS_STORAGE)
	target_sources(app PRIVATE src/nvs_storage.c)
endif()

if(CONFIG_MAUI_HOST_MUX)
	target_sources(app PRIVATE src/host_mux.c)
endif()

if(CONFIG_MAUI_H2H_FLASH)
	target_sources(app PRIVATE src/h2h_flash.c)
endif()

target_sources(app PRIVATE
	src/bypass.c
	src/commands.c
	src/gpio.c
	src/gpio_shell.c
	src/hwrev.c
	src/led.c
	src/main.c
	src/sbu_mux.c
	src/tps6699x.c
	src/tps6699x_fwup.c
	src/usb_init.c
	src/utils.c
)
