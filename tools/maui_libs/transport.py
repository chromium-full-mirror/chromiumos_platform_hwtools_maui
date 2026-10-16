# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""
Maui Serial Transport Module.

This module handles the low-level serial communication with the Maui device,
including device discovery, connection management, and robust command execution
with prompt detection.
It supports both Proto (FTDI) and Final (Native USB CDC ACM) phases.
"""

import logging
import re

import serial
import serial.tools.list_ports


logger = logging.getLogger(__name__)

# Maui Hardware Device Types
DEVICE_TYPE_PROTO = "maui_proto"
DEVICE_TYPE_V11 = "maui_v11"
DEVICE_TYPE_V12 = "maui_v12"
DEVICE_TYPE_V20 = "maui_v20"

VALID_DEVICE_TYPES = (
    DEVICE_TYPE_PROTO,
    DEVICE_TYPE_V11,
    DEVICE_TYPE_V12,
    DEVICE_TYPE_V20,
)
MSPM0_DEVICE_TYPES = (
    DEVICE_TYPE_PROTO,
    DEVICE_TYPE_V11,
)
ITE_DEVICE_TYPES = (
    DEVICE_TYPE_V12,
    DEVICE_TYPE_V20,
)

# Serial number prefixes for hardware classification
MAUI_V11_SERIAL_PREFIX = "MAUIV11L"
MAUI_V20_SERIAL_PREFIX = "MAUIV20"

# Maui USB Identification
MAUI_PROTO_FTDI_VID = 0x0403
MAUI_PROTO_FTDI_PID = 0x6002  # Custom PID for Maui Proto / v1.1
MAUI_PROTO_FTDI_PID_DEFAULT = 0x6001  # Standard FTDI PID fallback

# Native USB Phase (Zephyr CDC ACM - ITE IT82002)
MAUI_FINAL_VID = 0x18D1  # Google VID
MAUI_FINAL_PID = 0x5085  # Maui Main / Bootloader PID
MAUI_IT82002_APP_PID = 0x5066  # Maui IT82002 Legacy App PID
MAUI_IT82002_BOOTLOADER_PID = 0x5067  # Maui IT82002 Legacy MCUboot Recovery PID

# USB interface string of the IT82002 App's RPC CDC-ACM port.
MAUI_RPC_INTERFACE_NAME = "Maui RPC"


def classify_ftdi_device(serial_number: str = None) -> str:
    """Classifies an FTDI-connected Maui device into maui_v11 or maui_proto.

    v1.1 boards have a serial number starting with 'MAUIV11L'; all other FTDI
    boards are classified as 'maui_proto'.
    """
    if serial_number and serial_number.upper().startswith(MAUI_V11_SERIAL_PREFIX):
        return DEVICE_TYPE_V11
    return DEVICE_TYPE_PROTO


def classify_ite_device(serial_number: str = None, product: str = None) -> str:
    """Classifies a native-USB ITE Maui device into maui_v12 or maui_v20."""
    if serial_number and serial_number.upper().startswith(MAUI_V20_SERIAL_PREFIX):
        return DEVICE_TYPE_V20
    if product and "V20" in product.upper():
        return DEVICE_TYPE_V20
    return DEVICE_TYPE_V12


class MauiSerialTransport:
    """Manages serial communication with a Maui device."""

    def __init__(self, port: str, baudrate: int = 115200, timeout: float = 3.0):
        self.port = port
        self.baudrate = baudrate
        self.timeout = timeout
        self.ser = None

    def connect(self):
        """Establishes a serial connection."""
        try:
            self.ser = serial.Serial(
                self.port,
                self.baudrate,
                timeout=self.timeout,
                xonxoff=False,
                rtscts=False,
                dsrdtr=False,
            )
            self.ser.reset_input_buffer()
            self.ser.reset_output_buffer()
            logger.info("Connected to serial port %s", self.port)
            self._synchronize_shell()
        except serial.SerialException as e:
            logger.error("Failed to connect to %s: %s", self.port, e)
            raise

    def disconnect(self):
        """Closes the serial connection."""
        if self.ser and self.ser.is_open:
            self.ser.close()
            logger.info("Disconnected from serial port %s", self.port)

    def _read_until_prompt(self, expected_cmd: str = None) -> bytes:
        """Reads bytes from serial until the shell prompt is encountered.

        If expected_cmd is specified, ensures that the buffer contains expected_cmd
        before accepting a trailing shell prompt.
        """
        buffer = b""
        # Regex to match 'maui$' at the end,
        # allowing for trailing spaces and ANSI codes (e.g. color reset)
        prompt_re = re.compile(b"maui\\$(?:\\s|\\x1b\\[[0-9;]*m)*$")
        cmd_token = expected_cmd.strip().split()[0].encode() if expected_cmd else None

        while True:
            # Read available bytes, or block for at least 1 byte if empty
            n = self.ser.in_waiting or 1
            chunk = self.ser.read(n)

            if not chunk:
                if buffer:
                    logger.warning(
                        "Timeout while waiting for prompt. Received buffer: %r",
                        buffer,
                    )
                else:
                    logger.warning("Timeout while waiting for prompt. Buffer empty.")
                break
            buffer += chunk

            # Check for prompt at the end of the buffer
            if prompt_re.search(buffer):
                if cmd_token is None or cmd_token in buffer:
                    break

        return buffer

    def _flush_input(self):
        """Discards all data in the input buffer and logs if anything was found."""
        if not self.ser or not self.ser.is_open:
            return
        try:
            in_waiting = self.ser.in_waiting
            if in_waiting > 0:
                flushed = self.ser.read(in_waiting)
                logger.debug("Flushed leftover: %r", flushed)
            self.ser.reset_input_buffer()
        except (serial.SerialException, OSError) as e:
            logger.warning("Failed to flush input: %s", e)

    def _synchronize_shell(self):
        """Ensures the shell is in a known state (at prompt) by waiting for quiet."""
        if not self.ser or not self.ser.is_open:
            return

        logger.info("Synchronizing shell (waiting for quiet)...")
        # Send a newline to get a fresh prompt
        self.ser.write(b"\r")
        # Consume any output until we see a prompt, discarding the returned content
        self._read_until_prompt()

        # Wait for asynchronous logs to quiet down
        orig_timeout = self.ser.timeout
        quiet_timeout = 0.3  # 300ms of silence required
        self.ser.timeout = quiet_timeout
        ansi_escape = re.compile(b"\x1b(?:[@-Z\\\\-_]|\\[[0-?]*[ -/]*[@-~])")
        try:
            retries = 0
            max_retries = 5
            while retries < max_retries:
                data = self.ser.read(1024)
                if not data:
                    break

                # Clean data to see if it contains real content
                clean_data = ansi_escape.sub(b"", data).strip()
                if not clean_data:
                    logger.debug("Discarded trailing prompt characters: %r", data)
                    continue

                logger.debug("Discarded async log during sync: %r", data)
                # If we got real data, it might have corrupted the prompt state,
                # so request a new prompt and wait for it.
                self.ser.write(b"\r")
                self.ser.timeout = orig_timeout
                self._read_until_prompt()
                self.ser.timeout = quiet_timeout
                retries += 1
            if retries >= max_retries:
                logger.warning(
                    "Shell synchronization: reached max retries waiting for "
                    "quiet. Proceeding anyway."
                )
        finally:
            self.ser.timeout = orig_timeout

        self._flush_input()

    def send_command(self, command: str) -> str:
        """Sends a command and returns the clean output without echoes or prompts."""
        if not self.ser or not self.ser.is_open:
            raise serial.SerialException("Serial port not connected.")

        logger.debug("Sending: %s", command)
        self._flush_input()  # Clear buffer before sending

        self.ser.write(command.encode() + b"\r")
        # Ensure command is sent before waiting for response
        self.ser.flush()

        # _read_until_prompt returns raw bytes including prompt
        raw_bytes = self._read_until_prompt(expected_cmd=command)
        raw_response = raw_bytes.decode(errors="replace")

        # Strip ANSI escape codes
        ansi_escape = re.compile(r"\x1B(?:[@-Z\\-_]|\[[0-?]*[ -/]*[@-~])")
        clean_response = ansi_escape.sub("", raw_response)

        # Remove prompt from the end (more robust than line-by-line)
        # We look for "maui$" with optional trailing space/newline at the end
        prompt_re_str = re.compile(r"maui\$\s*[\r\n]*$")
        clean_response = prompt_re_str.sub("", clean_response)

        # Split into lines for filtering
        lines = clean_response.splitlines()

        clean_lines = []
        echo_found = False
        cmd_strip = command.strip()

        # Filter out any asynchronous lines before the echoed command,
        # the echoed command itself, and any trailing empty lines.
        for line in lines:
            stripped_line = line.strip()

            if not echo_found:
                if stripped_line == cmd_strip or stripped_line.startswith(cmd_strip):
                    echo_found = True
                continue

            if stripped_line:  # Only keep non-empty output lines
                clean_lines.append(stripped_line)

        final_response = "\n".join(clean_lines).strip()

        logger.debug("Received (clean): %r", final_response)
        return final_response

    def send_command_raw(self, command: str, timeout: float = None) -> str:
        """Sends a command and returns the raw output without any cleaning.

        Useful for scripts that need to parse specific output formats or debug.
        An optional `timeout` can be provided to override the default for this command.
        """
        if not self.ser or not self.ser.is_open:
            raise serial.SerialException("Serial port not connected.")

        logger.debug("Sending (raw): %s", command)
        self.ser.reset_input_buffer()

        original_timeout = self.ser.timeout
        if timeout is not None:
            self.ser.timeout = timeout

        self._flush_input()  # Clear buffer before sending
        try:
            self.ser.write(command.encode() + b"\r")
            self.ser.flush()
            raw_bytes = self._read_until_prompt(expected_cmd=command)
            response = raw_bytes.decode(errors="replace")

            # Slice output from the echoed command
            # onwards to ignore preceding async logs
            cmd_strip = command.strip()
            idx = response.find(cmd_strip)
            if idx != -1:
                response = response[idx:]
        finally:
            # Restore original timeout
            if timeout is not None:
                self.ser.timeout = original_timeout

        return response


def list_maui_devices():
    """Scans for connected Maui devices across all hardware generations.

    Classifies devices into:
      * 'maui_v11': FTDI USB-to-UART with serial starting with 'MAUIV11L'
      * 'maui_proto': FTDI USB-to-UART with any other serial number
      * 'maui_v20': Native USB ITE device with serial starting with 'MAUIV20'
      * 'maui_v12': Native USB ITE device (all other serials)

    Returns a list of dictionaries containing device info.
    """
    devices = []

    # Use serial port enumeration for both environments since Zephyr provides CDC ACM
    for port in serial.tools.list_ports.comports():
        # Check for FTDI (maui_proto and maui_v11) devices
        if port.vid == MAUI_PROTO_FTDI_VID and port.pid in (
            MAUI_PROTO_FTDI_PID,
            MAUI_PROTO_FTDI_PID_DEFAULT,
        ):
            dev_type = classify_ftdi_device(port.serial_number)
            devices.append(
                {
                    "type": dev_type,
                    "is_bootloader": False,
                    "port": port.device,
                    "serial_number": port.serial_number,
                    "vid": f"0x{port.vid:04x}",
                    "pid": f"0x{port.pid:04x}",
                    "description": port.description,
                }
            )
        # Check for ITE Native USB CDC ACM devices (maui_v12 and maui_v20)
        # (App or Bootloader sharing 0x18D1:0x5085, or legacy 0x5066/0x5067).
        # The App's second interface is the machine RPC channel (maui_libs.rpc),
        # not a shell.
        elif (
            port.vid == MAUI_FINAL_VID
            and port.pid
            in (
                MAUI_FINAL_PID,
                MAUI_IT82002_APP_PID,
                MAUI_IT82002_BOOTLOADER_PID,
            )
            and (port.interface or "") != MAUI_RPC_INTERFACE_NAME
        ):
            is_bootloader = (
                port.pid == MAUI_IT82002_BOOTLOADER_PID
                or "BOOTLOADER" in (port.product or "").upper()
            )
            dev_type = classify_ite_device(port.serial_number, port.product)
            devices.append(
                {
                    "type": dev_type,
                    "is_bootloader": is_bootloader,
                    "port": port.device,
                    "serial_number": port.serial_number,
                    "vid": f"0x{port.vid:04x}",
                    "pid": f"0x{port.pid:04x}",
                    "description": port.product
                    or port.description
                    or "Google Inc. Maui Debug Board (Native USB)",
                }
            )

    return devices
