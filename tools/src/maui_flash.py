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
import time
import urllib.error
import urllib.request
import xml.etree.ElementTree as ET

from maui_libs import cli
from maui_libs.device import MauiDevice
from maui_libs.pdc import MauiPdcClientAdapter
from maui_libs.pdc import Tps6699xFirmwareUpdater
from maui_libs.transport import MAUI_FINAL_PID
from maui_libs.transport import MAUI_FINAL_VID
from maui_libs.transport import MAUI_IT82002_BOOTLOADER_PID
import serial
import serial.tools.list_ports


logger = logging.getLogger(__name__)

# GCS Bucket Base URL (Public)
GCS_BASE_URL = "https://storage.googleapis.com/maui-firmware"


def _resolve_tag_to_dir(
    version: str, component: str, comp_dir: str, tag_suffix: str
) -> str:
    """Resolves a version tag to a directory key in GCS."""
    list_url = f"{GCS_BASE_URL}?prefix={comp_dir}/"
    logger.info("Resolving tag '%s' by listing %s...", version, list_url)

    try:
        with urllib.request.urlopen(list_url) as response:
            xml_content = response.read()

        # Parse XML.
        root = ET.fromstring(xml_content)

        found_key = None
        for child in root:
            if child.tag.endswith("Contents"):
                key_elem = None
                for sub in child:
                    if sub.tag.endswith("Key"):
                        key_elem = sub
                        break

                if key_elem is not None:
                    key = key_elem.text
                    if key.endswith(f"/{version}{tag_suffix}"):
                        found_key = key
                        break

        if not found_key:
            raise RuntimeError(f"Tag '{version}' not found for {component}")

        # Found key: pdc-fw/v10_122/stable.pdcfw
        # Parent dir: pdc-fw/v10_122
        target_dir_key = os.path.dirname(found_key)
        logger.info("Tag '%s' resolves to: %s", version, target_dir_key)
        return target_dir_key

    except Exception as e:
        raise RuntimeError(f"Failed to resolve tag '{version}': {e}") from e


def _find_binary_in_dir(target_dir_key: str, file_ext: str) -> str:
    """Finds the firmware binary in the specified directory key."""
    bin_url = ""
    try:
        list_url = f"{GCS_BASE_URL}?prefix={target_dir_key}/"
        with urllib.request.urlopen(list_url) as response:
            xml_content = response.read()

        root = ET.fromstring(xml_content)

        candidates = []
        for child in root:
            if child.tag.endswith("Contents"):
                key_elem = None
                for sub in child:
                    if sub.tag.endswith("Key"):
                        key_elem = sub
                        break

                if key_elem is not None:
                    key = key_elem.text
                    if key.endswith(file_ext):
                        candidates.append(key)

        if not candidates:
            raise RuntimeError(f"No firmware binary found in {target_dir_key}")

        # Pick the first one
        bin_key = candidates[0]
        bin_url = f"{GCS_BASE_URL}/{bin_key}"
        return bin_url

    except Exception as e:
        raise RuntimeError(f"Failed to locate binary in {target_dir_key}: {e}") from e


def fetch_firmware(component: str, version: str = "stable") -> str:
    """
    Fetches the firmware binary for the specified component and version
    using HTTP (public GCS bucket).
    Returns the path to the downloaded file.
    """
    # Component Configuration
    if component == "pdc":
        comp_dir = "pdc-fw"
        file_ext = ".bin"
        tag_suffix = ".pdcfw"
    elif component == "mcu":
        comp_dir = "mcu-fw"
        file_ext = ".txt"
        tag_suffix = ".mcufw"
    else:
        # Fallback/Placeholder
        comp_dir = f"{component}-fw"
        file_ext = ".bin"
        tag_suffix = f".{component}fw"

    target_dir_key = ""

    # Resolve Version/Tag
    if version in ["stable", "alpha", "prev"]:
        target_dir_key = _resolve_tag_to_dir(version, component, comp_dir, tag_suffix)
    else:
        # Explicit version
        target_dir_key = f"{comp_dir}/{version}"

    # Now find the binary in the target directory
    bin_url = _find_binary_in_dir(target_dir_key, file_ext)

    # Download
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
    if device and device.device_type == "it82002_bootloader":
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

    # Dispatch to IT82002 MCUboot SMP updater if device is IT82002 or stage is specified
    if (
        (device and device.device_type in ("it82002_app", "it82002_bootloader"))
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
    """Handles updating all components."""
    # 1. Fetch Firmware
    mcu_fw = None
    pdc_fw = None
    try:
        mcu_fw = fetch_firmware("mcu", version)
        pdc_fw = fetch_firmware("pdc", version)
    except RuntimeError as e:
        logger.error(e)
        sys.exit(1)

    try:
        port_path = device.port
        # 2. Update MCU
        device.connect()
        device.disconnect()

        do_update_mcu(
            mcu_fw,
            bsl_mode=bsl_mode,
            use_usb=use_usb,
            port=port_path,
            device=device,
        )

        if use_usb and port_path:
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
            fw_path = fetch_firmware("pdc", args.fw_channel)
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
    fw_path = None
    temp_file = None
    if args.file:
        fw_path = args.file
    elif args.fw_channel:
        try:
            fw_path = fetch_firmware("mcu", args.fw_channel)
            temp_file = fw_path
        except RuntimeError as e:
            logger.error(e)
            sys.exit(1)

    target_stage = getattr(args, "target", "app")

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
        if args.usb and port_path and device.device_type == "final_native_usb":
            try:
                wait_for_port(port_path, exists=True, timeout=10.0)
            except TimeoutError as e:
                logger.error(e)
                sys.exit(1)
    finally:
        if temp_file and os.path.exists(temp_file):
            os.remove(temp_file)


def handle_h2h_update(device, args):
    """Handles Prolific PL27A1 H2H external SPI flash operations."""
    device.connect()
    try:
        logger.info("Acquiring H2H SPI flash bus...")
        resp = device.send_command("h2h_flash enable")
        logger.info("h2h_flash enable: %s", resp)

        status_resp = device.send_command("h2h_flash status")
        logger.info("h2h_flash status:\n%s", status_resp)

        if getattr(args, "erase", False):
            logger.info("Erasing H2H SPI flash sector 0...")
            erase_resp = device.send_command("h2h_flash erase 0x0 4096")
            logger.info("h2h_flash erase: %s", erase_resp)

        if getattr(args, "file", None):
            logger.info(
                "H2H flash image provided (%s). Use SPI flasher or shell "
                "stream to write.",
                args.file,
            )
    finally:
        logger.info("Releasing H2H SPI flash bus back to PL27A1...")
        rel_resp = device.send_command("h2h_flash disable")
        logger.info("h2h_flash disable: %s", rel_resp)


def handle_serial_cmd(device, args):
    """Handles reading or writing the persistent USB serial number in NVS."""
    device.connect()
    if getattr(args, "set_sn", None):
        logger.info("Setting NVS serial number to: %s", args.set_sn)
        resp = device.send_command(f"nvs_sn set {args.set_sn}")
        logger.info("Response: %s", resp)
    else:
        resp = device.send_command("nvs_sn get")
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
        help="Manage or update Prolific PL27A1 Host-to-Host (H2H) bridge SPI flash",
    )
    h2h_parser.add_argument(
        "--file",
        help="Path to local H2H SPI flash binary file",
    )
    h2h_parser.add_argument(
        "--erase",
        action="store_true",
        help="Erase first 4KB sector of H2H SPI flash",
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
    device = None
    try:
        device = MauiDevice.find_device(serial_number=args.serial, port=args.port)

        if args.component == "all":
            handle_all_update(
                device, args.fw_channel, bsl_mode=args.bsl_mode, use_usb=args.usb
            )

        elif args.component == "pdc":
            handle_pdc_update(device, args)

        elif args.component == "mcu":
            handle_mcu_update(device, args)

        elif args.component == "h2h":
            handle_h2h_update(device, args)

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
    except Exception as e:  # pylint: disable=broad-exception-caught
        logger.critical("An unexpected error occurred: %s", e, exc_info=True)
        sys.exit(1)
