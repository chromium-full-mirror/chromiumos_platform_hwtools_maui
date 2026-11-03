# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Tests for `maui-flash h2h-serial`: JSON output, exit codes, board lookup."""

import argparse
import contextlib
import io
import json
import tempfile
import unittest
from unittest import mock

from maui_libs import h2h_serial as hs
from maui_libs import h2h_serial_cli
from maui_libs.rpc import RpcTransportError
from src import maui_flash
from test_h2h_serial import FakeBridge
from test_h2h_serial import FakeFlasher
from test_h2h_serial import make_image
from test_h2h_serial import make_port
from test_h2h_serial import NEW_SERIAL
from test_h2h_serial import OLD_SERIAL


class TestCli(unittest.TestCase):
    """`maui-flash h2h-serial`: JSON output and exit codes."""

    def setUp(self):
        self.base = make_image()
        self.bridge = FakeBridge(self.base)
        self.flasher = FakeFlasher(self.base, self.bridge)
        port = make_port("/dev/ttyACM1", "MCU123", "1-2.1:1.2")
        patches = [
            mock.patch.object(
                h2h_serial_cli, "find_rpc_port", return_value="/dev/ttyACM1"
            ),
            mock.patch.object(h2h_serial_cli, "list_rpc_ports", return_value=[port]),
            mock.patch.object(h2h_serial_cli, "RpcClient"),
            mock.patch.object(h2h_serial_cli, "H2hFlasher", return_value=self.flasher),
            mock.patch.object(hs, "scan_bridges", side_effect=self.bridge.scan),
            mock.patch.object(hs, "REENUM_SETTLE_S", 0),
            mock.patch.object(h2h_serial_cli, "list_maui_devices", return_value=[]),
            mock.patch.object(
                h2h_serial_cli.serial.tools.list_ports, "comports", return_value=[port]
            ),
        ]
        for p in patches:
            self.mocks = getattr(self, "mocks", []) + [p.start()]
            self.addCleanup(p.stop)

    @staticmethod
    def args(**kwargs):
        """maui-flash h2h-serial arguments."""
        defaults = {
            "serial": None,
            "port": None,
            "rpc_port": None,
            "get": False,
            "set_h2h_sn": None,
            "file": None,
            "force": False,
            "full_erase": False,
            "usb_only": False,
            "json": True,
            "reenum_timeout": 0.5,
        }
        defaults.update(kwargs)
        return argparse.Namespace(**defaults)

    def run_cli(self, **kwargs):
        """Runs the command; returns (exit code, the JSON result)."""
        out = io.StringIO()
        with contextlib.redirect_stdout(out), self.assertLogs(level="INFO"):
            code = h2h_serial_cli.handle(self.args(**kwargs))
        lines = out.getvalue().splitlines()
        self.assertEqual(len(lines), 1, out.getvalue())
        return code, json.loads(lines[0])

    def test_set_json(self):
        """A successful write prints one JSON object and returns 0."""
        code, res = self.run_cli(set_h2h_sn=NEW_SERIAL)
        self.assertEqual(code, 0)
        self.assertEqual(res["schema"], hs.RESULT_SCHEMA)
        self.assertTrue(res["success"])
        self.assertEqual(res["exit_code"], 0)
        self.assertEqual(res["operation"], "write")
        self.assertEqual(res["usb_serial"], NEW_SERIAL)
        self.assertEqual(res["flash_serial"], NEW_SERIAL)
        self.assertEqual(res["previous_serial"], OLD_SERIAL)
        self.assertEqual(res["flash_state"], "written")
        self.assertEqual(res["write_mode"], "sectors")
        self.assertEqual(res["write_range"], "0x0000-0x0fff")
        self.assertEqual(res["rpc_port"], "/dev/ttyACM1")
        self.assertEqual(res["mcu_usb_serial"], "MCU123")

    def test_set_full_erase_json(self):
        """--full-erase is passed through and reported."""
        code, res = self.run_cli(set_h2h_sn=NEW_SERIAL, full_erase=True)
        self.assertEqual(code, 0)
        self.assertEqual(res["write_mode"], "chip")

    def test_get_json(self):
        """--get reports the serial."""
        code, res = self.run_cli(get=True)
        self.assertEqual(code, 0)
        self.assertEqual(res["flash_serial"], OLD_SERIAL)
        self.assertFalse(res["flash_serial_valid"])

    def test_get_plain(self):
        """Without --json, --get prints only the serial on stdout."""
        out = io.StringIO()
        with contextlib.redirect_stdout(out), self.assertLogs(level="INFO"):
            code = h2h_serial_cli.handle(self.args(get=True, json=False))
        self.assertEqual(code, 0)
        self.assertEqual(out.getvalue(), OLD_SERIAL + "\n")

    def test_invalid_serial_exit_code(self):
        """A bad serial exits 3 without looking for the device."""
        code, res = self.run_cli(set_h2h_sn="MAUIV12H1")
        self.assertEqual(code, 3)
        self.assertEqual(res["exit_code"], 3)
        self.assertEqual(res["requested_serial"], "MAUIV12H1")
        self.assertFalse(res["success"])
        self.assertEqual(res["flash_state"], "untouched")
        h2h_serial_cli.find_rpc_port.assert_not_called()

    def test_invalid_file_exit_code(self):
        """A bad --file exits 4 without looking for the device."""
        with tempfile.NamedTemporaryFile(suffix=".bin") as f:
            f.write(self.base[:-1])
            f.flush()
            code, res = self.run_cli(set_h2h_sn=NEW_SERIAL, file=f.name)
        self.assertEqual(code, 4)
        self.assertEqual(res["image_file"], f.name)
        h2h_serial_cli.find_rpc_port.assert_not_called()
        code, _ = self.run_cli(set_h2h_sn=NEW_SERIAL, file="/nonexistent.bin")
        self.assertEqual(code, 4)

    def test_file(self):
        """--file is patched and flashed."""
        with tempfile.NamedTemporaryFile(suffix=".bin") as f:
            f.write(make_image("MAUIV11L26090001"))
            f.flush()
            code, res = self.run_cli(set_h2h_sn=NEW_SERIAL, file=f.name)
        self.assertEqual(code, 0)
        self.assertEqual(res["image_source"], "file")

    def test_no_device_exit_code(self):
        """No RPC port exits 5."""
        h2h_serial_cli.find_rpc_port.side_effect = RpcTransportError("No 'Maui RPC'")
        code, res = self.run_cli(set_h2h_sn=NEW_SERIAL)
        self.assertEqual(code, 5)
        self.assertIn("Maui RPC", res["message"])

    def test_flash_failure_exit_code(self):
        """A failed write exits 6 and reports the flash state."""
        self.flasher.fail_flash = RpcTransportError("h2h_write: no response")
        code, res = self.run_cli(set_h2h_sn=NEW_SERIAL)
        self.assertEqual(code, 6)
        self.assertEqual(res["flash_state"], "unknown")

    def test_unexpected_error_exit_code(self):
        """Unexpected exceptions exit 1 with a JSON result."""
        self.flasher.read = mock.Mock(side_effect=KeyError("boom"))
        code, res = self.run_cli(get=True)
        self.assertEqual(code, 1)
        self.assertIn("boom", res["message"])

    def test_interrupted_exit_code(self):
        """Ctrl-C exits 130 with a JSON result."""
        self.flasher.flash = mock.Mock(side_effect=KeyboardInterrupt)
        code, res = self.run_cli(set_h2h_sn=NEW_SERIAL)
        self.assertEqual(code, 130)
        self.assertEqual(res["flash_state"], "unknown")
        self.assertIsNone(res["flash_serial"])

    def test_main_usage_errors(self):
        """Option combinations that make no sense are usage errors (2)."""
        for argv in (
            ["h2h-serial"],
            ["h2h-serial", "--get", "--set", NEW_SERIAL],
            ["h2h-serial", "--get", "--file", "x.bin"],
            ["h2h-serial", "--get", "--force"],
            ["h2h-serial", "--get", "--full-erase"],
            ["h2h-serial", "--set", NEW_SERIAL, "--usb-only"],
        ):
            with self.subTest(argv=argv), mock.patch(
                "sys.argv", ["maui-flash"] + argv
            ), contextlib.redirect_stderr(io.StringIO()):
                with self.assertRaises(SystemExit) as ctx:
                    maui_flash.main()
                self.assertEqual(ctx.exception.code, 2)

    def test_usb_only_needs_no_rpc(self):
        """--usb-only works on a board without an RPC port."""
        h2h_serial_cli.find_rpc_port.side_effect = RpcTransportError("No 'Maui RPC'")
        code, res = self.run_cli(get=True, usb_only=True)
        self.assertEqual(code, 0)
        self.assertEqual(res["usb_serial"], OLD_SERIAL)
        self.assertEqual(res["mcu_usb_serial"], "MCU123")
        h2h_serial_cli.RpcClient.assert_not_called()

    def test_usb_only_several_boards(self):
        """Several boards need --port (or a distinct --serial)."""
        ports = [
            make_port("/dev/ttyACM0", "Uninitialized", "1-2.1:1.0"),
            make_port("/dev/ttyACM2", "Uninitialized", "3-2.1:1.0"),
        ]
        h2h_serial_cli.serial.tools.list_ports.comports.return_value = ports
        code, res = self.run_cli(get=True, usb_only=True)
        self.assertEqual(code, 5)
        self.assertIn("--port", res["message"])

    def test_json_usage_errors(self):
        """With --json, command-line errors also print a JSON result."""
        for argv in (
            ["h2h-serial", "--get", "--file", "x.bin", "--json"],
            ["h2h-serial", "--set", "--json"],  # No value.
            ["h2h-serial", "--get", "--reenum-timeout", "-1", "--json"],
        ):
            out = io.StringIO()
            with self.subTest(argv=argv), mock.patch(
                "sys.argv", ["maui-flash"] + argv
            ), contextlib.redirect_stdout(out), contextlib.redirect_stderr(
                io.StringIO()
            ):
                with self.assertRaises(SystemExit) as ctx:
                    maui_flash.main()
                self.assertEqual(ctx.exception.code, 2)
                res = json.loads(out.getvalue())
                self.assertEqual(res["exit_code"], 2)
                self.assertFalse(res["success"])
                self.assertEqual(res["flash_state"], "untouched")

    def test_dash_serial_is_invalid_serial(self):
        """A scanned value starting with '-' is an invalid serial (3)."""
        out = io.StringIO()
        argv = ["maui-flash", "h2h-serial", "--set", "-123", "--json"]
        with mock.patch("sys.argv", argv), contextlib.redirect_stdout(
            out
        ), self.assertLogs(level="ERROR"):
            with self.assertRaises(SystemExit) as ctx:
                maui_flash.main()
        self.assertEqual(ctx.exception.code, 3)
        self.assertEqual(json.loads(out.getvalue())["requested_serial"], "-123")

    def test_main_exit_code(self):
        """main() exits with the h2h-serial exit code."""
        argv = ["maui-flash", "h2h-serial", "--set", "bad", "--json"]
        with mock.patch("sys.argv", argv), contextlib.redirect_stdout(
            io.StringIO()
        ), self.assertLogs(level="ERROR"):
            with self.assertRaises(SystemExit) as ctx:
                maui_flash.main()
        self.assertEqual(ctx.exception.code, 3)


if __name__ == "__main__":
    unittest.main()
