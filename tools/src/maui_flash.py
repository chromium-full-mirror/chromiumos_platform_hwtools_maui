# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""
Maui Firmware Flashing CLI Tool.

This script provides a command-line interface for updating firmware on Maui
devices, supporting various components like the MCU and PDC.
"""

# pylint: disable=duplicate-code

import argparse
import logging
import os
import shutil
import subprocess
import sys
import tempfile
import threading
import time
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET
import zlib

from maui_libs import cli
from maui_libs.device import MauiDevice
from maui_libs.h2h import H2H_IMAGE_SIZE
from maui_libs.h2h import H2hFlasher
from maui_libs.pdc import MauiPdcClientAdapter
from maui_libs.pdc import Tps6699xFirmwareUpdater
from maui_libs.rpc import find_rpc_port
from maui_libs.rpc import RpcClient
from maui_libs.transport import DEVICE_TYPE_PROTO
from maui_libs.transport import DEVICE_TYPE_V11
from maui_libs.transport import DEVICE_TYPE_V12
from maui_libs.transport import ITE_DEVICE_TYPES
from maui_libs.transport import list_maui_devices
from maui_libs.transport import MAUI_FINAL_PID
from maui_libs.transport import MAUI_FINAL_VID
from maui_libs.transport import MAUI_IT82002_BOOTLOADER_PID
from maui_libs.transport import MSPM0_DEVICE_TYPES
from maui_libs.transport import VALID_DEVICE_TYPES
import serial
import serial.tools.list_ports


logger = logging.getLogger(__name__)

# GCS Bucket Base URL (Public)
GCS_BASE_URL = "https://storage.googleapis.com/maui-firmware"


def _list_gcs_keys(prefix: str) -> list[str]:
    """Lists all object keys in GCS matching the given prefix."""
    list_url = f"{GCS_BASE_URL}?prefix={prefix}"
    with urllib.request.urlopen(list_url) as response:
        xml_content = response.read()
    root = ET.fromstring(xml_content)
    keys = []
    for child in root:
        if child.tag.endswith("Contents"):
            for sub in child:
                if sub.tag.endswith("Key") and sub.text:
                    keys.append(sub.text)
                    break
    return keys


def _get_comp_subdir(component: str, device_type: str) -> str:
    """Returns the GCS firmware subdirectory for a component and device type."""
    if device_type not in VALID_DEVICE_TYPES:
        raise RuntimeError(
            f"Unknown device type '{device_type}'. "
            f"Specify one of {VALID_DEVICE_TYPES} via --device_type."
        )
    if component == "mcu":
        return "mspm0" if device_type in MSPM0_DEVICE_TYPES else device_type
    if device_type in (DEVICE_TYPE_V11, DEVICE_TYPE_V12):
        return "v11_v12"
    return device_type


def _match_tag_key(
    keys: list[str],
    version: str,
    component: str,
    tag_suffix: str,
    device_type: str,
) -> str:
    """Finds the matching tag marker key in GCS by priority."""
    comp_dir = f"{component}-fw"
    group_sub = _get_comp_subdir(component, device_type)
    plain_marker = f"/{version}{tag_suffix}"

    # 1. Device-specific or group-specific marker anywhere under comp_dir/
    for suffix in (
        f"/{version}.{device_type}{tag_suffix}",
        f"/{version}.{group_sub}{tag_suffix}",
    ):
        for key in keys:
            if key.endswith(suffix):
                return key

    # 2. Plain marker inside comp_dir/<device_or_group>/
    for sub in (device_type, group_sub):
        sub_prefix = f"{comp_dir}/{sub}/"
        for key in keys:
            if key.startswith(sub_prefix) and key.endswith(plain_marker):
                return key

    # 3. Legacy un-scoped marker fallback ONLY for compatible devices
    allow_legacy = (component == "mcu" and device_type in MSPM0_DEVICE_TYPES) or (
        component == "pdc" and device_type == DEVICE_TYPE_PROTO
    )
    if allow_legacy:
        for key in keys:
            if key.endswith(plain_marker) and key.count("/") == 2:
                logger.warning("Using legacy tag '%s'", key)
                return key

    raise RuntimeError(
        f"Tag '{version}' not found for {component} (device: {device_type}). "
        f"Expected marker '{version}.{device_type}{tag_suffix}' in {comp_dir}/."
    )


def _resolve_tag_to_dir(
    version: str,
    component: str,
    comp_dir: str,
    tag_suffix: str,
    device_type: str = DEVICE_TYPE_PROTO,
) -> str:
    """Resolves a channel tag ('stable', 'alpha', 'prev') to a directory key in GCS."""
    logger.info(
        "Resolving %s tag '%s' for device '%s' in %s/...",
        component,
        version,
        device_type,
        comp_dir,
    )
    try:
        keys = _list_gcs_keys(f"{comp_dir}/")
        found_key = _match_tag_key(keys, version, component, tag_suffix, device_type)
        target_dir_key = os.path.dirname(found_key)
        logger.info("Tag '%s' resolves to: %s", version, target_dir_key)
        return target_dir_key
    except Exception as e:
        raise RuntimeError(f"Failed to resolve tag '{version}': {e}") from e


def _find_binary_in_dir(
    target_dir_key: str,
    file_ext: str,
    preferred_name: str = None,
    target_stage: str = "app",
) -> str:
    """Finds the firmware binary in the specified directory key."""
    try:
        keys = _list_gcs_keys(f"{target_dir_key}/")
        candidates = [k for k in keys if k.endswith(file_ext)]
        if not candidates:
            raise RuntimeError(
                f"No '{file_ext}' firmware binary found in {target_dir_key}"
            )

        if preferred_name:
            for key in candidates:
                if os.path.basename(key) == preferred_name:
                    return f"{GCS_BASE_URL}/{key}"
            if target_stage == "rw-bootloader":
                raise RuntimeError(
                    f"RW bootloader binary '{preferred_name}' not found in "
                    f"{target_dir_key}"
                )

        if target_stage == "app" and len(candidates) > 1:
            app_bins = [
                k
                for k in candidates
                if not any(
                    x in os.path.basename(k).lower()
                    for x in ("bootloader", "full_flash", "standalone")
                )
            ]
            if app_bins:
                candidates = app_bins

        return f"{GCS_BASE_URL}/{candidates[0]}"
    except Exception as e:
        raise RuntimeError(f"Failed to locate binary in {target_dir_key}: {e}") from e


def _get_fw_spec(
    component: str, device_type: str, target_stage: str
) -> tuple[str, str, str, str]:
    """Returns (comp_dir, file_ext, tag_suffix, preferred_name) for a request."""
    if component == "pdc":
        return "pdc-fw", ".bin", ".pdcfw", None
    if component == "mcu":
        if device_type in ITE_DEVICE_TYPES:
            stage_map = {
                "rw-bootloader": "maui_rw_bootloader_signed.bin",
                "ro-bootloader": "maui_full_flash_1mb.bin",
            }
            pref = stage_map.get(target_stage, "maui_app_signed.bin")
            return "mcu-fw", ".bin", ".mcufw", pref
        return "mcu-fw", ".txt", ".mcufw", "zephyr.txt"
    return f"{component}-fw", ".bin", f".{component}fw", None


def _resolve_fw_url(
    component: str,
    version: str,
    device_type: str,
    target_stage: str,
) -> tuple[str, str]:
    """Resolves the full GCS download URL and file extension for a firmware request."""
    comp_dir, file_ext, tag_suffix, pref = _get_fw_spec(
        component, device_type, target_stage
    )
    if version in ["stable", "alpha", "prev"]:
        d_key = _resolve_tag_to_dir(
            version, component, comp_dir, tag_suffix, device_type=device_type
        )
        return (
            _find_binary_in_dir(
                d_key, file_ext, preferred_name=pref, target_stage=target_stage
            ),
            file_ext,
        )

    sub = _get_comp_subdir(component, device_type)
    last_err = None
    for d_key in (f"{comp_dir}/{sub}/{version}", f"{comp_dir}/{version}"):
        try:
            url = _find_binary_in_dir(
                d_key, file_ext, preferred_name=pref, target_stage=target_stage
            )
            return url, file_ext
        except RuntimeError as err:
            last_err = err
    raise last_err


def fetch_firmware(
    component: str,
    version: str = "stable",
    device_type: str = DEVICE_TYPE_PROTO,
    target_stage: str = "app",
) -> str:
    """Fetches the firmware binary for the component, version, and device type."""
    if device_type not in VALID_DEVICE_TYPES:
        raise RuntimeError(
            f"Cannot fetch cloud firmware for unknown device type '{device_type}'. "
            f"Please specify --device_type ({', '.join(VALID_DEVICE_TYPES)})."
        )

    bin_url, file_ext = _resolve_fw_url(component, version, device_type, target_stage)
    temp_fd, temp_path = tempfile.mkstemp(prefix=f"maui_{component}_", suffix=file_ext)
    os.close(temp_fd)

    try:
        logger.info("Downloading firmware from %s...", bin_url)
        with urllib.request.urlopen(bin_url) as response, open(
            temp_path, "wb"
        ) as out_file:
            shutil.copyfileobj(response, out_file)
        return temp_path
    except Exception as e:
        if os.path.exists(temp_path):
            os.remove(temp_path)
        raise RuntimeError(f"Download failed: {e}") from e


def do_update_pdc(device, fw_path):
    """Performs the PDC firmware update."""
    if not fw_path:
        logger.error("No firmware file specified.")
        sys.exit(1)
    logger.info("Starting PDC firmware update from %s", fw_path)
    try:
        with open(fw_path, "rb") as f:
            fw_data = f.read()
    except FileNotFoundError:
        logger.error("Firmware file not found: %s", fw_path)
        sys.exit(1)
    client = MauiPdcClientAdapter(device)
    updater = Tps6699xFirmwareUpdater(client)
    updater.update(fw_data)


def wait_for_port(port_path: str, exists: bool = True, timeout: float = 10.0):
    """Waits for a port path to exist or disappear."""
    logger.info(
        "Waiting for port %s to %s (timeout %.1fs)...",
        port_path,
        "appear" if exists else "disappear",
        timeout,
    )
    start_time = time.time()
    while True:
        current_exists = os.path.exists(port_path)
        if current_exists == exists:
            if exists:
                try:
                    ser = serial.Serial(port_path)
                    ser.close()
                    break
                except (serial.SerialException, PermissionError):
                    pass
            else:
                break

        if time.time() - start_time > timeout:
            raise TimeoutError(
                f"Timeout waiting for port {port_path} to "
                f"{'appear' if exists else 'disappear'}"
            )
        time.sleep(0.5)


def find_it82002_bootloader_port(timeout: float = 10.0) -> tuple[str, str]:
    """Polls USB ports for an IT82002 MCUboot Serial Recovery device.

    Returns:
        tuple[str, str]: (port_device_path, usb_product_string)
    """
    logger.info(
        "Waiting for IT82002 MCUboot Serial Recovery device "
        "(VID 0x%04x, PID 0x%04x)...",
        MAUI_FINAL_VID,
        MAUI_FINAL_PID,
    )
    start_time = time.time()
    while time.time() - start_time < timeout:
        for port in serial.tools.list_ports.comports():
            if port.vid == MAUI_FINAL_VID and port.pid in (
                MAUI_FINAL_PID,
                MAUI_IT82002_BOOTLOADER_PID,
            ):
                product = port.product or ""
                if (
                    "BOOTLOADER" in product.upper()
                    or port.pid == MAUI_IT82002_BOOTLOADER_PID
                ):
                    logger.info(
                        "Found IT82002 MCUboot Serial Recovery device '%s' on port %s",
                        product or "Maui Bootloader",
                        port.device,
                    )
                    return port.device, (product or "Maui Bootloader")
        time.sleep(0.5)

    raise TimeoutError(
        "Timeout waiting for IT82002 MCUboot Serial Recovery device to appear."
    )


def _enter_it82002_recovery(device, target_stage: str) -> tuple[str, str]:
    """Gets the IT82002 into MCUboot serial recovery and returns (port, product).

    Depending on what we are connected to, this either uses the recovery port we
    are already sitting on, or asks the running App to reboot into the right
    bootloader and then waits for it to re-enumerate.
    """
    if device and device.is_bootloader:
        recovery_port = device.port
        for p in serial.tools.list_ports.comports():
            if p.device == recovery_port:
                return recovery_port, (p.product or "")
        return recovery_port, ""

    if not device:
        return find_it82002_bootloader_port(timeout=5.0)

    logger.info(
        "Connecting to Maui device to request %s recovery mode...", target_stage
    )
    try:
        device.connect()
        if target_stage == "rw-bootloader":
            logger.info("Sending 'reboot ro-bootloader' (ETWD hardware reset)...")
            device.send_command_raw("reboot ro-bootloader", timeout=2.0)
        else:
            logger.info("Sending 'reboot bootloader' (RW Serial Recovery)...")
            device.send_command_raw("reboot bootloader", timeout=2.0)
    except Exception as e:  # pylint: disable=broad-exception-caught
        logger.debug("Reboot command disconnected serial as expected: %s", e)
    finally:
        device.disconnect()

    time.sleep(1.5)
    return find_it82002_bootloader_port(timeout=12.0)


def wait_for_ite_app_port(device: MauiDevice, timeout: float = 15.0):
    """Waits for an ITE device to reboot back into the Main Application."""
    logger.info(
        "Waiting for ITE Main Application to enumerate (timeout %.1fs)...",
        timeout,
    )
    time.sleep(1.5)
    start_time = time.time()
    target_sn = (
        device.serial_number
        if device.serial_number not in (None, "", "Explicit Port", "No Serial Number")
        else None
    )

    while time.time() - start_time < timeout:
        candidates = [
            d
            for d in list_maui_devices()
            if d["type"] in ITE_DEVICE_TYPES and not d.get("is_bootloader")
        ]
        if target_sn:
            # Only accept the target device; never fall back to another ITE
            # device that happens to be connected while the target reboots.
            candidates = [d for d in candidates if d.get("serial_number") == target_sn]

        for dev_info in candidates:
            port_path = dev_info.get("port")
            if port_path and os.path.exists(port_path):
                try:
                    ser = serial.Serial(port_path)
                    ser.close()
                    logger.info("ITE Main Application is back online on %s", port_path)
                    device.set_port(port_path, is_bootloader=False)
                    return
                except (serial.SerialException, PermissionError, OSError):
                    pass
        time.sleep(0.5)

    raise TimeoutError(
        "Timeout waiting for ITE Main Application serial port to appear after reboot."
    )


def do_update_it82002_mcuboot(device, fw_path: str, target_stage: str = "app"):
    """Performs an MCU firmware or RW bootloader update on IT82002 via MCUboot SMP."""
    if not os.path.exists(fw_path):
        logger.error("Firmware file not found: %s", fw_path)
        sys.exit(1)

    if target_stage == "ro-bootloader":
        logger.warning(
            "RO Bootloader is hardware write-protected after boot. To update RO "
            "Bootloader, use an external programmer."
        )

    recovery_port, bootloader_product = _enter_it82002_recovery(device, target_stage)

    # MCUboot serial recovery can only write slot 0, whichever bootloader is
    # running: `smpmgr --slot N` sets the SMP *image number*, not the MCUboot
    # slot, and we build with a single updatable image, so only 0 is valid.
    # - In RW Bootloader: slot 0 = Main App (0x38000)
    # - In RO Bootloader: slot 0 = RW Bootloader (0x18000)
    # The App is therefore not reachable from RO recovery.
    is_ro_bootloader = "RO" in bootloader_product.upper()
    if is_ro_bootloader and target_stage == "app":
        logger.error(
            "Cannot flash the App from the RO Recovery Bootloader; it can only "
            "write the RW Bootloader. Flash the RW Bootloader first "
            "(--target rw-bootloader), then reset: the RW Bootloader will drop "
            "into its own serial recovery and the App can be flashed from there."
        )
        sys.exit(1)
    slot_index = 0

    logger.info(
        "Uploading %s to %s (bootloader='%s', slot=%d) via smpmgr...",
        fw_path,
        recovery_port,
        bootloader_product or "unknown",
        slot_index,
    )
    if not shutil.which("smpmgr"):
        logger.error("smpmgr not found in PATH. Install smpmgr (pip install smpmgr).")
        sys.exit(1)

    upload_cmd = [
        "smpmgr",
        "--port",
        recovery_port,
        "upgrade",
        "--slot",
        str(slot_index),
        fw_path,
    ]
    logger.info("Running: %s", " ".join(upload_cmd))
    subprocess.check_call(upload_cmd)

    logger.info("IT82002 %s update completed successfully.", target_stage)


# This is the single dispatch point for every MCU update variant (MSPM0 vs
# IT82002, USB vs UART, App vs bootloader), so it genuinely needs all of these.
def do_update_mcu(  # pylint: disable=too-many-arguments
    fw_path,
    *,
    bsl_mode=False,
    use_usb=False,
    port=None,
    device=None,
    target_stage="app",
):
    """Performs the MCU firmware update."""
    if not fw_path:
        logger.error("No firmware file specified.")
        sys.exit(1)

    # Dispatch to IT82002 MCUboot SMP updater if device is ITE or stage is specified
    if (
        (device and device.is_ite)
        or target_stage in ("rw-bootloader", "ro-bootloader")
        or fw_path.endswith(".bin")
    ):
        do_update_it82002_mcuboot(device, fw_path, target_stage=target_stage)
        return

    logger.info("Starting MSPM0 MCU firmware update from %s", fw_path)
    if not shutil.which("fw-updater"):
        logger.error("fw-updater tool not found in PATH.")
        sys.exit(1)
    try:
        # Invoke fw-updater
        cmd = ["fw-updater", fw_path]
        if use_usb:
            cmd.append("--usb")
        if port:
            cmd.extend(["--uart", port])

        if bsl_mode:
            cmd.append("--bsl-mode")

        if logger.isEnabledFor(logging.DEBUG):
            cmd.append("-v")

        logger.info("Running: %s", " ".join(cmd))
        subprocess.check_call(cmd)
        logger.info("MCU firmware update successful.")
    except subprocess.CalledProcessError as e:
        logger.error("MCU firmware update failed: %s", e)
        sys.exit(1)


def handle_all_update(device, version, bsl_mode=False, use_usb=False):
    """Handles updating all components (MCU then PDC)."""
    # 1. Fetch Firmware for the detected hardware variant
    mcu_fw = None
    pdc_fw = None
    try:
        mcu_fw = fetch_firmware(
            "mcu", version, device_type=device.device_type, target_stage="app"
        )
        pdc_fw = fetch_firmware("pdc", version, device_type=device.device_type)
    except RuntimeError as e:
        logger.error(e)
        sys.exit(1)

    try:
        port_path = device.port
        # 2. Update MCU
        if not device.is_ite:
            device.connect()
            device.disconnect()

        do_update_mcu(
            mcu_fw,
            bsl_mode=bsl_mode,
            use_usb=use_usb,
            port=port_path,
            device=device,
            target_stage="app",
        )

        if device.is_ite:
            try:
                wait_for_ite_app_port(device, timeout=15.0)
            except TimeoutError as e:
                logger.error(e)
                sys.exit(1)
        elif use_usb and port_path:
            try:
                wait_for_port(port_path, exists=True, timeout=10.0)
            except TimeoutError as e:
                logger.error(e)
                sys.exit(1)
        else:
            logger.info("Waiting for device to settle after MCU update...")
            time.sleep(1)

        # 3. Update PDC
        device.connect()
        do_update_pdc(device, pdc_fw)

    finally:
        # Cleanup
        for p in [mcu_fw, pdc_fw]:
            if p and os.path.exists(p):
                os.remove(p)


def handle_pdc_update(device, args):
    """Handles updating only the PDC component."""
    fw_path = None
    temp_file = None
    if args.file:
        fw_path = args.file
    elif args.fw_channel:
        try:
            fw_path = fetch_firmware(
                "pdc", args.fw_channel, device_type=device.device_type
            )
            temp_file = fw_path
        except RuntimeError as e:
            logger.error(e)
            sys.exit(1)

    try:
        device.connect()
        do_update_pdc(device, fw_path)
    finally:
        if temp_file and os.path.exists(temp_file):
            os.remove(temp_file)


def handle_mcu_update(device, args):
    """Handles updating only the MCU component."""
    target_stage = getattr(args, "target", "app")
    fw_path = None
    temp_file = None
    if args.file:
        fw_path = args.file
    elif args.fw_channel:
        try:
            fw_path = fetch_firmware(
                "mcu",
                args.fw_channel,
                device_type=device.device_type,
                target_stage=target_stage,
            )
            temp_file = fw_path
        except RuntimeError as e:
            logger.error(e)
            sys.exit(1)

    try:
        port_path = device.port
        do_update_mcu(
            fw_path,
            bsl_mode=args.bsl_mode,
            use_usb=args.usb,
            port=port_path,
            device=device,
            target_stage=target_stage,
        )
        if device.is_ite and target_stage == "app":
            try:
                wait_for_ite_app_port(device, timeout=15.0)
            except TimeoutError as e:
                logger.error(e)
                sys.exit(1)
        elif args.usb and port_path:
            try:
                wait_for_port(port_path, exists=True, timeout=10.0)
            except TimeoutError as e:
                logger.error(e)
                sys.exit(1)
    finally:
        if temp_file and os.path.exists(temp_file):
            os.remove(temp_file)


class _H2hProgressPrinter:
    """Progress display for H2H flashing.

    Called from both the main thread (writes) and the RPC reader thread
    (erase/verify events). On a terminal it redraws one line per phase;
    otherwise (CI logs) it logs every 10%.
    """

    def __init__(self):
        self._lock = threading.Lock()
        self._tty = sys.stderr.isatty()
        self._phase = None
        self._pct = -1

    def __call__(self, phase: str, done: int, total: int):
        pct = done * 100 // total if total else 100
        with self._lock:
            if phase == self._phase and pct == self._pct:
                return
            if not self._tty:
                if phase != self._phase or pct >= self._pct + 10 or pct == 100:
                    logger.info("  %s %3d%% (%d/%d B)", phase, pct, done, total)
                    self._phase, self._pct = phase, pct
                return
            if self._phase and phase != self._phase:
                sys.stderr.write("\n")
            self._phase, self._pct = phase, pct
            sys.stderr.write(f"\r  {phase:<8} {pct:3d}%  ({done}/{total} B)")
            sys.stderr.flush()

    def done(self):
        """Terminates the progress line."""
        with self._lock:
            if self._tty and self._phase:
                sys.stderr.write("\n")
                sys.stderr.flush()


def handle_h2h(args):
    """Reads or programs the Prolific PL27A1 H2H bridge SPI flash over RPC.

    Everything goes over the device's "Maui RPC" CDC-ACM interface, so the
    shell port may be busy (e.g. open in a terminal). Writing erases the whole
    flash, programs the image and verifies it with a CRC-32 read back on the
    MCU side; reading streams the flash back from offset 0.
    """
    image = None
    if args.file:
        try:
            with open(args.file, "rb") as f:
                image = f.read()
        except OSError as e:
            raise RuntimeError(f"Cannot read H2H image: {e}") from e

    rpc_port = args.rpc_port or find_rpc_port(
        serial_number=args.serial, console_port=args.port
    )
    logger.info("Using RPC port %s", rpc_port)

    with RpcClient(rpc_port) as rpc:
        flasher = H2hFlasher(rpc)

        if args.read:
            flasher.check_supported("h2h_read_begin")
            _h2h_read(flasher, args.read, args.size)
            return

        flasher.check_supported()

        if args.info:
            logger.info("H2H flash: %s", flasher.info())
            return

        logger.info(
            "Flashing %s (%d B, CRC-32 0x%08X) to the H2H SPI flash...",
            args.file,
            len(image),
            zlib.crc32(image) & 0xFFFFFFFF,
        )
        printer = _H2hProgressPrinter()
        try:
            result = flasher.flash(image, progress=printer)
        finally:
            printer.done()

    logger.info(
        "H2H flash programmed and verified: %s, %d B in %.1f s "
        "(device %.1f s, %.0f KB/s)",
        result.info,
        result.image_size,
        result.host_s,
        result.device_ms / 1000.0,
        result.image_size / 1024.0 / max(result.host_s, 1e-3),
    )


def _h2h_read(flasher, out_file, size):
    """Dumps the first `size` bytes of the H2H SPI flash to out_file."""
    printer = _H2hProgressPrinter()
    try:
        result = flasher.read(size, progress=printer)
    finally:
        printer.done()

    try:
        with open(out_file, "wb") as f:
            f.write(result.data)
    except OSError as e:
        raise RuntimeError(f"Cannot write {out_file}: {e}") from e

    logger.info(
        "H2H flash dumped to %s: %s, %d B (CRC-32 0x%08X) in %.1f s (%.0f KB/s)",
        out_file,
        result.info,
        len(result.data),
        zlib.crc32(result.data) & 0xFFFFFFFF,
        result.host_s,
        len(result.data) / 1024.0 / max(result.host_s, 1e-3),
    )


def handle_serial_cmd(device, args):
    """Handles reading or writing the persistent USB serial number in NVS."""
    device.connect()
    if getattr(args, "set_sn", None):
        logger.info("Setting NVS serial number to: %s", args.set_sn)
        resp = device.send_command(f"serial set {args.set_sn}")
        logger.info("Response: %s", resp)
    else:
        resp = device.send_command("serial get")
        logger.info("Current NVS Serial Number:\n%s", resp)


def main():
    """Main entry point for the maui-flash CLI."""
    parser = argparse.ArgumentParser(description="Maui Firmware Flashing CLI Tool")
    cli.add_common_args(parser)

    subparsers = parser.add_subparsers(dest="component", help="Component to flash")

    # MCU options shared between 'mcu' and 'all'
    mcu_options = argparse.ArgumentParser(add_help=False)
    mcu_options.add_argument(
        "--bsl-mode",
        action="store_true",
        help="Pass --bsl-mode flag to fw-updater during the MCU update portion.",
    )
    mcu_options.add_argument(
        "--target",
        choices=["app", "rw-bootloader", "ro-bootloader"],
        default="app",
        help=(
            "Target partition on IT82002 ('app', 'rw-bootloader', or "
            "'ro-bootloader')."
        ),
    )

    # PDC subcommand
    pdc_parser = subparsers.add_parser(
        "pdc", help="Update Power Delivery Controller (PDC) firmware"
    )
    pdc_group = pdc_parser.add_mutually_exclusive_group(required=True)
    pdc_group.add_argument(
        "--file",
        help="Path to the local PDC firmware binary file",
    )
    pdc_group.add_argument(
        "--fw_channel",
        help="Fetch and flash a specific version (e.g., 'stable', 'v1.2.3')",
    )

    # MCU subcommand
    mcu_parser = subparsers.add_parser(
        "mcu",
        parents=[mcu_options],
        help="Update Microcontroller (MCU) firmware",
    )
    mcu_group = mcu_parser.add_mutually_exclusive_group(required=True)
    mcu_group.add_argument(
        "--file",
        help=(
            "Path to the local MCU firmware binary file "
            "(IT82002 signed .bin or MSPM0 .txt)"
        ),
    )
    mcu_group.add_argument(
        "--fw_channel",
        help="Fetch and flash a specific version (e.g., 'stable', '0.27-18b13be')",
    )

    # H2H subcommand
    h2h_parser = subparsers.add_parser(
        "h2h",
        help="Read or program the Prolific PL27A1 H2H bridge SPI flash",
    )
    h2h_group = h2h_parser.add_mutually_exclusive_group(required=True)
    h2h_group.add_argument(
        "--file",
        help=(
            "Raw H2H SPI flash image (.bin, programmed at offset 0; the whole "
            "flash is erased first)"
        ),
    )
    h2h_group.add_argument(
        "--info",
        action="store_true",
        help="Only print the H2H SPI flash JEDEC ID and size",
    )
    h2h_group.add_argument(
        "--read",
        metavar="OUT_FILE",
        help=(
            "Dump the H2H SPI flash from offset 0 to OUT_FILE (the H2H bridge "
            "is held in reset while reading)"
        ),
    )
    h2h_parser.add_argument(
        "--size",
        type=int,
        default=H2H_IMAGE_SIZE,
        help=(
            f"Number of bytes to dump with --read (default: {H2H_IMAGE_SIZE}, "
            "the size of a PL27A1 image)"
        ),
    )
    h2h_parser.add_argument(
        "--rpc-port",
        help="RPC port of the device (default: auto-detect the 'Maui RPC' port)",
    )

    # Serial Number (NVS) subcommand
    sn_parser = subparsers.add_parser(
        "serial", help="Read or write persistent USB serial number in NVS flash"
    )
    sn_parser.add_argument(
        "--set",
        dest="set_sn",
        help=(
            "Write a new persistent USB serial number to NVS "
            "(reboot required to take effect)"
        ),
    )

    # All subcommand
    all_parser = subparsers.add_parser(
        "all",
        parents=[mcu_options],
        help="Update ALL components (MCU then PDC) from cloud",
    )
    all_parser.add_argument(
        "--fw_channel",
        default="stable",
        help="Fetch and flash a specific version/tag (default: 'stable')",
    )

    args = parser.parse_args()
    cli.handle_common_args(args)

    if not args.component:
        parser.print_help()
        sys.exit(1)

    # Main Execution Logic
    # H2H talks only to the RPC port; don't require (or open) the shell port.
    if args.component == "h2h":
        handle_h2h(args)
        return

    device = None
    try:
        device = MauiDevice.find_device(
            serial_number=args.serial,
            port=args.port,
            device_type=args.device_type,
        )

        if args.component == "all":
            handle_all_update(
                device, args.fw_channel, bsl_mode=args.bsl_mode, use_usb=args.usb
            )

        elif args.component == "pdc":
            handle_pdc_update(device, args)

        elif args.component == "mcu":
            handle_mcu_update(device, args)

        elif args.component == "serial":
            handle_serial_cmd(device, args)

    finally:
        if device and device.is_connected:
            device.disconnect()


if __name__ == "__main__":
    cli.setup_logging()
    try:
        main()
    except RuntimeError as e:
        logger.error("Command failed: %s", e)
        sys.exit(1)
    except KeyboardInterrupt:
        logger.error("Interrupted")
        sys.exit(130)
    except Exception as e:  # pylint: disable=broad-exception-caught
        logger.critical("An unexpected error occurred: %s", e, exc_info=True)
        sys.exit(1)
