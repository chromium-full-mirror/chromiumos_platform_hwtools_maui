# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""
Maui Device Discovery Test Script.

This script tests the device discovery functionality of the maui-libs package.
It can scan for connected Maui devices and print their details.
"""

import argparse
import logging
import sys

from maui_libs.device import MauiDevice


logger = logging.getLogger(__name__)


def main():
    """Main entry point for discovery test."""
    parser = argparse.ArgumentParser(description="Maui Device Discovery Test")
    parser.add_argument(
        "--serial",
        help="Specify the serial number of the Maui device to connect to.",
    )
    args = parser.parse_args()

    try:
        device = MauiDevice.find_device(serial_number=args.serial)
        logger.info("Successfully found and initialized Maui device:")
        logger.info("  Serial Number: %s", device.serial_number)
        logger.info("  Port: %s", device.port)
        logger.info("  Type: %s", device.device_type)
        logger.info("  VID: %s", device.vid)
        logger.info("  PID: %s", device.pid)

    except RuntimeError as e:
        logger.error("Discovery failed: %s", e)
        sys.exit(1)
    except Exception as e:  # pylint: disable=broad-exception-caught
        logger.critical("An unexpected error occurred: %s", e)
        sys.exit(1)


if __name__ == "__main__":
    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
    main()
