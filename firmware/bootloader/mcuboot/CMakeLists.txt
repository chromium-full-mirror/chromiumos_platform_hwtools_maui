# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

if(CONFIG_BOOT_GO_HOOKS)
  zephyr_library()
  zephyr_library_sources(it82002_mcuboot_hook.c)
  zephyr_library_link_libraries(MCUBOOT_BOOTUTIL)
endif()
