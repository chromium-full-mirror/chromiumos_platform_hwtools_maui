# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""
Maui Firmware Flashing CLI Tool.

This script provides a command-line interface for updating firmware on Maui
devices, supporting various components like the MCU and PDC.
"""

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


def do_update_mcu(fw_path, bsl_mode=False):
    """Performs the MCU firmware update."""
    if not fw_path:
        logger.error("No firmware file specified.")
        sys.exit(1)
    logger.info("Starting MCU firmware update from %s", fw_path)
    if not shutil.which("fw-updater"):
        logger.error("fw-updater tool not found in PATH.")
        sys.exit(1)
    try:
        # Invoke fw-updater
        cmd = ["fw-updater", fw_path]
        if bsl_mode:
            cmd.append("--bsl-mode")
        subprocess.check_call(cmd)
        logger.info("MCU firmware update successful.")
    except subprocess.CalledProcessError as e:
        logger.error("MCU firmware update failed: %s", e)
        sys.exit(1)


def handle_all_update(device, version, bsl_mode=False):
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
        # 2. Update MCU
        # MCU update requires disconnected port for fw-updater
        device.connect()  # Check connection
        device.disconnect()

        do_update_mcu(mcu_fw, bsl_mode=bsl_mode)

        logger.info("Waiting for device to settle after MCU update...")
        time.sleep(1)

        # 3. Update PDC
        # PDC update requires connected MauiDevice
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

    try:
        device.connect()
        device.disconnect()
        do_update_mcu(fw_path, bsl_mode=args.bsl_mode)
    finally:
        if temp_file and os.path.exists(temp_file):
            os.remove(temp_file)


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
        help="Path to the local MCU firmware binary file (TI .txt format)",
    )
    mcu_group.add_argument(
        "--fw_channel",
        help="Fetch and flash a specific version (e.g., 'stable', '0.27-18b13be')",
    )

    # H2H subcommand
    subparsers.add_parser(
        "h2h", help="(Placeholder) Update Host-to-Host (H2H) bridge firmware"
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
        device = MauiDevice.find_device(serial_number=args.serial)

        if args.component == "all":
            handle_all_update(device, args.fw_channel, bsl_mode=args.bsl_mode)

        elif args.component == "pdc":
            handle_pdc_update(device, args)

        elif args.component == "mcu":
            handle_mcu_update(device, args)

        elif args.component == "h2h":
            logger.info("H2H firmware update not yet implemented.")

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
