#!/bin/bash
# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
FIRMWARE_DIR="${REPO_DIR}/firmware"

echo "=== Building Maui ITE IT82002 Multi-Stage MCUboot System ==="
echo "    Layout: RO (96KB @ 0x0) | RW (128KB @ 0x18000) | App (768KB @ 0x38000) | NVS (32KB @ 0xF8000)"

cd "${FIRMWARE_DIR}"
# Provided by the maui-builder-it82002 container image, so ShellCheck cannot
# follow it from the repo.
# shellcheck source=/dev/null
source /zephyr/zephyr/zephyr-env.sh
chmod +x ./version.sh
./version.sh

# 1. Build RO Bootloader (Offset 0x00000000, 96KB)
echo "--> Building RO Recovery Bootloader (96 KB @ 0x80000000)..."
west build -b maui_v1p2_it82002 -d build_mcuboot_ro -p always /zephyr/bootloader/mcuboot/boot/zephyr -- \
	-DBOARD_ROOT="${FIRMWARE_DIR}" \
	-DDTS_ROOT="${FIRMWARE_DIR}" \
	-DZEPHYR_EXTRA_MODULES="${SCRIPT_DIR}" \
	-DEXTRA_CFLAGS="-DMAUI_RO_BOOTLOADER=1" \
	-DEXTRA_CONF_FILE="${SCRIPT_DIR}/ro_bootloader.conf" \
	-DDTC_OVERLAY_FILE="${SCRIPT_DIR}/ro_bootloader.overlay"
cp -f build_mcuboot_ro/zephyr/zephyr.bin build_mcuboot_ro/zephyr/ro_bootloader.bin
echo "    RO Bootloader built: build_mcuboot_ro/zephyr/ro_bootloader.bin ($(stat -c%s build_mcuboot_ro/zephyr/ro_bootloader.bin) bytes)"

# 2. Build RW Bootloader (Offset 0x00018000, 128KB)
echo "--> Building RW Updatable Bootloader (128 KB @ 0x80018000)..."
west build -b maui_v1p2_it82002 -d build_mcuboot_rw -p always /zephyr/bootloader/mcuboot/boot/zephyr -- \
	-DBOARD_ROOT="${FIRMWARE_DIR}" \
	-DDTS_ROOT="${FIRMWARE_DIR}" \
	-DZEPHYR_EXTRA_MODULES="${SCRIPT_DIR}" \
	-DEXTRA_CFLAGS="-DMAUI_RW_BOOTLOADER=1" \
	-DEXTRA_CONF_FILE="${SCRIPT_DIR}/rw_bootloader.conf" \
	-DDTC_OVERLAY_FILE="${SCRIPT_DIR}/rw_bootloader.overlay"
cp -f build_mcuboot_rw/zephyr/zephyr.bin build_mcuboot_rw/zephyr/rw_bootloader.bin
# NOTE: Do NOT pass --pad-header because CONFIG_ROM_START_OFFSET=0x200 already pads 0x200 bytes in zephyr.bin
imgtool sign --header-size 0x200 --align 4 --version 1.0.0 --slot-size 0x20000 \
	build_mcuboot_rw/zephyr/zephyr.bin build_mcuboot_rw/zephyr/rw_bootloader_signed.bin
cp -f build_mcuboot_rw/zephyr/rw_bootloader_signed.bin "${FIRMWARE_DIR}/maui_rw_bootloader_signed.bin"
echo "    RW Bootloader signed: ${FIRMWARE_DIR}/maui_rw_bootloader_signed.bin ($(stat -c%s "${FIRMWARE_DIR}/maui_rw_bootloader_signed.bin") bytes)"

# 3. Build Main Application (Offset 0x00038000, 768KB)
echo "--> Building Main Application (768 KB @ 0x80038000)..."
west build -b maui_v1p2_it82002 -d build_docker -p always "${FIRMWARE_DIR}" -- \
	-DCONFIG_BOOTLOADER_MCUBOOT=y \
	-DCONFIG_USE_DT_CODE_PARTITION=y \
	-DDTC_OVERLAY_FILE="${SCRIPT_DIR}/app_mcuboot.overlay"
cp -f build_docker/zephyr/zephyr.bin build_docker/zephyr/ec.bin
# NOTE: Do NOT pass --pad-header because CONFIG_BOOTLOADER_MCUBOOT=y already pads 0x200 bytes in zephyr.bin
imgtool sign --header-size 0x200 --align 4 --version 1.0.0 --slot-size 0xc0000 \
	build_docker/zephyr/zephyr.bin build_docker/zephyr/ec_signed.bin
cp -f build_docker/zephyr/ec_signed.bin "${FIRMWARE_DIR}/maui_app_signed.bin"
echo "    Main Application signed: ${FIRMWARE_DIR}/maui_app_signed.bin ($(stat -c%s "${FIRMWARE_DIR}/maui_app_signed.bin") bytes)"

# 3b. Also build Standalone Main Application at 0x00000000 (No Bootloader) for bringup isolation
echo "--> Building Standalone Main Application (at 0x80000000, no bootloader)..."
west build -b maui_v1p2_it82002 -d build_standalone -p always "${FIRMWARE_DIR}"
cp -f build_standalone/zephyr/zephyr.bin build_docker/zephyr/maui_standalone_0x0.bin
cp -f build_standalone/zephyr/zephyr.bin "${FIRMWARE_DIR}/maui_standalone_0x0.bin"
echo "    Standalone App built: ${FIRMWARE_DIR}/maui_standalone_0x0.bin ($(stat -c%s "${FIRMWARE_DIR}/maui_standalone_0x0.bin") bytes)"

# 4. Generate Combined 1MB Full-Flash Binary
echo "--> Assembling 1MB Full Flash Image (maui_full_flash_1mb.bin)..."
python3 - <<EOF
import os

FLASH_SIZE = 1024 * 1024  # 1 MB
flash_buf = bytearray([0xFF] * FLASH_SIZE)

FREE_WARN_PCT = 15.0  # Warn when a partition has less headroom than this.

usage = []

def place_binary(path, offset, max_size, name):
    if not os.path.exists(path):
        raise FileNotFoundError(f"Error: {path} not found")
    with open(path, "rb") as f:
        data = f.read()
    if len(data) > max_size:
        raise ValueError(f"{name} ({len(data)} B) exceeds partition limit ({max_size} B)!")
    flash_buf[offset:offset+len(data)] = data
    usage.append((name, offset, len(data), max_size, False))

def reserve(name, offset, max_size):
    """Record a partition this script does not populate (runtime data area)."""
    usage.append((name, offset, None, max_size, True))

place_binary("build_mcuboot_ro/zephyr/ro_bootloader.bin",        0x00000,  96 * 1024, "RO Bootloader")
place_binary("build_mcuboot_rw/zephyr/rw_bootloader_signed.bin", 0x18000, 128 * 1024, "RW Bootloader")
place_binary("build_docker/zephyr/ec_signed.bin",                0x38000, 768 * 1024, "Main App (slot 0)")
reserve("NVS Storage",                                           0xF8000,  32 * 1024)

print("")
print("    Partition          Offset       Used      Size    Used%      Free")
print("    -----------------------------------------------------------------")
low = []
for name, offset, used, size, is_reserved in usage:
    if is_reserved:
        print(f"    {name:18s} 0x{offset:06X} {'-':>9s} {size:9d}  {'reserved':>15s}")
        continue
    free = size - used
    free_pct = 100.0 * free / size
    flag = ""
    if free_pct < FREE_WARN_PCT:
        flag = "  <-- LOW"
        low.append((name, free, size))
    print(f"    {name:18s} 0x{offset:06X} {used:9d} {size:9d}  {100.0 - free_pct:6.1f}% {free:9d}{flag}")
print("")

for name, free, size in low:
    print(f"    WARNING: {name} has only {free} B free ({100.0 * free / size:.1f}% of "
          f"{size} B). Shrink the image or grow the partition (the partition table "
          f"lives in the board DTS and in the bootloader overlays).")

out_path = "build_docker/zephyr/maui_full_flash_1mb.bin"
with open(out_path, "wb") as f:
    f.write(flash_buf)
with open("maui_full_flash_1mb.bin", "wb") as f:
    f.write(flash_buf)
print(f"=== Complete: {out_path} and firmware/maui_full_flash_1mb.bin ({len(flash_buf)} bytes) ===")
EOF

# Fix file ownership when run inside Docker container as root
if [ "$(id -u)" = "0" ] && [ -d "${REPO_DIR}" ]; then
    OWNER="$(stat -c '%u:%g' "${REPO_DIR}")"
    chown -R "${OWNER}" build_mcuboot_ro build_mcuboot_rw build_docker build_standalone src/app_version.h maui_standalone_0x0.bin maui_full_flash_1mb.bin 2>/dev/null || true
fi

echo "=== Build Complete ==="
