# Copyright 2025 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""
Maui Control CLI Tool.

This script provides a command-line interface for controlling and querying
Maui devices. It supports status retrieval and placeholder commands for
power and data control.
"""

import argparse
import logging
import sys

from maui_libs.device import MauiDevice


logger = logging.getLogger(__name__)


def main():
    """Main entry point for the maui-ctl CLI."""
    parser = argparse.ArgumentParser(description="Maui Control CLI Tool")
    parser.add_argument(
        "--serial",
        help="Specify the serial number of the Maui device to connect to.",
    )

    subparsers = parser.add_subparsers(
        dest="command", required=True, help="Available commands"
    )

    # Status command
    subparsers.add_parser("status", help="Get current status of the Maui device")

    # Placeholder for Power command
    power_parser = subparsers.add_parser(
        "power", help="(Placeholder) Control power to the DUT"
    )
    power_parser.add_argument(
        "state",
        choices=["on", "off", "cycle"],
        help="Power state: on, off, or cycle",
    )

    # Placeholder for Data command
    data_parser = subparsers.add_parser(
        "data", help="(Placeholder) Control USB data lines to the DUT"
    )
    data_parser.add_argument(
        "state",
        choices=["connect", "disconnect"],
        help="Data line state: connect or disconnect",
    )

    args = parser.parse_args()

    device = None
    try:
        device = MauiDevice.find_device(serial_number=args.serial)
        device.connect()

        if args.command == "status":
            logger.info("Fetching device status...")
            response = device.send_command("version")
            logger.info("Device Version: %s", response.strip())
        elif args.command == "power":
            logger.info("Power command (%s) not yet supported by firmware.", args.state)
        elif args.command == "data":
            logger.info("Data command (%s) not yet supported by firmware.", args.state)

    except RuntimeError as e:
        logger.error("Command failed: %s", e)
        sys.exit(1)
    except Exception as e:  # pylint: disable=broad-exception-caught
        logger.critical("An unexpected error occurred: %s", e, exc_info=True)
        sys.exit(1)
    finally:
        if device and device.is_connected:
            device.disconnect()


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
    main()
