# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Tests for the H2H (PL27A1) serial number tool (maui_libs.h2h_serial)."""

import os
import struct
import tempfile
import unittest
from unittest import mock

from maui_libs import h2h_serial as hs
from maui_libs.h2h import H2H_IMAGE_SIZE
from maui_libs.h2h import H2hFlashInfo
from maui_libs.h2h import H2hFlashResult
from maui_libs.h2h import H2hReadResult
from maui_libs.rpc import RpcError
from maui_libs.rpc import RpcTransportError


# Serial of the pre-burned engineering image; not a valid unit serial (month 21).
OLD_SERIAL = "MAUIV10G05210033"
NEW_SERIAL = "MAUIV12H26110042"
JEDEC_ID = 0xEF4013
FLASH_SIZE = 512 * 1024
MCU_USB = "/sys/devices/pci0000:00/usb1/1-2/1-2.1"
H2H_USB = "/sys/devices/pci0000:00/usb1/1-2/1-2.3"


def _string(text: str) -> bytes:
    encoded = text.encode("utf-16-le")
    return bytes([2 + len(encoded), hs.USB_DT_STRING]) + encoded


def make_image(
    serial: str = OLD_SERIAL,
    *,
    manufacturer: str = "Google LLC",
    product: str = "ADB v1",
    pid: int = hs.IMAGE_PID,
    signature: bytes = hs.IMAGE_SIGNATURE,
) -> bytes:
    """A synthetic raw PL27A1 image with the real layout."""
    data = bytearray(H2H_IMAGE_SIZE)
    data[0:8] = signature + struct.pack("<HHH", hs.GOOGLE_VID, pid, 0x0200)
    data[8:10] = b"\xa0\x32"
    strings = (
        b"\x04\x03\x09\x04" + _string(manufacturer) + _string(product) + _string(serial)
    )
    data[0x10 : 0x10 + len(strings)] = strings
    # Hardware config and firmware: anything non-zero that must not change.
    for i in range(hs.STRING_REGION_END, H2H_IMAGE_SIZE):
        data[i] = (i * 7 + 3) & 0xFF
    return bytes(data)


class FakeFlasher:  # pylint: disable=too-many-instance-attributes
    """In-memory H2H flash behind the H2hFlasher interface.

    Like the firmware, any read or write session holds the PL27A1 in reset
    and lets it boot from the flash at the end (bridge.boot()).
    """

    def __init__(self, image: bytes, bridge: "FakeBridge" = None):
        self.mem = bytearray(b"\xff" * FLASH_SIZE)
        self.mem[: len(image)] = image
        self.bridge = bridge
        self.calls = []
        self.supported = {"h2h_begin", "h2h_read_begin", "h2h_begin_range"}
        self.fail_flash = None
        self.fail_flash_times = None  # None: every write fails.
        self.corrupt_after_write = False
        self.fail_read_n = None  # Fail the n-th read (1-based).
        self._reads = 0

    def supports(self, request):
        """H2hFlasher.supports()."""
        return request in self.supported

    def check_supported(self, request="h2h_begin"):
        """H2hFlasher.check_supported()."""
        self.calls.append(("check_supported", request))
        if request not in self.supported:
            raise RuntimeError(f"Device firmware does not support {request} over RPC")

    def read(self, size=None, progress=None):
        """H2hFlasher.read()."""
        del progress
        self._reads += 1
        self.calls.append(("read", size))
        if self.fail_read_n == self._reads:
            raise RpcTransportError("h2h_read: no response in 10.0s")
        self._boot()
        return H2hReadResult(
            info=H2hFlashInfo(JEDEC_ID, FLASH_SIZE),
            data=bytes(self.mem[:size]),
            host_s=0.1,
        )

    def flash(self, image, progress=None, offset=None):
        """H2hFlasher.flash(): chip erase, or sector erase with offset."""
        del progress
        self.calls.append(("flash", len(image), offset))
        base = offset or 0
        if offset is None:
            self.mem[:] = b"\xff" * FLASH_SIZE
        else:
            assert offset % hs.H2H_SECTOR_SIZE == 0
            assert len(image) % hs.H2H_SECTOR_SIZE == 0
            self.mem[base : base + len(image)] = b"\xff" * len(image)
        if self.fail_flash and self.fail_flash_times != 0:
            if self.fail_flash_times is not None:
                self.fail_flash_times -= 1
            self.mem[base : base + 100] = image[:100]
            raise self.fail_flash
        self.mem[base : base + len(image)] = image
        if self.corrupt_after_write:
            self.mem[0x200] ^= 0xFF
        self._boot()
        return H2hFlashResult(H2hFlashInfo(JEDEC_ID, FLASH_SIZE), len(image), 0, 5, 0.2)

    def _boot(self):
        if self.bridge:
            self.bridge.boot(bytes(self.mem[:H2H_IMAGE_SIZE]))

    @property
    def wrote(self):
        """True if a write session was started."""
        return any(c[0] == "flash" for c in self.calls)


class FakeBridge:
    """The bridge as the host sees it: re-enumerates with the flash's serial."""

    def __init__(self, image: bytes, enumerates: bool = True):
        self.enumerates = enumerates
        self.serial_override = None
        self.serial = hs.decode_image(image).serial

    def boot(self, image: bytes):
        """The PL27A1 left reset and booted from image."""
        try:
            self.serial = hs.decode_image(image).serial
        except ValueError:
            self.serial = None  # A blank or corrupt image does not boot.

    def scan(self):
        """scan_bridges() replacement."""
        if not self.enumerates or self.serial is None:
            return []
        return [
            hs.UsbBridge(
                sys_path=H2H_USB,
                pid=hs.IMAGE_PID,
                serial=self.serial_override or self.serial,
            )
        ]


class FakeClock:
    """Monotonic clock advanced by sleep()."""

    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        """time.sleep() replacement."""
        self.now += seconds


def make_tool(flasher, bridge, **kwargs):
    """An H2hSerialTool on fakes with a fake clock."""
    clock = FakeClock()
    return hs.H2hSerialTool(
        flasher,
        mcu_usb_path=MCU_USB,
        scan=bridge.scan,
        sleep=clock.sleep,
        clock=clock,
        **kwargs,
    )


def make_port(device, serial_number, location):
    """A pyserial port of a Maui IT82002 board."""
    return mock.Mock(
        device=device,
        serial_number=serial_number,
        location=location,
        vid=0x18D1,
        pid=0x5085,
    )


class TestSerialFormat(unittest.TestCase):
    """Serial number validation."""

    def test_valid(self):
        """MAUIV + version + batch letter + YYMM + 4 digits."""
        for serial in (
            "MAUIV12H26010001",
            "MAUIV12A00129999",
            "MAUIV11L26090000",
            "MAUIV20Z99100000",
        ):
            with self.subTest(serial=serial):
                self.assertTrue(hs.is_valid_serial(serial))
                hs.validate_serial(serial)

    def test_invalid(self):
        """Wrong prefix, case, month, length, characters or whitespace."""
        for serial in (
            None,
            "",
            "MAUIV12H2601000",  # 3-digit unit.
            "MAUIV12H260100011",  # 5-digit unit.
            "mauiv12h26010001",
            "MAUIV12h26010001",  # Lower-case batch.
            "MAUIV122260100001",  # Digit instead of a batch letter.
            "MAUIV1HH26010001",  # 1-digit version.
            "MAUIV12H26000001",  # Month 00.
            "MAUIV12H26130001",  # Month 13.
            "MAUIX12H26010001",
            "MAUIV12H2601000A",
            "MAUIV12H26010001\n",
            " MAUIV12H26010001",
            "MAUIV12H２６０１０００１",  # Full-width digits.
            "MAUIV12Ä26010001",  # Non-ASCII letter.
        ):
            with self.subTest(serial=serial):
                self.assertFalse(hs.is_valid_serial(serial))
                with self.assertRaises(hs.H2hSerialError) as ctx:
                    hs.validate_serial(serial)
                self.assertEqual(ctx.exception.code, hs.ExitCode.INVALID_SERIAL)


class TestImage(unittest.TestCase):
    """Image decoding, validation and patching."""

    def test_decode(self):
        """The header and the strings of a raw image."""
        image = hs.decode_image(make_image())
        self.assertEqual((image.vid, image.pid), (0x18D1, 0x506D))
        self.assertEqual(image.manufacturer, "Google LLC")
        self.assertEqual(image.product, "ADB v1")
        self.assertEqual(image.serial, OLD_SERIAL)
        self.assertEqual(image.warnings, [])

    def test_decode_warnings(self):
        """Unexpected strings decode with a warning."""
        image = hs.decode_image(make_image(product="Other"))
        self.assertEqual(len(image.warnings), 1)
        self.assertIn("product", image.warnings[0])

    def test_patch_changes_only_the_serial(self):
        """Every byte outside the serial descriptor stays as it is."""
        old = make_image()
        new = hs.patch_serial(old, NEW_SERIAL)
        self.assertEqual(len(new), len(old))
        self.assertEqual(hs.decode_image(new).serial, NEW_SERIAL)
        start = hs.SERIAL_DESCRIPTOR_OFFSET
        end = start + 2 + 2 * len(NEW_SERIAL)
        self.assertEqual(new[:start], old[:start])
        self.assertEqual(new[end:], old[end:])
        back = hs.patch_serial(make_image("MAUIV12H00000001"), NEW_SERIAL)
        self.assertEqual(back, new)

    def test_patch_is_idempotent(self):
        """Patching the serial an image already has returns it unchanged."""
        image = make_image(NEW_SERIAL)
        self.assertEqual(hs.patch_serial(image, NEW_SERIAL), image)

    def test_patch_non_v12_base_serial(self):
        """The pre-burned image may carry any (e.g. a v1.1) serial."""
        new = hs.patch_serial(make_image("MAUIV11L26090000"), NEW_SERIAL)
        self.assertEqual(hs.decode_image(new).serial, NEW_SERIAL)

    def test_patch_rejects_bad_serial(self):
        """The serial is validated before the image."""
        with self.assertRaises(hs.H2hSerialError) as ctx:
            hs.patch_serial(b"", "MAUIV12H1")
        self.assertEqual(ctx.exception.code, hs.ExitCode.INVALID_SERIAL)

    def test_bad_images(self):
        """Images that cannot be patched are rejected as INVALID_IMAGE."""
        good = make_image()
        moved = make_image(manufacturer="Google LLC Inc")  # Serial not at 0x38.
        dirty = bytearray(good)
        dirty[0xF0] = 0x01  # Garbage in the zero fill after the serial.
        broken = bytearray(good)
        broken[0x11] = 0x02  # LANGID descriptor type.
        cases = {
            "short": good[:-1],
            "long": good + b"\xff",
            "full dump": good + b"\xff" * (FLASH_SIZE - len(good)),
            "blank": b"\xff" * H2H_IMAGE_SIZE,
            "zero": bytes(H2H_IMAGE_SIZE),
            "pid": make_image(pid=0x5084),
            "signature": make_image(signature=b"\x00\x00"),
            "moved serial": moved,
            "dirty fill": bytes(dirty),
            "descriptor": bytes(broken),
        }
        for name, data in cases.items():
            with self.subTest(name):
                with self.assertRaises(hs.H2hSerialError) as ctx:
                    hs.check_base_image(data)
                self.assertEqual(ctx.exception.code, hs.ExitCode.INVALID_IMAGE)
                with self.assertRaises(hs.H2hSerialError):
                    hs.patch_serial(data, NEW_SERIAL)


class TestChangedSectors(unittest.TestCase):
    """changed_sectors()."""

    def test_changed_sectors(self):
        """The sector-aligned range covering every difference."""
        sector = hs.H2H_SECTOR_SIZE
        old = make_image()
        self.assertIsNone(hs.changed_sectors(old, old))
        new = hs.patch_serial(old, NEW_SERIAL)
        self.assertEqual(hs.changed_sectors(old, new), range(0, sector))
        both = bytearray(new)
        both[sector + 5] ^= 1
        self.assertEqual(hs.changed_sectors(old, bytes(both)), range(0, 2 * sector))
        only_1 = bytearray(old)
        only_1[2 * sector - 1] ^= 1
        self.assertEqual(
            hs.changed_sectors(old, bytes(only_1)), range(sector, 2 * sector)
        )
        last = bytearray(old)
        last[-1] ^= 1
        self.assertIsNone(hs.changed_sectors(old, bytes(last)))
        with self.assertRaises(ValueError):
            hs.changed_sectors(old, old[:-1])


class TestUsb(unittest.TestCase):
    """Finding the board's bridge on USB."""

    def test_scan_bridges(self):
        """Only 18d1:506d/5084 devices (not interfaces) are bridges."""
        with tempfile.TemporaryDirectory() as root:

            def device(name, vid, pid, serial):
                os.makedirs(os.path.join(root, name))
                for attr, value in (
                    ("idVendor", vid),
                    ("idProduct", pid),
                    ("serial", serial),
                    ("devnum", "7"),
                ):
                    path = os.path.join(root, name, attr)
                    with open(path, "w", encoding="utf-8") as f:
                        f.write(value + "\n")

            device("1-2.3", "18d1", "506d", NEW_SERIAL)
            device("1-2.1", "18d1", "5085", "MCU")
            device("1-2.3:1.0", "18d1", "506d", "")
            device("2-2.3", "18d1", "5084", "OTHER")
            os.makedirs(os.path.join(root, "usb1"))
            bridges = hs.scan_bridges(root)
        self.assertEqual([b.serial for b in bridges], [NEW_SERIAL, "OTHER"])
        self.assertEqual(bridges[0].pid, 0x506D)
        self.assertEqual(bridges[0].devnum, "7")
        self.assertEqual(hs.scan_bridges("/nonexistent"), [])

    def test_usb_device_path(self):
        """A pyserial location maps to its USB device directory."""
        self.assertEqual(hs.usb_device_path(None), "")
        with tempfile.TemporaryDirectory() as root:
            self.assertEqual(
                hs.usb_device_path("1-2.1:1.2", root),
                os.path.realpath(os.path.join(root, "1-2.1")),
            )

    @staticmethod
    def _sysfs(root, ports, devices):
        """Builds hub ports (with 'peer' links) and USB devices under root.

        ports: {(controller, bus, port): (peer_bus, peer_port)}
        devices: [(controller, path below the controller)]
        """

        def port_dir(ctrl, bus, port):
            return os.path.join(
                root, ctrl, f"usb{bus}", f"{bus}-0:1.0", f"usb{bus}-port{port}"
            )

        for (ctrl, bus, port), (pbus, pport) in ports.items():
            os.makedirs(port_dir(ctrl, bus, port), exist_ok=True)
            os.makedirs(port_dir(ctrl, pbus, pport), exist_ok=True)
            os.symlink(
                port_dir(ctrl, pbus, pport),
                os.path.join(port_dir(ctrl, bus, port), "peer"),
            )
        out = []
        for ctrl, rel in devices:
            path = os.path.join(root, ctrl, rel)
            os.makedirs(path, exist_ok=True)
            out.append(path)
        return out

    def test_pair_bridge_two_boards_peer_links(self):
        """Two boards on two controllers, same port numbers: each MCU gets
        the bridge of its own board (USB 3 half matched by port peers)."""
        with tempfile.TemporaryDirectory() as root:
            # pylint: disable-next=unbalanced-tuple-unpacking
            mcu_a, br_a, mcu_b, br_b = self._sysfs(
                root,
                {
                    ("c1", 1, 2): (2, 2),
                    ("c1", 2, 2): (1, 2),
                    ("c2", 3, 2): (4, 2),
                    ("c2", 4, 2): (3, 2),
                },
                [
                    ("c1", "usb1/1-2/1-2.1"),
                    ("c1", "usb2/2-2/2-2.3"),
                    ("c2", "usb3/3-2/3-2.1"),
                    ("c2", "usb4/4-2/4-2.3"),
                ],
            )
            bridges = [
                hs.UsbBridge(br_b, hs.IMAGE_PID, "B"),
                hs.UsbBridge(br_a, hs.IMAGE_PID, "A"),
            ]
            self.assertEqual(hs.pair_bridge(mcu_a, bridges, 2).serial, "A")
            self.assertEqual(hs.pair_bridge(mcu_b, bridges, 2).serial, "B")
            self.assertEqual(hs.pair_bridge(mcu_a, bridges[:1], 2), None)

    def test_pair_bridge_peer_beats_port_number(self):
        """USB 2 port 2 may pair with USB 3 port 1: the peer link decides,
        not the port number (another board sits on USB 3 port 2)."""
        with tempfile.TemporaryDirectory() as root:
            # pylint: disable-next=unbalanced-tuple-unpacking
            mcu, mine, other = self._sysfs(
                root,
                {("c1", 1, 2): (2, 1), ("c1", 2, 1): (1, 2)},
                [
                    ("c1", "usb1/1-2/1-2.1"),
                    ("c1", "usb2/2-1/2-1.3"),
                    ("c1", "usb2/2-2/2-2.3"),
                ],
            )
            bridges = [
                hs.UsbBridge(other, hs.IMAGE_PID, "X"),
                hs.UsbBridge(mine, hs.IMAGE_PID, "M"),
            ]
            self.assertEqual(hs.pair_bridge(mcu, bridges, 2).serial, "M")

    def test_pair_bridge_ambiguous(self):
        """Two bridges on the MCU's hub: refuse to guess."""
        hub = "/sys/x/usb1/1-2"
        bridges = [
            hs.UsbBridge(f"{hub}/1-2.3", hs.IMAGE_PID, "A"),
            hs.UsbBridge(f"{hub}/1-2.4", hs.IMAGE_PID, "B"),
        ]
        with self.assertLogs(hs.logger, "WARNING"):
            self.assertIsNone(hs.pair_bridge(f"{hub}/1-2.1", bridges, 2))

    def test_pair_bridge_no_peer_info_needs_same_controller(self):
        """Without peer links, the other bus must be on the same controller."""
        mcu = "/sys/c1/usb1/1-2/1-2.1"
        same = hs.UsbBridge("/sys/c1/usb2/2-2/2-2.3", hs.IMAGE_PID, "S")
        other_ctrl = hs.UsbBridge("/sys/c2/usb4/4-2/4-2.3", hs.IMAGE_PID, "O")
        self.assertIs(hs.pair_bridge(mcu, [other_ctrl, same], 2), same)
        self.assertIsNone(hs.pair_bridge(mcu, [other_ctrl], 2))

    def test_pair_bridge(self):
        """Same hub, same port chain on the other bus, lone bridge."""
        same_hub = hs.UsbBridge("/sys/x/usb1/1-2/1-2.3", 0x506D, "A")
        other_bus = hs.UsbBridge("/sys/x/usb2/2-2/2-2.3", 0x506D, "B")
        elsewhere = hs.UsbBridge("/sys/x/usb1/1-4/1-4.3", 0x506D, "C")
        mcu = "/sys/x/usb1/1-2/1-2.1"
        self.assertIs(hs.pair_bridge(mcu, [elsewhere, same_hub], 2), same_hub)
        self.assertIs(hs.pair_bridge(mcu, [elsewhere, other_bus], 2), other_bus)
        self.assertIsNone(hs.pair_bridge(mcu, [elsewhere], 2))
        self.assertIs(hs.pair_bridge(mcu, [elsewhere], 1), elsewhere)
        self.assertIs(hs.pair_bridge("", [elsewhere], 1), elsewhere)
        self.assertIsNone(hs.pair_bridge(mcu, [], 1))


class WriteFixture(unittest.TestCase):
    """An H2hSerialTool on a fake board with a pre-burned image."""

    def setUp(self):
        self.base = make_image()
        self.bridge = FakeBridge(self.base)
        self.flasher = FakeFlasher(self.base, self.bridge)
        self.tool = make_tool(self.flasher, self.bridge)
        self.result = hs.H2hSerialResult("write")

    def write(self, serial=NEW_SERIAL, **kwargs):
        """Runs the tool's write()."""
        self.tool.write(self.result, serial, **kwargs)

    def assert_fails(self, code, serial=NEW_SERIAL, **kwargs):
        """Checks that write() fails with exit code `code`."""
        with self.assertRaises(hs.H2hSerialError) as ctx:
            self.write(serial, **kwargs)
        self.assertEqual(ctx.exception.code, code, str(ctx.exception))
        return ctx.exception


class TestWrite(WriteFixture):
    """H2hSerialTool.write()."""

    def test_write_from_flash(self):
        """Patches the image in the flash, writes, reads back, checks USB."""
        self.write()
        expected = hs.patch_serial(self.base, NEW_SERIAL)
        self.assertEqual(bytes(self.flasher.mem[:H2H_IMAGE_SIZE]), expected)
        self.assertEqual(
            [c[0] for c in self.flasher.calls],
            ["check_supported", "check_supported", "read", "flash", "read"],
        )
        r = self.result
        self.assertEqual(r.previous_serial, OLD_SERIAL)
        self.assertEqual(r.flash_serial, NEW_SERIAL)
        self.assertEqual(r.usb_serial, NEW_SERIAL)
        self.assertEqual(r.usb_pid, "18d1:506d")
        self.assertEqual(r.image_source, "flash")
        self.assertEqual(r.flash_state, hs.FLASH_WRITTEN)
        self.assertTrue(r.written)
        self.assertTrue(r.readback_ok)
        self.assertEqual(r.jedec_id, "0xEF4013")
        self.assertEqual(r.flash_size, FLASH_SIZE)
        self.assertEqual(r.write_mode, hs.WRITE_SECTORS)
        self.assertEqual(r.write_range, "0x0000-0x0fff")

    def test_write_from_file(self):
        """A base image file replaces the image in the flash."""
        other = make_image("MAUIV11L26090007", product="ADB v2")
        self.write(base_image=other)
        flashed = bytes(self.flasher.mem[:H2H_IMAGE_SIZE])
        self.assertEqual(flashed, hs.patch_serial(other, NEW_SERIAL))
        self.assertEqual(self.result.image_source, "file")
        self.assertEqual(self.result.previous_serial, OLD_SERIAL)

    def test_write_over_blank_flash_with_file(self):
        """A blank board can be written from a file."""
        self.flasher.mem[:] = b"\xff" * FLASH_SIZE
        self.bridge.serial = None
        self.write(base_image=self.base)
        self.assertEqual(self.result.usb_serial, NEW_SERIAL)
        self.assertIsNone(self.result.previous_serial)

    def test_same_serial_is_not_rewritten(self):
        """A retry with the serial already written does not erase anything."""
        self.write()
        self.flasher.calls.clear()
        self.result = hs.H2hSerialResult("write")
        self.write()
        self.assertFalse(self.flasher.wrote)
        self.assertFalse(self.result.written)
        self.assertEqual(self.result.flash_state, hs.FLASH_UNTOUCHED)
        self.assertEqual(self.result.usb_serial, NEW_SERIAL)

    def test_force_rewrites(self):
        """--force rewrites an image that is already there."""
        self.flasher = FakeFlasher(make_image(NEW_SERIAL), self.bridge)
        self.tool = make_tool(self.flasher, self.bridge)
        self.write(force=True)
        self.assertTrue(self.flasher.wrote)
        self.assertTrue(self.result.written)

    def test_invalid_serial_touches_nothing(self):
        """A bad serial is rejected before any device access."""
        self.assert_fails(hs.ExitCode.INVALID_SERIAL, serial="MAUIV12H123")
        self.assertEqual(self.flasher.calls, [])

    def test_invalid_file_touches_nothing(self):
        """A bad base image is rejected before any device access."""
        self.assert_fails(hs.ExitCode.INVALID_IMAGE, base_image=self.base[:100])
        self.assertEqual(self.flasher.calls, [])

    def test_invalid_flash_image_is_not_erased(self):
        """No usable image in the flash and no file: nothing is erased."""
        for image in (
            b"\xff" * H2H_IMAGE_SIZE,
            make_image(manufacturer="Google LLC Inc"),
        ):
            with self.subTest(image=image[:0x20].hex()):
                self.flasher.mem[:H2H_IMAGE_SIZE] = image
                self.flasher.calls.clear()
                self.assert_fails(hs.ExitCode.INVALID_IMAGE)
                self.assertFalse(self.flasher.wrote)
                self.assertEqual(self.result.flash_state, hs.FLASH_UNTOUCHED)

    def test_unsupported_firmware(self):
        """An MCU App without H2H RPC support fails before any access."""
        self.flasher.supported = {"h2h_read_begin"}
        self.assert_fails(hs.ExitCode.DEVICE_ERROR)
        self.assertNotIn("read", [c[0] for c in self.flasher.calls])

    def test_initial_read_fails(self):
        """An RPC failure before the write leaves the flash untouched."""
        self.flasher.fail_read_n = 1
        self.assert_fails(hs.ExitCode.DEVICE_ERROR)
        self.assertFalse(self.flasher.wrote)
        self.assertEqual(self.result.flash_state, hs.FLASH_UNTOUCHED)

    def test_flash_fails(self):
        """A failed write reports that the flash may be erased."""
        self.flasher.fail_flash = RpcError("h2h_finish", 0, "verify failed")
        err = self.assert_fails(hs.ExitCode.FLASH_FAILED)
        self.assertIn("--file", str(err))
        self.assertEqual(self.result.flash_state, hs.FLASH_UNKNOWN)
        self.assertIsNone(self.result.flash_serial)
        self.assertFalse(self.result.written)
        self.assertEqual(self.result.write_attempts, 2)
        self.assertIn("attempt 2", str(err))

    def test_flash_retried_once(self):
        """A failed write is retried once from the image in memory."""
        self.flasher.fail_flash = RpcTransportError("h2h_write: no response")
        self.flasher.fail_flash_times = 1
        with self.assertLogs(hs.logger, "WARNING"):
            self.write()
        self.assertEqual(self.result.write_attempts, 2)
        self.assertEqual(self.result.flash_state, hs.FLASH_WRITTEN)
        self.assertEqual(self.result.usb_serial, NEW_SERIAL)
        self.assertEqual([c[0] for c in self.flasher.calls].count("flash"), 2)

    def test_interrupted_write_clears_flash_serial(self):
        """Ctrl-C during the write leaves no stale serial in the result."""
        self.flasher.flash = mock.Mock(side_effect=KeyboardInterrupt)
        with self.assertRaises(KeyboardInterrupt):
            self.write()
        self.assertEqual(self.result.flash_state, hs.FLASH_UNKNOWN)
        self.assertIsNone(self.result.flash_serial)
        self.assertFalse(self.result.to_dict()["flash_serial_valid"])

    def test_vanished_old_bridge_is_not_reported(self):
        """An old enumeration that disappears is BRIDGE_NOT_FOUND, not a
        mismatch with the old iSerial."""
        polls = []

        def scan():
            polls.append(1)
            return (
                [hs.UsbBridge(H2H_USB, hs.IMAGE_PID, OLD_SERIAL)]
                if len(polls) == 1
                else []
            )

        self.tool._scan = scan  # pylint: disable=protected-access
        self.assert_fails(hs.ExitCode.BRIDGE_NOT_FOUND)
        self.assertIsNone(self.result.usb_serial)

    def test_settle_delay_not_counted_in_timeout(self):
        """--reenum-timeout counts from the end of the settle delay."""
        scan = self.bridge.scan
        polls = []

        def slow_scan():
            polls.append(1)
            return scan() if len(polls) >= 3 else []

        self.tool = make_tool(self.flasher, self.bridge, reenum_timeout_s=0.5)
        self.tool._scan = slow_scan  # pylint: disable=protected-access
        self.write()
        self.assertEqual(self.result.usb_serial, NEW_SERIAL)

    def test_readback_mismatch(self):
        """A read-back that differs from the image fails the write."""
        self.flasher.corrupt_after_write = True
        err = self.assert_fails(hs.ExitCode.VERIFY_FAILED)
        self.assertIn("0x0200", str(err))
        self.assertFalse(self.result.readback_ok)
        self.assertEqual(self.result.flash_state, hs.FLASH_UNKNOWN)

    def test_readback_fails(self):
        """A failed read-back is a verify failure (the MCU verified the CRC)."""
        self.flasher.fail_read_n = 2
        self.assert_fails(hs.ExitCode.VERIFY_FAILED)
        self.assertEqual(self.result.flash_state, hs.FLASH_WRITTEN)

    def test_bridge_does_not_enumerate(self):
        """The bridge has to come back on USB after its reset."""
        self.bridge.enumerates = False
        self.assert_fails(hs.ExitCode.BRIDGE_NOT_FOUND)
        self.assertEqual(self.result.flash_state, hs.FLASH_WRITTEN)

    def test_usb_serial_mismatch(self):
        """The bridge has to enumerate with the new iSerial."""
        self.bridge.serial_override = OLD_SERIAL
        self.assert_fails(hs.ExitCode.USB_SERIAL_MISMATCH)
        self.assertEqual(self.result.usb_serial, OLD_SERIAL)

    def test_reenumeration_is_awaited(self):
        """The old enumeration is ignored until the new iSerial shows up."""
        boots = []
        scan = self.bridge.scan

        def slow_scan():
            boots.append(1)
            if len(boots) < 5:
                return [hs.UsbBridge(H2H_USB, hs.IMAGE_PID, OLD_SERIAL)]
            return scan()

        self.tool._scan = slow_scan  # pylint: disable=protected-access
        self.write()
        self.assertEqual(self.result.usb_serial, NEW_SERIAL)
        self.assertEqual(len(boots), 5)


class TestWriteSectors(WriteFixture):
    """H2hSerialTool.write(): which part of the flash is rewritten."""

    def test_serial_change_rewrites_only_sector_0(self):
        """Only the first 4 KB sector is erased and rewritten."""
        sector = hs.H2H_SECTOR_SIZE
        self.flasher.mem[H2H_IMAGE_SIZE:] = b"\xa5" * (FLASH_SIZE - H2H_IMAGE_SIZE)
        before = bytes(self.flasher.mem)
        self.write()
        self.assertIn(("flash", sector, 0), self.flasher.calls)
        self.assertEqual(bytes(self.flasher.mem[sector:]), before[sector:])
        self.assertNotEqual(bytes(self.flasher.mem[:sector]), before[:sector])

    def test_old_firmware_erases_the_chip(self):
        """Without h2h_begin_range the whole chip is erased (fallback)."""
        self.flasher.supported.discard("h2h_begin_range")
        with self.assertLogs(hs.logger, "WARNING") as logs:
            self.write()
        self.assertIn("h2h_begin_range", "\n".join(logs.output))
        self.assertIn(("flash", H2H_IMAGE_SIZE, None), self.flasher.calls)
        self.assertEqual(self.result.write_mode, hs.WRITE_CHIP)
        self.assertEqual(self.result.usb_serial, NEW_SERIAL)

    def test_force_unchanged_rewrites_only_sector_0(self):
        """--force with nothing to change rewrites sector 0, not the chip."""
        self.flasher = FakeFlasher(make_image(NEW_SERIAL), self.bridge)
        self.tool = make_tool(self.flasher, self.bridge)
        self.write(force=True)
        self.assertIn(("flash", hs.H2H_SECTOR_SIZE, 0), self.flasher.calls)
        self.assertEqual(self.result.write_mode, hs.WRITE_SECTORS)
        self.assertEqual(self.result.write_range, "0x0000-0x0fff")

    def test_full_erase(self):
        """full_erase erases the whole chip even if sectors are supported."""
        self.write(full_erase=True)
        self.assertIn(("flash", H2H_IMAGE_SIZE, None), self.flasher.calls)
        self.assertEqual(self.result.write_mode, hs.WRITE_CHIP)

    def test_file_changing_sector_1(self):
        """A base image that also differs in sector 1 rewrites sectors 0-1."""
        other = bytearray(self.base)
        other[0x1100] ^= 0xFF
        self.write(base_image=bytes(other))
        self.assertIn(("flash", 2 * hs.H2H_SECTOR_SIZE, 0), self.flasher.calls)
        self.assertEqual(self.result.write_range, "0x0000-0x1fff")

    def test_file_changing_last_partial_sector(self):
        """Changes in the last, partial sector need the whole chip erased."""
        other = bytearray(self.base)
        other[H2H_IMAGE_SIZE - 1] ^= 0xFF
        self.write(base_image=bytes(other))
        self.assertIn(("flash", H2H_IMAGE_SIZE, None), self.flasher.calls)
        self.assertEqual(self.result.write_mode, hs.WRITE_CHIP)

    def test_sector_write_fails(self):
        """A failed sector write leaves the flash state unknown."""
        self.flasher.fail_flash = RpcTransportError("h2h_write: no response")
        self.assert_fails(hs.ExitCode.FLASH_FAILED)
        self.assertEqual(self.result.write_mode, hs.WRITE_SECTORS)
        self.assertEqual(self.result.flash_state, hs.FLASH_UNKNOWN)


class TestRead(unittest.TestCase):
    """H2hSerialTool.read()."""

    def setUp(self):
        self.image = make_image(NEW_SERIAL)
        self.bridge = FakeBridge(self.image)
        self.flasher = FakeFlasher(self.image, self.bridge)
        self.tool = make_tool(self.flasher, self.bridge)
        self.result = hs.H2hSerialResult("read")

    def test_read(self):
        """Reads the serial from the flash and checks it on USB."""
        self.tool.read(self.result)
        self.assertEqual(self.result.flash_serial, NEW_SERIAL)
        self.assertEqual(self.result.usb_serial, NEW_SERIAL)
        self.assertFalse(self.flasher.wrote)
        self.assertTrue(self.result.to_dict()["flash_serial_valid"])

    def test_read_usb_only(self):
        """--usb-only does not touch the flash."""
        self.tool.read(self.result, usb_only=True)
        self.assertEqual(self.result.usb_serial, NEW_SERIAL)
        self.assertIsNone(self.result.flash_serial)
        self.assertEqual(self.flasher.calls, [])

    def test_read_usb_only_no_bridge(self):
        """--usb-only without a bridge on USB."""
        self.bridge.enumerates = False
        with self.assertRaises(hs.H2hSerialError) as ctx:
            self.tool.read(self.result, usb_only=True)
        self.assertEqual(ctx.exception.code, hs.ExitCode.BRIDGE_NOT_FOUND)

    def test_read_blank(self):
        """A blank flash holds no serial."""
        self.flasher.mem[:] = b"\xff" * FLASH_SIZE
        with self.assertRaises(hs.H2hSerialError) as ctx:
            self.tool.read(self.result)
        self.assertEqual(ctx.exception.code, hs.ExitCode.INVALID_IMAGE)

    def test_read_mismatch(self):
        """Flash and USB disagree."""
        self.bridge.serial_override = OLD_SERIAL
        with self.assertRaises(hs.H2hSerialError) as ctx:
            self.tool.read(self.result)
        self.assertEqual(ctx.exception.code, hs.ExitCode.USB_SERIAL_MISMATCH)


if __name__ == "__main__":
    unittest.main()
