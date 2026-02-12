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
import sys

from maui_libs.device import MauiDevice
from maui_libs.pdc import MauiPdcClientAdapter
from maui_libs.pdc import Tps6699xFirmwareUpdater


LOG = logging.getLogger(__name__)


def main():
    """Main entry point for the maui-flash CLI."""
    parser = argparse.ArgumentParser(description="Maui Firmware Flashing CLI Tool")
    parser.add_argument(
        "--serial",
        help="Specify the serial number of the Maui device to connect to.",
    )

    subparsers = parser.add_subparsers(
        dest="component", required=True, help="Component to flash"
    )

    # PDC subcommand
    pdc_parser = subparsers.add_parser(
        "pdc", help="Update Power Delivery Controller (PDC) firmware"
    )
    pdc_parser.add_argument(
        "--file",
        required=True,
        help="Path to the PDC firmware binary file (TI TFU Bundle format)",
    )

    # Placeholder for MCU subcommand
    mcu_parser = subparsers.add_parser(
        "mcu", help="(Placeholder) Update Microcontroller (MCU) firmware"
    )
    # Placeholder for H2H subcommand
    h2h_parser = subparsers.add_parser(
        "h2h", help="(Placeholder) Update Host-to-Host (H2H) bridge firmware"
    )

    args = parser.parse_args()

    device = None
    try:
        device = MauiDevice.find_device(serial_number=args.serial)
        device.connect()

        if args.component == "pdc":
            LOG.info("Starting PDC firmware update from %s", args.file)

            try:
                with open(args.file, "rb") as f:
                    fw_data = f.read()
            except FileNotFoundError:
                LOG.error("Firmware file not found: %s", args.file)
                sys.exit(1)

            client = MauiPdcClientAdapter(device)
            updater = Tps6699xFirmwareUpdater(client)

            updater.update(fw_data)
            # Tps6699xFirmwareUpdater.update raises Exception on failure, returns on success

        elif args.component == "mcu":
            LOG.info("MCU firmware update not yet implemented.")
        elif args.component == "h2h":
            LOG.info("H2H firmware update not yet implemented.")

    except RuntimeError as e:
        LOG.error("Command failed: %s", e)
        sys.exit(1)
    except Exception as e:  # pylint: disable=broad-exception-caught
        LOG.critical("An unexpected error occurred: %s", e, exc_info=True)
        sys.exit(1)
    finally:
        if device and device.is_connected:
            device.disconnect()


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
    main()
