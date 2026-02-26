# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""
Maui Device Module.

This module provides the high-level API for interacting with the Maui device.
It abstracts the underlying transport and provides methods for discovery and
command execution.
"""

import logging
from typing import Optional

from maui_libs.transport import list_maui_devices
from maui_libs.transport import MauiSerialTransport


logger = logging.getLogger(__name__)


class MauiDevice:
    """Represents a connected Maui device and its capabilities."""

    def __init__(
        self,
        port: str,
        serial_number: str,
        device_type: str,
        vid: str,
        pid: str,
    ):  # pylint: disable=too-many-arguments,too-many-positional-arguments
        self.port = port
        self.serial_number = serial_number
        self.device_type = device_type
        self.vid = vid
        self.pid = pid
        # Only create transport if port is available
        if self.port:
            self._transport = MauiSerialTransport(port)
            self.is_connected = False
        else:
            self._transport = None
            self.is_connected = False
            logger.warning(
                "MauiDevice initialized without a port. "
                "Connection methods will not work."
            )

    def connect(self):
        """Connects to the device's serial transport."""
        if not self._transport:
            raise NotImplementedError(
                "Can't connect to this device type (no serial port available)."
            )
        if not self.is_connected:
            self._transport.connect()
            self.is_connected = True

    def disconnect(self):
        """Disconnects from the device's serial transport."""
        if self._transport and self.is_connected:
            self._transport.disconnect()
            self.is_connected = False

    def send_command(self, command: str) -> str:
        """Sends a command to the Maui device and returns the response."""
        self.connect()  # Ensure connection before sending
        return self._transport.send_command(command)

    def send_command_raw(self, command: str, timeout: float = None) -> str:
        """
        Sends a command to the Maui device and returns the raw response.
        Useful for firmware update scripts that parse output themselves.
        An optional `timeout` can be provided for this specific command.
        """
        self.connect()
        return self._transport.send_command_raw(command, timeout=timeout)

    def set_power(self, state: str) -> str:
        """
        Controls power to the DUT.
        state: 'on', 'off', or 'cycle'
        """
        if state not in ["on", "off", "cycle"]:
            raise ValueError("Invalid power state. Use 'on', 'off', or 'cycle'.")
        return self.send_command(f"power_dut {state}")

    def set_data(self, state: str) -> str:
        """
        Controls USB data lines to the DUT.
        state: 'on', 'off', or 'cycle'
        """
        if state not in ["on", "off", "cycle"]:
            raise ValueError("Invalid data state. Use 'on', 'off', or 'cycle'.")

        return self.send_command(f"data_dut {state}")

    @classmethod
    def find_device(cls, serial_number: Optional[str] = None) -> "MauiDevice":
        """
        Discovers and returns a MauiDevice instance.
        If serial_number is None, it expects exactly one device.
        If multiple devices are found without a serial_number,
        it lists them and exits.
        """
        available_devices = list_maui_devices()

        if not available_devices:
            logger.error(
                "No Maui devices found."
                " Ensure it is connected and recognized by the system."
            )
            raise RuntimeError(
                "No Maui devices found."
                " Ensure it is connected and recognized by the system."
            )

        if serial_number:
            # Try to find the specific device
            for dev_info in available_devices:
                if dev_info.get("serial_number") == serial_number:
                    if not dev_info.get("port"):
                        raise NotImplementedError(
                            f"Device {serial_number} is a native USB device "
                            "and does not currently support direct serial"
                            " communication."
                        )
                    logger.info(
                        "Found specified Maui device: %s on %s",
                        serial_number,
                        dev_info["port"],
                    )
                    return cls(
                        port=dev_info["port"],
                        serial_number=dev_info["serial_number"],
                        device_type=dev_info["type"],
                        vid=dev_info["vid"],
                        pid=dev_info["pid"],
                    )
            logger.error("Maui device with serial %s not found.", serial_number)
            raise RuntimeError(
                f"Device with serial number '{serial_number}' not found."
                " Please check the serial number and try again."
            )

        # Handle multiple devices without a serial number specified
        if len(available_devices) > 1:
            logger.error(
                "Multiple Maui devices detected. Please specify a serial "
                "number using --serial to select your target device."
            )
            logger.info("Available devices:")
            for dev_info in available_devices:
                logger.info(
                    "  Serial: %s, Type: %s, Port: %s, VID: %s, PID: %s",
                    dev_info.get("serial_number", "No Serial Number"),
                    dev_info["type"],
                    dev_info.get("port", "N/A"),
                    dev_info["vid"],
                    dev_info["pid"],
                )
            raise RuntimeError(
                "Multiple Maui devices detected."
                " Use --serial to specify your target device."
            )

        # Exactly one device found, no serial specified
        dev_info = available_devices[0]
        if not dev_info.get("port"):
            raise NotImplementedError(
                f"Found a native USB device "
                f"(Serial: {dev_info.get("serial_number","No Serial Number")}) "
                "which does not currently support direct serial communication."
            )
        logger.info(
            "Found single Maui device: %s on %s",
            dev_info.get("serial_number", "No Serial Number"),
            dev_info["port"],
        )
        return cls(
            port=dev_info["port"],
            serial_number=dev_info.get("serial_number", "No Serial Number"),
            device_type=dev_info["type"],
            vid=dev_info["vid"],
            pid=dev_info["pid"],
        )
