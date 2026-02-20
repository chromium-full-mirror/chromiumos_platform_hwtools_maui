# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""
Maui PDC Firmware Update Module (TPS6699x).

This module handles the firmware update of the Power Delivery Controller (PDC)
via the Maui shell console using the TI TFU Bundle format.
"""

import base64
import logging
import re
import struct
import time
from typing import TYPE_CHECKING


if TYPE_CHECKING:
    from maui_libs.device import MauiDevice

logger = logging.getLogger(__name__)

# TPS6699x Constants
CMD_START = "pdc_tps_fwup start"
CMD_SEND_INITIATE = "pdc_tps_fwup send_initiate"
CMD_SEND_BLOCK = "pdc_tps_fwup send_block"
CMD_STREAM = "pdc_tps_fwup stream"
CMD_COMPLETE = "pdc_tps_fwup complete"
CMD_ABORT = "pdc_tps_fwup abort"
CMD_LOG_EXCLUSIVE = "log_exclusive tps6699x_fwup"
CMD_LOG_RESET = "log_exclusive reset"


class MauiPdcClientAdapter:
    """
    Adapts a MauiDevice to the interface required by Tps6699xFirmwareUpdater.
    """

    def __init__(self, device: "MauiDevice"):
        self.device = device

    def _check_response(self, response: str, expected_msg: str = None) -> bool:
        if "error" in response.lower() or "failed" in response.lower():
            logger.error("Command failed. Output: %s", response)
            raise RuntimeError("Command failed")
        if expected_msg and expected_msg not in response:
            logger.error(
                "Expected '%s' not found in response. Response: %s",
                expected_msg,
                response,
            )
            raise RuntimeError(f"Expected '{expected_msg}' not found")
        return True

    def fwup_start(self):
        """Start a FW update session"""
        # Ensure Maui is not printing any other logs to terminal
        resp = self.device.send_command_raw(CMD_LOG_EXCLUSIVE, timeout=5.0)
        self._check_response(resp, "Exclusive logging enabled")
        resp = self.device.send_command_raw(CMD_START, timeout=5.0)
        self._check_response(resp, "TPS_FWUP: Started")

    def fwup_initiate(self, data: bytes):
        """Send initiate command via TFU"""
        b64_data = base64.b64encode(data).decode("ascii")
        cmd = f"{CMD_SEND_INITIATE} {b64_data}"
        resp = self.device.send_command_raw(cmd)
        self._check_response(resp, "TPS_FWUP: Send Initiate complete")

    def fwup_block(self, data: bytes):
        """Send block command via TFU"""
        b64_data = base64.b64encode(data).decode("ascii")
        cmd = f"{CMD_SEND_BLOCK} {b64_data}"
        resp = self.device.send_command_raw(cmd)
        self._check_response(resp, "TPS_FWUP: Send Block complete")

    def fwup_stream(self, broadcast_address: int, data: bytes) -> int:
        """Transfer FW payload data to the EC via the console"""
        # Split into smaller chunks to prevent shell buffer overflow
        chunk_size = 32
        last_written = 0

        # Handle empty data (send at least one command to get status)
        # or split large data into chunks
        chunks = (
            [b""]
            if len(data) == 0
            else [data[i : i + chunk_size] for i in range(0, len(data), chunk_size)]
        )

        for chunk in chunks:
            broadcast_bytes = struct.pack("<H", broadcast_address)
            buf = broadcast_bytes + chunk
            b64_data = base64.b64encode(buf).decode("ascii")

            cmd = f"{CMD_STREAM} {b64_data}"
            resp = self.device.send_command_raw(cmd)
            self._check_response(resp)

            # Parse "TPS_FWUP: Stream - bytes written: <N>"
            match = re.search(r"TPS_FWUP: Stream - bytes written: (\d+)", resp)
            if match:
                last_written = int(match.group(1))
            else:
                logger.warning(
                    "Could not parse bytes written from response. Resp: %r", resp
                )
                last_written += len(chunk)

        return last_written

    def fwup_complete(self):
        """Finalize the update after all data is transferred"""
        # Extend the UART timeout because the validation and restart delay
        # steps take a long time (~7s + 5s)
        resp = self.device.send_command_raw(CMD_COMPLETE, timeout=16.0)
        self._check_response(resp, "TPS_FWUP: Success")
        # Reset Maui logging
        resp = self.device.send_command_raw(CMD_LOG_RESET, timeout=5.0)
        self._check_response(resp, "Reset all modules to default log levels")

    def fwup_abort(self):
        """Try to recover the PDC subsystem after a failed update procedure"""
        self.device.send_command_raw(CMD_ABORT)
        # Reset Maui logging
        resp = self.device.send_command_raw(CMD_LOG_RESET, timeout=5.0)
        self._check_response(resp, "Reset all modules to default log levels")


# Disable all the violations for keeping sync with sources
# pylint: disable=all
# Source: https://chromium.googlesource.com/chromiumos/platform/pdc/+/refs/heads/main/scripts/pdclib/console_fwup_tps.py
class TfuiData:
    """Unpacked data for TFUi"""

    def __init__(self, data: bytes):
        (
            self.num_blocks,
            self.data_block_size,
            self.timeout_secs,
            self.broadcast_address,
        ) = struct.unpack("<HHHH", data)


class TfudData:
    """Unpacked data for TFUd"""

    def __init__(self, data: bytes):
        (
            self.block_num,
            self.data_block_size,
            self.timeout_secs,
            self.broadcast_address,
        ) = struct.unpack("<HHHH", data)


class Tps6699xFirmwareUpdater:
    """Firmware updater for TPS6699x PDC."""

    MAX_READ_CHUNK_SIZE = 0x4000

    # TFUI file constants
    TFUI_METADATA_OFFSET = 0x4
    TFUI_METADATA_LENGTH = 0x8
    TFUI_STREAM_HEADER_BLOCK_OFFSET = 0xC
    TFUI_STREAM_HEADER_BLOCK_LENGTH = 0x800

    # TFUD file constants
    TFUD_REGION_OFFSET = 0x80C
    TFUD_BLOCK_SIZE = 0x4000
    TFUD_METADATA_LENGTH = 0x8
    TFUD_CHUNK_SIZE = 64
    TFUD_MAX_NUM_BLOCKS = 12

    # Size of fw not including appconfig and header block is at this offset. \*/
    FW_SIZE_OFFSET = 0x4F8

    # Streaming packets will be the broadcast byte + 64 bytes of string data
    HOST_STREAM_PACKET_FMT = "<B64s"
    STREAM_SIZE = 64

    def __init__(self, servo):
        self.servo = servo

    def _TFUD_METADATA_OFFSET_AT(self, block: int) -> int:
        return (
            (self.TFUD_BLOCK_SIZE + self.TFUD_METADATA_LENGTH) * block
        ) + self.TFUD_REGION_OFFSET

    def _TFUD_DATA_AT(self, block: int) -> int:
        return self._TFUD_METADATA_OFFSET_AT(block) + self.TFUD_METADATA_LENGTH

    def _APPCONFIG_DATA(self, fw_size: int, num_data_blocks: int) -> (int, int):
        # The Application Configuration is stored at the following offset:
        # FirmwareImageSize (Which excludes Header and App Config)
        # + 0x800 (Header Block Size)
        # + (8 (Meta Data for Each Block + header) * Number of Data block + 1)
        # + 4 (File Identifier)
        metadata_offset = (
            fw_size
            + self.TFUI_STREAM_HEADER_BLOCK_LENGTH
            + (self.TFUD_METADATA_LENGTH * (num_data_blocks + 1))
            + 4
        )

        # Appconfig block uses same metadata block as TFUd so data is after the
        # metadata.
        data_block_offset = metadata_offset + self.TFUD_METADATA_LENGTH

        return (metadata_offset, data_block_offset)

    def _read_data(self, data: bytes, offset: int, length: int) -> bytes:
        return data[offset : offset + length]

    def update(self, fw_image: bytes):
        """Update firmware with the supplied PDC FW binary."""

        def chunk_stream(stream_for, broadcast_address, buf):
            """Chunk a single stream into multiple calls to the EC."""
            buf_len = len(buf)
            count = (buf_len // self.STREAM_SIZE) + 1
            ec_bytes_received = 0
            bytes_written = 0
            progress_log_target = 4000

            for _ in range(count):
                (start, end) = (
                    bytes_written,
                    min(bytes_written + self.STREAM_SIZE, buf_len),
                )
                # Send to console
                ec_bytes_received = self.servo.fwup_stream(
                    broadcast_address, buf[start:end]
                )

                assert ec_bytes_received == end, f"{ec_bytes_received=} vs {end=}"

                bytes_written = end

                # Show progress info every 4KB transferred
                if ec_bytes_received > progress_log_target:
                    logger.info(
                        "%s: Progess: %d/%d bytes transferred (%.2f%%)",
                        stream_for,
                        progress_log_target,
                        buf_len,
                        100.0 * progress_log_target / buf_len,
                    )
                    progress_log_target += 4000

        def do_tfud_block(msg_prefix, metadata_offset, data_offset):
            """Handle a single TFUd block + stream."""
            tfud_buf = self._read_data(
                fw_image, metadata_offset, self.TFUD_METADATA_LENGTH
            )
            tfud = TfudData(tfud_buf)
            stream_data = self._read_data(fw_image, data_offset, tfud.data_block_size)
            self.servo.fwup_block(tfud_buf)
            chunk_stream(msg_prefix, tfud.broadcast_address, stream_data)

        try:
            # Start firmware update session
            logger.info("Starting firmware update session")
            self.servo.fwup_start()

            logger.info("Sending data blocks")
            # Send TFUi command and then stream the header
            tfui_buf = self._read_data(
                fw_image, self.TFUI_METADATA_OFFSET, self.TFUI_METADATA_LENGTH
            )
            tfui_header = self._read_data(
                fw_image,
                self.TFUI_STREAM_HEADER_BLOCK_OFFSET,
                self.TFUI_STREAM_HEADER_BLOCK_LENGTH,
            )
            tfui = TfuiData(tfui_buf)
            self.servo.fwup_initiate(tfui_buf)
            chunk_stream("FW header", tfui.broadcast_address, tfui_header)

            # TFUd will enumerate blocks
            for block in range(tfui.num_blocks):
                do_tfud_block(
                    f"Block{block}",
                    self._TFUD_METADATA_OFFSET_AT(block),
                    self._TFUD_DATA_AT(block),
                )

            logger.info("Sending appconfig")
            # Last, do the appconfig region.
            fw_size_buf = self._read_data(fw_image, self.FW_SIZE_OFFSET, 4)
            (fw_size,) = struct.unpack("<I", fw_size_buf)

            (metadata_offset, stream_data_offset) = self._APPCONFIG_DATA(
                fw_size, tfui.num_blocks
            )
            do_tfud_block("Appconfig", metadata_offset, stream_data_offset)

            # Complete firmware update session
            logger.info("Finalizing firmware update. This may take a moment.")
            self.servo.fwup_complete()
            logger.info("✅ Update succeeded")
        except Exception:
            # In case of failure, try to recover the PDC subsystem
            logger.info("❌ Update failed. Attempting to recover PDC.")

            self.servo.fwup_abort()
            raise


# pylint: enable=all
