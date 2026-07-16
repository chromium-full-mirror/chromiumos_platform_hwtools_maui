# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""
Unit tests for the Maui PDC Firmware Update Module.
"""

import unittest
from unittest.mock import MagicMock

from maui_libs.pdc import MauiPdcClientAdapter


class TestMauiPdcClientAdapter(unittest.TestCase):
    """Tests the MauiPdcClientAdapter's interaction with the device."""

    def setUp(self):
        self.mock_device = MagicMock()
        self.adapter = MauiPdcClientAdapter(self.mock_device)

    def test_fwup_start_success(self):
        """Tests successful start of firmware update session."""
        self.mock_device.send_command_raw.side_effect = [
            "Exclusive logging enabled",
            "TPS_FWUP: Started",
        ]
        self.adapter.fwup_start()
        self.assertEqual(self.mock_device.send_command_raw.call_count, 2)

    def test_fwup_start_failure(self):
        """Tests failure to start firmware update session."""
        self.mock_device.send_command_raw.return_value = "Error: Busy"
        with self.assertRaises(RuntimeError):
            self.adapter.fwup_start()

    def test_fwup_stream_success(self):
        """Tests successful streaming of firmware data."""
        self.mock_device.send_command_raw.return_value = (
            "TPS_FWUP: Stream - bytes written: 64"
        )
        written = self.adapter.fwup_stream(0x100, b"data")
        self.assertEqual(written, 64)

    def test_fwup_stream_parse_fail(self):
        """Tests streaming when bytes written cannot be parsed."""
        self.mock_device.send_command_raw.return_value = "TPS_FWUP: Stream - success"
        written = self.adapter.fwup_stream(0x100, b"12345")
        # Fallback to len(data) if parsing fails
        self.assertEqual(written, 5)

    def test_fwup_complete_success(self):
        """Tests successful completion of firmware update."""
        self.mock_device.send_command_raw.side_effect = [
            "TPS_FWUP: Success",
            "Reset all modules to default log levels",
        ]
        self.adapter.fwup_complete()
        self.assertEqual(self.mock_device.send_command_raw.call_count, 2)

    def test_fwup_complete_failure(self):
        """Tests failure to complete firmware update."""
        self.mock_device.send_command_raw.return_value = "Error: Validation failed"
        with self.assertRaises(RuntimeError):
            self.adapter.fwup_complete()

    def test_fwup_stream_chunking(self):
        """Tests that large data is split into chunks."""
        # Data length 64 bytes. Chunk size is 32. Should be 2 chunks.
        data = b"A" * 64

        # Mock responses.
        # First call returns "bytes written: 32"
        # Second call returns "bytes written: 64" (cumulative)
        self.mock_device.send_command_raw.side_effect = [
            "TPS_FWUP: Stream - bytes written: 32",
            "TPS_FWUP: Stream - bytes written: 64",
        ]

        written = self.adapter.fwup_stream(0x10, data)

        # Verify 2 calls
        self.assertEqual(self.mock_device.send_command_raw.call_count, 2)

        # Verify return value is the last one
        self.assertEqual(written, 64)

    def test_fwup_stream_empty(self):
        """Tests that empty data sends at least one command (for status)."""
        data = b""
        self.mock_device.send_command_raw.return_value = (
            "TPS_FWUP: Stream - bytes written: 100"
        )

        written = self.adapter.fwup_stream(0x10, data)

        # Verify 1 call (even for empty data)
        self.assertEqual(self.mock_device.send_command_raw.call_count, 1)
        self.assertEqual(written, 100)


if __name__ == "__main__":
    unittest.main()
