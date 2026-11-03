# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""`maui-flash h2h-serial`: read or write the H2H bridge serial number.

    maui-flash [--serial MCU_SN | --port CONSOLE] h2h-serial --get [--usb-only]
    maui-flash [...] h2h-serial --set MAUIV12H26110042 [--file BASE.bin] [--force]
                                [--full-erase]

With --json, exactly one JSON object (h2h_serial.H2hSerialResult.to_dict(),
schema h2h_serial.RESULT_SCHEMA) is printed on stdout, also for command-line
errors (exit code 2); logs and progress go to stderr. The exit code is one of
h2h_serial.ExitCode.
"""

import argparse
import json
import logging
import os
import sys

from maui_libs import h2h_serial
from maui_libs.h2h import H2hFlasher
from maui_libs.h2h_serial import ExitCode
from maui_libs.h2h_serial import H2hSerialError
from maui_libs.h2h_serial import H2hSerialResult
from maui_libs.rpc import find_rpc_port
from maui_libs.rpc import list_rpc_ports
from maui_libs.rpc import RpcClient
from maui_libs.rpc import RpcTransportError
from maui_libs.transport import list_maui_devices
from maui_libs.transport import MAUI_FINAL_PID
from maui_libs.transport import MAUI_FINAL_VID
from maui_libs.transport import MAUI_IT82002_APP_PID
from maui_libs.transport import MAUI_IT82002_BOOTLOADER_PID
from maui_libs.transport import MSPM0_DEVICE_TYPES
import serial
import serial.tools.list_ports


logger = logging.getLogger(__name__)

COMMAND = "h2h-serial"


def add_parser(subparsers) -> argparse.ArgumentParser:
    """Adds the h2h-serial subcommand to maui-flash; returns its parser."""
    parser = subparsers.add_parser(
        COMMAND,
        help="Read or write the unit serial number in the H2H bridge image",
        description=(
            "Reads or writes the unit serial number (USB iSerial) in the "
            "Prolific PL27A1 H2H bridge image over the RPC port. --set patches "
            "the serial into the base image, flashes and verifies it, resets "
            "the bridge and checks its new iSerial on USB. The global --serial "
            "and --port select the Maui board (its MCU), not the H2H serial."
        ),
        epilog="Exit codes: " + ", ".join(f"{int(c)} {c.name}" for c in ExitCode) + ".",
    )
    group = parser.add_mutually_exclusive_group(required=True)
    group.add_argument(
        "--get",
        action="store_true",
        help=(
            "Read the serial from the H2H flash (resets the bridge) and check "
            "it against the bridge's iSerial on USB"
        ),
    )
    group.add_argument(
        "--set",
        dest="set_h2h_sn",
        metavar="H2H_SERIAL",
        help=f"Write a new serial ({h2h_serial.SERIAL_FORMAT})",
    )
    parser.add_argument(
        "--file",
        help=(
            "--set: base PL27A1 image to patch "
            f"({h2h_serial.H2H_IMAGE_SIZE} B raw .bin; default: the image "
            "currently in the H2H flash)"
        ),
    )
    parser.add_argument(
        "--force",
        action="store_true",
        help="--set: rewrite the flash even if it already holds the patched image",
    )
    parser.add_argument(
        "--full-erase",
        action="store_true",
        help=(
            "--set: erase the whole chip and write the whole image (default: "
            "only rewrite the 4 KB sector(s) that change, if the MCU App "
            "supports it)"
        ),
    )
    parser.add_argument(
        "--usb-only",
        action="store_true",
        help="--get: only read the iSerial on USB, do not touch the H2H flash",
    )
    parser.add_argument(
        "--json",
        action="store_true",
        help="Print the result as one JSON object on stdout (logs go to stderr)",
    )
    parser.add_argument(
        "--reenum-timeout",
        type=float,
        default=h2h_serial.REENUM_TIMEOUT_S,
        help=(
            "Seconds the bridge has to enumerate on USB after its reset, "
            f"after a {h2h_serial.REENUM_SETTLE_S:g} s settle delay (default: "
            f"{h2h_serial.REENUM_TIMEOUT_S:.0f})"
        ),
    )
    parser.add_argument(
        "--rpc-port",
        help="RPC port of the device (default: auto-detect the 'Maui RPC' port)",
    )
    return parser


def _print_json(result: H2hSerialResult):
    sys.stdout.write(json.dumps(result.to_dict(), sort_keys=True) + "\n")
    sys.stdout.flush()


def json_usage_errors(argv, *parsers: argparse.ArgumentParser):
    """With `h2h-serial ... --json` in argv, makes command-line errors of the
    given parsers also print a JSON result (exit code 2) on stdout."""
    if COMMAND not in argv or "--json" not in argv:
        return
    writing = any(a == "--set" or a.startswith("--set=") for a in argv)

    def make_error(parser):
        def error(message):
            parser.print_usage(sys.stderr)
            sys.stderr.write(f"{parser.prog}: error: {message}\n")
            result = H2hSerialResult("write" if writing else "read")
            result.fail(ExitCode.USAGE, f"Command-line error: {message}")
            _print_json(result)
            sys.exit(int(ExitCode.USAGE))

        return error

    for parser in parsers:
        parser.error = make_error(parser)


def check_args(parser: argparse.ArgumentParser, args: argparse.Namespace):
    """Rejects option combinations that make no sense (exit code 2)."""
    for option, given, needs, ok in (
        ("--file", args.file, "--set", args.set_h2h_sn is not None),
        ("--force", args.force, "--set", args.set_h2h_sn is not None),
        ("--full-erase", args.full_erase, "--set", args.set_h2h_sn is not None),
        ("--usb-only", args.usb_only, "--get", args.get),
    ):
        if given and not ok:
            parser.error(f"{COMMAND}: {option} needs {needs}")
    if args.reenum_timeout < 0:
        parser.error(f"{COMMAND}: --reenum-timeout must not be negative")


def _usb_device_path(rpc_port: str) -> str:
    """sysfs path of the USB device that owns rpc_port, '' if unknown."""
    target = os.path.realpath(rpc_port)
    for p in serial.tools.list_ports.comports():
        if os.path.realpath(p.device) == target:
            return h2h_serial.usb_device_path(p.location)
    return ""


def _usb_key(port) -> str:
    """The USB device of a pyserial port ('1-3.2' of '1-3.2:1.2')."""
    return (port.location or port.device).split(":", 1)[0]


def _ite_ports() -> list:
    """Every CDC-ACM port of a Maui IT82002 board (App or bootloader)."""
    return [
        p
        for p in serial.tools.list_ports.comports()
        if p.vid == MAUI_FINAL_VID
        and p.pid in (MAUI_FINAL_PID, MAUI_IT82002_APP_PID, MAUI_IT82002_BOOTLOADER_PID)
    ]


def _board_count() -> int:
    """Number of Maui boards connected, in any mode, with or without RPC."""
    ite = {_usb_key(p) for p in _ite_ports()}
    ftdi = [d for d in list_maui_devices() if d["type"] in MSPM0_DEVICE_TYPES]
    return len(ite) + len(ftdi)


def _find_board(args: argparse.Namespace):
    """(sysfs USB path, USB serial) of the selected board, without RPC.

    Selected by --rpc-port/--port, else --serial, else the only board.
    """
    ports = _ite_ports()
    if args.rpc_port or args.port:
        target = os.path.realpath(args.rpc_port or args.port)
        chosen = [p for p in ports if os.path.realpath(p.device) == target]
    else:
        chosen = [p for p in ports if args.serial in (None, p.serial_number)]
    boards = {_usb_key(p) for p in chosen}
    if not boards:
        raise H2hSerialError(
            ExitCode.DEVICE_ERROR,
            f"No Maui board ({MAUI_FINAL_VID:04x}:{MAUI_FINAL_PID:04x}) found"
            + (" for the given --serial/--port." if ports else "."),
        )
    if len(boards) > 1:
        raise H2hSerialError(
            ExitCode.DEVICE_ERROR,
            f"{len(boards)} Maui boards found ({', '.join(sorted(boards))}): "
            "select one with --port (or --serial if their serials differ).",
        )
    return h2h_serial.usb_device_path(chosen[0].location), chosen[0].serial_number or ""


def _read_base_image(path: str) -> bytes:
    try:
        with open(path, "rb") as f:
            data = f.read(h2h_serial.H2H_IMAGE_SIZE + 1)
    except OSError as e:
        raise H2hSerialError(
            ExitCode.INVALID_IMAGE, f"Cannot read H2H image: {e}"
        ) from e
    h2h_serial.check_base_image(data)
    return data


def _run(args: argparse.Namespace, result: H2hSerialResult, progress):
    """Runs one operation into result; raises H2hSerialError."""
    base_image = None
    if result.operation == "write":
        # Everything that can be checked offline is checked before any device
        # access, so that a bad request never touches the board.
        result.requested_serial = args.set_h2h_sn
        h2h_serial.validate_serial(args.set_h2h_sn)
        if args.file:
            result.image_file = args.file
            result.image_source = "file"
            base_image = _read_base_image(args.file)

    if args.usb_only:
        # Only USB is read: no RPC needed, so any MCU App (or none) will do.
        mcu_usb_path, result.mcu_usb_serial = _find_board(args)
        tool = h2h_serial.H2hSerialTool(
            None,
            mcu_usb_path=mcu_usb_path,
            board_count=_board_count(),
            reenum_timeout_s=args.reenum_timeout,
        )
        tool.read(result, usb_only=True)
        return

    try:
        rpc_port = args.rpc_port or find_rpc_port(
            serial_number=args.serial, console_port=args.port
        )
        result.rpc_port = rpc_port
        for p in list_rpc_ports():
            if os.path.realpath(p.device) == os.path.realpath(rpc_port):
                result.mcu_usb_serial = p.serial_number or ""
        logger.info("Using RPC port %s", rpc_port)
        rpc = RpcClient(rpc_port)
        rpc.open()
    except (RpcTransportError, serial.SerialException, OSError) as e:
        raise H2hSerialError(ExitCode.DEVICE_ERROR, str(e)) from e

    try:
        tool = h2h_serial.H2hSerialTool(
            H2hFlasher(rpc),
            mcu_usb_path=_usb_device_path(rpc_port),
            board_count=_board_count(),
            reenum_timeout_s=args.reenum_timeout,
            progress=progress,
        )
        if result.operation == "write":
            tool.write(
                result,
                args.set_h2h_sn,
                base_image=base_image,
                force=args.force,
                full_erase=args.full_erase,
            )
        else:
            tool.read(result)
    finally:
        rpc.close()


def handle(args: argparse.Namespace, progress=None) -> int:
    """Runs `maui-flash h2h-serial`; returns the exit code (ExitCode).

    progress is an optional h2h.ProgressCallback; its done() method, if any,
    is called at the end.
    """
    result = H2hSerialResult(
        operation="write" if args.set_h2h_sn is not None else "read"
    )
    try:
        _run(args, result, progress)
        result.success = True
        result.exit_code = int(ExitCode.OK)
    except H2hSerialError as e:
        result.fail(e.code, str(e))
    except KeyboardInterrupt:
        result.fail(ExitCode.INTERRUPTED, "Interrupted.")
    except Exception as e:  # pylint: disable=broad-exception-caught
        logger.debug("%s failed", COMMAND, exc_info=True)
        result.fail(ExitCode.ERROR, f"Unexpected error: {e}")
    finally:
        if hasattr(progress, "done"):
            progress.done()

    for warning in result.warnings:
        logger.warning("%s", warning)
    if result.success:
        logger.info("%s", result.message)
    else:
        logger.error(
            "%s [exit %d, H2H flash %s]",
            result.message,
            result.exit_code,
            result.flash_state,
        )
    if args.json:
        _print_json(result)
    elif result.success:
        print(result.usb_serial if args.usb_only else result.flash_serial)
    return result.exit_code
