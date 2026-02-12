# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""
Maui Serial Transport Module.

This module handles the low-level serial communication with the Maui device,
including device discovery, connection management, and robust command execution
with prompt detection.
"""

import logging
import re

import serial
import serial.tools.list_ports
import usb.core
import usb.util


LOG = logging.getLogger(__name__)

# Maui Device Identification
MAUI_PROTO_FTDI_VID = 0x0403
MAUI_PROTO_FTDI_PID = 0x6002  # Custom PID for Maui Proto
MAUI_PROTO_FTDI_PID_DEFAULT = 0x6001  # Standard FTDI PID fallback
MAUI_FINAL_VID = 0x18D1  # Google VID (placeholder)
MAUI_FINAL_PID = 0x5066  # Maui PID (placeholder)


class MauiSerialTransport:
    """Manages serial communication with a Maui device."""

    def __init__(self, port: str, baudrate: int = 115200, timeout: float = 1.0):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.ser = None

    def connect(self):
        """Establishes a serial connection."""
        try:
            self.ser = serial.Serial(self.port, self.baudrate, timeout=self.timeout)
            LOG.info("Connected to serial port %s", self.port)
            self._synchronize_shell()
        except serial.SerialException as e:
            LOG.error("Failed to connect to %s: %s", self.port, e)
            raise

    def disconnect(self):
        """Closes the serial connection."""
        if self.ser and self.ser.is_open:
            self.ser.close()
            LOG.info("Disconnected from serial port %s", self.port)

    def _read_until_prompt(self) -> str:
        """Reads characters from serial until the shell prompt is encountered,
        returning the content *before* the prompt itself."""
        buffer = b""
        prompt_re = re.compile(b"maui\\$ *")
        chunk_size = 64  # Read in chunks of 64 bytes

        while True:
            chunk = self.ser.read(chunk_size)
            if not chunk:
                if buffer:
                    LOG.warning(
                        "Timeout while waiting for prompt. Received buffer: %r",
                        buffer,
                    )
                else:
                    LOG.warning("Timeout while waiting for prompt. Buffer empty.")
                break
            buffer += chunk

            # Check for prompt anywhere in the buffer
            if prompt_re.search(buffer):
                break

        # Remove the prompt and any content after it
        match = prompt_re.search(buffer)
        if match:
            return buffer[: match.start()].decode(errors="replace")
        
        return buffer.decode(errors="replace")
    def _synchronize_shell(self):
        """Ensures the shell is in a known state (at prompt)."""
        if not self.ser or not self.ser.is_open:
            return

        # Send a newline to get a fresh prompt
        self.ser.write(b"\n")
        # Consume any output until we see a prompt, discarding the returned content
        self._read_until_prompt()
        # Flush any remaining input just in case
        self.ser.reset_input_buffer()

    def send_command(self, command: str) -> str:
        """Sends a command and returns the clean output without echoes or prompts."""
        if not self.ser or not self.ser.is_open:
            raise serial.SerialException("Serial port not connected.")

        LOG.debug("Sending: %s", command)
        self.ser.reset_input_buffer()  # Clear buffer before sending

        self.ser.write(command.encode() + b"\n")

        # _read_until_prompt now returns content *before* the prompt
        raw_response = self._read_until_prompt()

        # Strip ANSI escape codes
        ansi_escape = re.compile(r"\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])")
        clean_response = ansi_escape.sub("", raw_response)

        # Split into lines for filtering
        lines = clean_response.splitlines()

        clean_lines = []
        echo_found = False

        # Filter out the echoed command and any empty lines
        for line in lines:
            stripped_line = line.strip()
            # The echoed command is usually the first line matching the sent command
            if not echo_found and stripped_line == command.strip():
                echo_found = True
                continue  # Skip the echoed command

            if stripped_line:  # Only keep non-empty, non-echoed lines
                clean_lines.append(stripped_line)

        final_response = "\n".join(clean_lines).strip()

        LOG.debug("Received (clean): %r", final_response)
        return final_response


def list_maui_devices():
    """
    Scans for connected Maui devices (both Proto and Final phases).
    Returns a list of dictionaries containing device info.
    """
    devices = []

    # Scan for Proto (FTDI) devices
    for port in serial.tools.list_ports.comports():
        if port.vid == MAUI_PROTO_FTDI_VID and port.pid in (
            MAUI_PROTO_FTDI_PID,
            MAUI_PROTO_FTDI_PID_DEFAULT,
        ):
            devices.append(
                {
                    "type": "proto_ftdi",
                    "port": port.device,
                    "serial_number": port.serial_number,
                    "vid": f"0x{port.vid:04x}",
                    "pid": f"0x{port.pid:04x}",
                    "description": port.description,
                }
            )

    # Scan for Final (Native USB) devices
    # Note: For native USB devices, serial_number might be exposed differently
    # or require specific USB descriptors. This is a generic approach.
    try:
        for dev in usb.core.find(
            find_all=True,
            idVendor=MAUI_FINAL_VID,
            idProduct=MAUI_FINAL_PID,
        ):
            # Attempt to get serial number from iSerialNumber descriptor
            serial_number = None
            if dev.iSerialNumber:
                try:
                    serial_number = usb.util.get_string(dev, dev.iSerialNumber)
                except (usb.core.USBError, ValueError):
                    pass  # Ignore errors if serial cannot be read

            devices.append(
                {
                    "type": "final_native_usb",
                    "port": None,
                    # Native USB might not have a direct COM port mapping initially
                    "serial_number": serial_number,
                    "vid": f"0x{dev.idVendor:04x}",
                    "pid": f"0x{dev.idProduct:04x}",
                    "description": (
                        f"Google Inc. Maui Debug Board (Native USB) "
                        f"{dev.bDeviceClass}"
                    ),
                }
            )
    except usb.core.NoBackendError:
        LOG.warning("libusb backend not found. Native USB discovery might be limited.")
    except usb.core.USBError as e:
        LOG.error("USB error during native USB device discovery: %s", e)

    return devices
