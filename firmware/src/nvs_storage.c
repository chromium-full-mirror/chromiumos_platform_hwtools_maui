/* Copyright 2026 The ChromiumOS Authors
 * Use of this source code is governed by a BSD-style license that can be
 * found in the LICENSE file.
 */

#include "nvs_storage.h"

#include <errno.h>
#include <string.h>

#include <zephyr/device.h>
#include <zephyr/drivers/flash.h>
#include <zephyr/kernel.h>
#include <zephyr/kvss/nvs.h>
#include <zephyr/logging/log.h>
#include <zephyr/storage/flash_map.h>

LOG_MODULE_REGISTER(nvs_storage);

#if DT_NODE_EXISTS(DT_NODELABEL(storage_partition)) && IS_ENABLED(CONFIG_NVS)
static struct nvs_fs nvs_fs_inst;
static bool nvs_mounted;
#endif

int nvs_storage_init(void)
{
#if DT_NODE_EXISTS(DT_NODELABEL(storage_partition)) && IS_ENABLED(CONFIG_NVS)
	struct flash_pages_info info;
	int rc;

	if (nvs_mounted) {
		return 0;
	}

	nvs_fs_inst.flash_device = PARTITION_DEVICE(storage_partition);
	if (!device_is_ready(nvs_fs_inst.flash_device)) {
		LOG_ERR("NVS flash device not ready");
		return -ENODEV;
	}

	nvs_fs_inst.offset = PARTITION_OFFSET(storage_partition);
	rc = flash_get_page_info_by_offs(nvs_fs_inst.flash_device,
					 nvs_fs_inst.offset, &info);
	if (rc != 0) {
		LOG_ERR("Failed to get flash page info (%d)", rc);
		return rc;
	}

	nvs_fs_inst.sector_size = info.size;
	nvs_fs_inst.sector_count =
		PARTITION_SIZE(storage_partition) / info.size;

	rc = nvs_mount(&nvs_fs_inst);
	if (rc != 0) {
		LOG_ERR("Failed to mount NVS partition (%d)", rc);
		return rc;
	}

	nvs_mounted = true;
	LOG_INF("NVS mounted at 0x%lx (%u sectors of %u B)",
		(unsigned long)nvs_fs_inst.offset, nvs_fs_inst.sector_count,
		nvs_fs_inst.sector_size);
	return 0;
#else
	return 0;
#endif
}

int nvs_storage_get_serial(char *buf, size_t len)
{
	if (!buf || len == 0) {
		return -EINVAL;
	}

#if DT_NODE_EXISTS(DT_NODELABEL(storage_partition)) && IS_ENABLED(CONFIG_NVS)
	if (!nvs_mounted && nvs_storage_init() != 0) {
		strncpy(buf, NVS_DEFAULT_SERIAL, len - 1);
		buf[len - 1] = '\0';
		return 0;
	}

	ssize_t rc =
		nvs_read(&nvs_fs_inst, NVS_KEY_SERIAL_NUMBER, buf, len - 1);
	if (rc > 0) {
		buf[rc] = '\0';
		return 0;
	}
#endif

	strncpy(buf, NVS_DEFAULT_SERIAL, len - 1);
	buf[len - 1] = '\0';
	return 0;
}

int nvs_storage_set_serial(const char *serial)
{
	if (!serial) {
		return -EINVAL;
	}

	size_t slen = strlen(serial);
	if (slen == 0 || slen >= NVS_SERIAL_MAX_LEN) {
		return -EINVAL;
	}

#if DT_NODE_EXISTS(DT_NODELABEL(storage_partition)) && IS_ENABLED(CONFIG_NVS)
	if (!nvs_mounted) {
		int rc = nvs_storage_init();
		if (rc != 0) {
			return rc;
		}
	}

	ssize_t written = nvs_write(&nvs_fs_inst, NVS_KEY_SERIAL_NUMBER, serial,
				    slen + 1);
	if (written < 0) {
		LOG_ERR("Failed to write serial to NVS (%zd)", written);
		return (int)written;
	}

	LOG_INF("Stored serial number in NVS: %s", serial);
	return 0;
#else
	return -ENOTSUP;
#endif
}

int nvs_storage_set_boot_target(uint32_t target)
{
#if DT_NODE_EXISTS(DT_NODELABEL(storage_partition)) && IS_ENABLED(CONFIG_NVS)
	if (!nvs_mounted && nvs_storage_init() != 0) {
		return -ENODEV;
	}

	ssize_t written = nvs_write(&nvs_fs_inst, NVS_KEY_BOOT_TARGET, &target,
				    sizeof(target));
	return (written < 0) ? (int)written : 0;
#else
	return -ENOTSUP;
#endif
}

int nvs_storage_get_and_clear_boot_target(uint32_t *target)
{
	if (!target) {
		return -EINVAL;
	}
	*target = 0;

#if DT_NODE_EXISTS(DT_NODELABEL(storage_partition)) && IS_ENABLED(CONFIG_NVS)
	if (!nvs_mounted && nvs_storage_init() != 0) {
		return -ENODEV;
	}

	ssize_t rc = nvs_read(&nvs_fs_inst, NVS_KEY_BOOT_TARGET, target,
			      sizeof(*target));
	if (rc == sizeof(*target)) {
		(void)nvs_delete(&nvs_fs_inst, NVS_KEY_BOOT_TARGET);
		return 0;
	}
#endif
	return -ENOENT;
}
