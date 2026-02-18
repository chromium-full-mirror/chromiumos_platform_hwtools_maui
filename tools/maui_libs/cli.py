# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""CLI utilities for Maui tools."""

import argparse
import logging
import sys

import maui_libs


def add_common_args(parser: argparse.ArgumentParser):
    """Adds common arguments to the parser."""
    parser.add_argument(
        "--internal-version",
        action="store_true",
        help="Report the internal software version and exit.",
    )
    parser.add_argument(
        "--serial",
        help="Specify the serial number of the Maui device to connect to.",
    )


def handle_common_args(args: argparse.Namespace):
    """Handles common arguments like --internal-version."""
    if args.internal_version:
        print(maui_libs.__version__)
        sys.exit(0)


def setup_logging():
    """Configures basic logging for CLI tools."""
    logging.basicConfig(level=logging.INFO, format="%(levelname)s: %(message)s")
