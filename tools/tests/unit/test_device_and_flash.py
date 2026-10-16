# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Unit tests for Maui device classification and GCS firmware resolution."""

from types import SimpleNamespace
import unittest
from unittest.mock import patch

from maui_libs.device import MauiDevice
from maui_libs.transport import classify_ftdi_device
from maui_libs.transport import classify_ite_device
from maui_libs.transport import DEVICE_TYPE_PROTO
from maui_libs.transport import DEVICE_TYPE_V11
from maui_libs.transport import DEVICE_TYPE_V12
from maui_libs.transport import DEVICE_TYPE_V20
from maui_libs.transport import list_maui_devices
from maui_libs.transport import MAUI_FINAL_PID
from maui_libs.transport import MAUI_FINAL_VID
from maui_libs.transport import MAUI_PROTO_FTDI_PID
from maui_libs.transport import MAUI_PROTO_FTDI_VID
from maui_libs.transport import MAUI_RPC_INTERFACE_NAME

from src import maui_flash


def _make_port(
    device="/dev/ttyUSB0",
    vid=MAUI_PROTO_FTDI_VID,
    pid=MAUI_PROTO_FTDI_PID,
    serial_number="PROTO001",
    product="Maui",
    description="Maui",
    interface=None,
):  # pylint: disable=too-many-arguments,too-many-positional-arguments
    return SimpleNamespace(
        device=device,
        vid=vid,
        pid=pid,
        serial_number=serial_number,
        product=product,
        description=description,
        interface=interface,
    )


class TestDeviceClassification(unittest.TestCase):
    """Tests FTDI and ITE USB device classification rules."""

    def test_classify_ftdi_serial_prefix(self):
        """FTDI serials starting with MAUIV11L are maui_v11; others are maui_proto."""
        self.assertEqual(classify_ftdi_device("MAUIV11L0001"), DEVICE_TYPE_V11)
        self.assertEqual(classify_ftdi_device("mauiv11l9999"), DEVICE_TYPE_V11)
        self.assertEqual(classify_ftdi_device("MAUIPROTO1"), DEVICE_TYPE_PROTO)
        self.assertEqual(classify_ftdi_device("FT123456"), DEVICE_TYPE_PROTO)
        self.assertEqual(classify_ftdi_device(None), DEVICE_TYPE_PROTO)
        self.assertEqual(classify_ftdi_device(""), DEVICE_TYPE_PROTO)

    def test_classify_ite_device(self):
        """ITE native USB devices default to maui_v12, or maui_v20 with prefix."""
        self.assertEqual(classify_ite_device("MAUIV12L0001"), DEVICE_TYPE_V12)
        self.assertEqual(classify_ite_device("Uninitialized"), DEVICE_TYPE_V12)
        self.assertEqual(classify_ite_device(None), DEVICE_TYPE_V12)
        self.assertEqual(classify_ite_device("MAUIV20L0001"), DEVICE_TYPE_V20)
        self.assertEqual(
            classify_ite_device("Uninitialized", "Maui V20 Controller"),
            DEVICE_TYPE_V20,
        )

    @patch("serial.tools.list_ports.comports")
    def test_list_maui_devices_all_variants(self, mock_comports):
        """Verifies list_maui_devices across proto, v11, v12, v20, and RPC."""
        mock_comports.return_value = [
            _make_port(
                device="/dev/ttyUSB0",
                serial_number="FT_PROTO_01",
            ),
            _make_port(
                device="/dev/ttyUSB1",
                serial_number="MAUIV11L0042",
            ),
            _make_port(
                device="/dev/ttyACM0",
                vid=MAUI_FINAL_VID,
                pid=MAUI_FINAL_PID,
                serial_number="MAUIV12L0005",
                product="Maui Diagnostic Controller",
                interface="Maui Shell",
            ),
            _make_port(
                device="/dev/ttyACM1",
                vid=MAUI_FINAL_VID,
                pid=MAUI_FINAL_PID,
                serial_number="MAUIV12L0005",
                product="Maui Diagnostic Controller",
                interface=MAUI_RPC_INTERFACE_NAME,
            ),
            _make_port(
                device="/dev/ttyACM2",
                vid=MAUI_FINAL_VID,
                pid=MAUI_FINAL_PID,
                serial_number="MAUIV12L0009",
                product="Maui RW Bootloader",
            ),
            _make_port(
                device="/dev/ttyACM3",
                vid=MAUI_FINAL_VID,
                pid=MAUI_FINAL_PID,
                serial_number="MAUIV20L0001",
                product="Maui Diagnostic Controller",
            ),
        ]

        devices = list_maui_devices()
        self.assertEqual(len(devices), 5)

        self.assertEqual(devices[0]["type"], DEVICE_TYPE_PROTO)
        self.assertFalse(devices[0]["is_bootloader"])

        self.assertEqual(devices[1]["type"], DEVICE_TYPE_V11)
        self.assertFalse(devices[1]["is_bootloader"])

        self.assertEqual(devices[2]["type"], DEVICE_TYPE_V12)
        self.assertFalse(devices[2]["is_bootloader"])
        self.assertEqual(devices[2]["port"], "/dev/ttyACM0")

        self.assertEqual(devices[3]["type"], DEVICE_TYPE_V12)
        self.assertTrue(devices[3]["is_bootloader"])
        self.assertEqual(devices[3]["port"], "/dev/ttyACM2")

        self.assertEqual(devices[4]["type"], DEVICE_TYPE_V20)
        self.assertFalse(devices[4]["is_bootloader"])
        self.assertEqual(devices[4]["port"], "/dev/ttyACM3")

    @patch("maui_libs.device.list_maui_devices")
    def test_find_device_is_ite_property(self, mock_list):
        """Verifies MauiDevice.is_ite is True for v12/v20 and False for proto/v11."""
        mock_list.return_value = [
            {
                "type": DEVICE_TYPE_V11,
                "is_bootloader": False,
                "port": "/dev/ttyUSB0",
                "serial_number": "MAUIV11L0001",
                "vid": "0x0403",
                "pid": "0x6002",
            }
        ]
        dev_v11 = MauiDevice.find_device()
        self.assertEqual(dev_v11.device_type, DEVICE_TYPE_V11)
        self.assertFalse(dev_v11.is_ite)

        mock_list.return_value = [
            {
                "type": DEVICE_TYPE_V12,
                "is_bootloader": False,
                "port": "/dev/ttyACM0",
                "serial_number": "MAUIV12L0001",
                "vid": "0x18d1",
                "pid": "0x5085",
            }
        ]
        dev_v12 = MauiDevice.find_device()
        self.assertEqual(dev_v12.device_type, DEVICE_TYPE_V12)
        self.assertTrue(dev_v12.is_ite)


class TestGcsFirmwareResolution(unittest.TestCase):
    """Tests GCS tag and binary resolution across maui_proto, v11, v12, and v20."""

    MOCK_GCS_KEYS = [
        # MCU MSPM0 (shared by maui_proto and maui_v11)
        "mcu-fw/mspm0/0.60-042f858/zephyr.txt",
        "mcu-fw/mspm0/0.60-042f858/stable.maui_proto.mcufw",
        "mcu-fw/mspm0/0.60-042f858/stable.maui_v11.mcufw",
        # MCU ITE v1.2 (maui_v12)
        "mcu-fw/maui_v12/0.60-52742e3/maui_app_signed.bin",
        "mcu-fw/maui_v12/0.60-52742e3/maui_rw_bootloader_signed.bin",
        "mcu-fw/maui_v12/0.60-52742e3/maui_full_flash_1mb.bin",
        "mcu-fw/maui_v12/0.60-52742e3/stable.maui_v12.mcufw",
        # MCU ITE v2.0 (maui_v20)
        "mcu-fw/maui_v20/0.61-aaaa111/maui_app_signed.bin",
        "mcu-fw/maui_v20/0.61-aaaa111/stable.maui_v20.mcufw",
        # PDC v20 (maui_proto)
        "pdc-fw/v20_0325/v20_0325.bin",
        "pdc-fw/v20_0325/stable.maui_proto.pdcfw",
        "pdc-fw/v20_0325/stable.pdcfw",
        # PDC v20_100W (shared by maui_v11 and maui_v12)
        "pdc-fw/v20_0325_100W/v20_0325_100W.bin",
        "pdc-fw/v20_0325_100W/stable.maui_v11.pdcfw",
        "pdc-fw/v20_0325_100W/stable.maui_v12.pdcfw",
    ]

    def _mock_list_keys(self, prefix: str):
        return [k for k in self.MOCK_GCS_KEYS if k.startswith(prefix)]

    @patch("src.maui_flash._list_gcs_keys")
    def test_pdc_resolution_per_device(self, mock_list_keys):
        """Verifies maui_proto gets v20_0325 while v11 and v12 get v20_0325_100W."""
        mock_list_keys.side_effect = self._mock_list_keys

        proto_dir = maui_flash._resolve_tag_to_dir(  # pylint: disable=protected-access
            "stable", "pdc", "pdc-fw", ".pdcfw", device_type=DEVICE_TYPE_PROTO
        )
        self.assertEqual(proto_dir, "pdc-fw/v20_0325")

        v11_dir = maui_flash._resolve_tag_to_dir(  # pylint: disable=protected-access
            "stable", "pdc", "pdc-fw", ".pdcfw", device_type=DEVICE_TYPE_V11
        )
        self.assertEqual(v11_dir, "pdc-fw/v20_0325_100W")

        v12_dir = maui_flash._resolve_tag_to_dir(  # pylint: disable=protected-access
            "stable", "pdc", "pdc-fw", ".pdcfw", device_type=DEVICE_TYPE_V12
        )
        self.assertEqual(v12_dir, "pdc-fw/v20_0325_100W")

    @patch("src.maui_flash._list_gcs_keys")
    def test_pdc_v11_does_not_fallback_to_proto_legacy_tag(self, mock_list_keys):
        """Ensures v11/v12/v20 never accidentally flash proto's legacy stable.pdcfw."""
        mock_list_keys.return_value = [
            "pdc-fw/v20_0325/v20_0325.bin",
            "pdc-fw/v20_0325/stable.pdcfw",
        ]
        with self.assertRaises(RuntimeError):
            maui_flash._resolve_tag_to_dir(  # pylint: disable=protected-access
                "stable", "pdc", "pdc-fw", ".pdcfw", device_type=DEVICE_TYPE_V11
            )
        with self.assertRaises(RuntimeError):
            maui_flash._resolve_tag_to_dir(  # pylint: disable=protected-access
                "stable", "pdc", "pdc-fw", ".pdcfw", device_type=DEVICE_TYPE_V12
            )

    @patch("src.maui_flash._list_gcs_keys")
    def test_mcu_resolution_and_binary_selection(self, mock_list_keys):
        """Verifies proto/v11 share MSPM0 .txt and v12/v20 select ITE .bin artifacts."""
        mock_list_keys.side_effect = self._mock_list_keys

        # proto and v11 resolve to mcu-fw/mspm0/0.60-042f858/zephyr.txt
        for dev_type in (DEVICE_TYPE_PROTO, DEVICE_TYPE_V11):
            d_key = maui_flash._resolve_tag_to_dir(  # pylint: disable=protected-access
                "stable", "mcu", "mcu-fw", ".mcufw", device_type=dev_type
            )
            self.assertEqual(d_key, "mcu-fw/mspm0/0.60-042f858")
            url = maui_flash._find_binary_in_dir(  # pylint: disable=protected-access
                d_key, ".txt", preferred_name="zephyr.txt"
            )
            self.assertTrue(url.endswith("/mcu-fw/mspm0/0.60-042f858/zephyr.txt"))

        # v12 resolves to mcu-fw/maui_v12/0.60-52742e3 and picks app or rw-bootloader
        v12_dir = maui_flash._resolve_tag_to_dir(  # pylint: disable=protected-access
            "stable", "mcu", "mcu-fw", ".mcufw", device_type=DEVICE_TYPE_V12
        )
        self.assertEqual(v12_dir, "mcu-fw/maui_v12/0.60-52742e3")

        app_url = maui_flash._find_binary_in_dir(  # pylint: disable=protected-access
            v12_dir,
            ".bin",
            preferred_name="maui_app_signed.bin",
            target_stage="app",
        )
        self.assertTrue(
            app_url.endswith("/mcu-fw/maui_v12/0.60-52742e3/maui_app_signed.bin")
        )

        rw_url = maui_flash._find_binary_in_dir(  # pylint: disable=protected-access
            v12_dir,
            ".bin",
            preferred_name="maui_rw_bootloader_signed.bin",
            target_stage="rw-bootloader",
        )
        self.assertTrue(
            rw_url.endswith(
                "/mcu-fw/maui_v12/0.60-52742e3/maui_rw_bootloader_signed.bin"
            )
        )

        # v20 resolves to mcu-fw/maui_v20/0.61-aaaa111
        v20_dir = maui_flash._resolve_tag_to_dir(  # pylint: disable=protected-access
            "stable", "mcu", "mcu-fw", ".mcufw", device_type=DEVICE_TYPE_V20
        )
        self.assertEqual(v20_dir, "mcu-fw/maui_v20/0.61-aaaa111")


if __name__ == "__main__":
    unittest.main()
