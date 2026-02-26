# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""
Maui Control CLI Tool.

This script provides a command-line interface for controlling and querying
Maui devices. It supports status retrieval and commands for
power and data control.
"""

import argparse
import logging
import sys

from maui_libs import cli
from maui_libs.device import MauiDevice


logger = logging.getLogger(__name__)


def main():
    """Main entry point for the maui-ctl CLI."""
    parser = argparse.ArgumentParser(description="Maui Control CLI Tool")
    cli.add_common_args(parser)

    subparsers = parser.add_subparsers(dest="command", help="Available commands")

    # Status command
    subparsers.add_parser("status", help="Get current status of the Maui device")

    # Power command
    power_parser = subparsers.add_parser("power", help="Control power to the DUT")
    power_parser.add_argument(
        "state",
        choices=["on", "off", "cycle"],
        help="Power state: on, off, or cycle",
    )

    # Data command
    data_parser = subparsers.add_parser(
        "data", help="Control USB data lines to the DUT"
    )
    data_parser.add_argument(
        "state",
        choices=["on", "off", "cycle"],
        help="Data state: on, off, or cycle",
    )

    args = parser.parse_args()
    cli.handle_common_args(args)

    if not args.command:
        parser.print_help()
        sys.exit(1)

    device = None
    try:
        device = MauiDevice.find_device(serial_number=args.serial)
        device.connect()

        if args.command == "status":
            logger.info("Fetching device status...")
            response = device.send_command("version")
            logger.info("Device Version: %s", response.strip())
        elif args.command == "power":
            logger.info("Setting power state to %s...", args.state)
            response = device.set_power(args.state)
            logger.info("Response: %s", response.strip())
        elif args.command == "data":
            logger.info("Setting data state to %s...", args.state)
            response = device.set_data(args.state)
            logger.info("Response: %s", response.strip())

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
