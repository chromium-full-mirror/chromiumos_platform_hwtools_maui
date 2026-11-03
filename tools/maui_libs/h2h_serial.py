# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Unit serial number of the Prolific PL27A1 H2H bridge.

In mass production every Maui v1.2 board gets the same pre-burned PL27A1
image; the unit serial number is written later, after SMT, by the test
station. The serial is the USB iSerial string descriptor inside the image, so
changing it means patching the image and flashing it again.

H2hSerialTool.write():
  1. Validates the serial format and the base image (a file, or the image
     currently in the flash). Nothing is erased if either is wrong.
  2. Patches the serial string descriptor into a copy of the image; every
     other byte stays as it is.
  3. Flashes it over RPC and the MCU verifies it with a CRC-32 (see
     maui_libs.h2h). Only the 4 KB sector(s) that differ from the flash are
     erased and rewritten (for a serial change: the first sector) if the MCU
     App supports h2h_begin_range; otherwise the whole chip is erased and the
     whole image is written. A failed write is retried once from the image
     in memory.
  4. Reads the image back and compares it byte for byte. The MCU holds the
     PL27A1 in reset during the read and releases it at the end, which resets
     the bridge so that it boots the new image.
  5. Waits for the bridge to enumerate on USB with the new iSerial. The
     bridge must sit on the same physical hub as the board's MCU (see
     pair_bridge), so several boards can be connected at once.

H2hSerialTool.read() reads the serial from the flash (which also resets the
bridge) and from USB. Errors are raised as H2hSerialError, whose code is the
process exit code of `maui-flash h2h-serial` (see ExitCode).
"""

import dataclasses
import enum
import hashlib
import logging
import os
import re
import struct
import time
from typing import Callable
from typing import List
from typing import Optional
from typing import Sequence

from maui_libs.h2h import H2H_IMAGE_SIZE
from maui_libs.h2h import H2H_SECTOR_SIZE
from maui_libs.h2h import ProgressCallback
from maui_libs.rpc import RpcError
from maui_libs.rpc import RpcTransportError


logger = logging.getLogger(__name__)

# Unit serial number (housing label, H2H iSerial, MCU NVS):
#   MAUIV <2-digit HW version> <batch letter> <YYMM> <4 digits>
# e.g. MAUIV12H26010001 = v1.2, batch H, January 2026, unit 0001.
SERIAL_FORMAT = "MAUIV + 2-digit version + batch letter + YYMM + 4 digits"
SERIAL_EXAMPLE = "MAUIV12H26010001"
SERIAL_RE = re.compile(
    r"MAUIV(?P<version>[0-9]{2})(?P<batch>[A-Z])"
    r"(?P<year>[0-9]{2})(?P<month>0[1-9]|1[0-2])(?P<unit>[0-9]{4})"
)

GOOGLE_VID = 0x18D1
# USB IDs of the H2H bridge; a raw image enumerates as IMAGE_PID.
H2H_PIDS = (0x506D, 0x5084)

# PL27A1 image layout: a header (signature, VID, PID, bcdDevice), then packed
# USB string descriptors from 0x10 (LANGID, manufacturer, product, serial),
# zero-filled up to STRING_REGION_END. The hardware configuration that
# follows (up to 0x1FF) and the Prolific firmware (0x200 to the end) must
# never change. The image has no checksum.
IMAGE_SIGNATURE = b"\x7b\x06"
IMAGE_PID = 0x506D
IMAGE_BCD_DEVICE = 0x0200
STRING_TABLE_OFFSET = 0x10
SERIAL_DESCRIPTOR_OFFSET = 0x38
STRING_REGION_END = 0xF9
USB_DT_STRING = 0x03
LANGID_EN_US = 0x0409
EXPECTED_MANUFACTURER = "Google LLC"
EXPECTED_PRODUCT = "ADB v1"

SYSFS_USB_DEVICES = "/sys/bus/usb/devices"
# The bridge is back about 0.3 s after its reset. USB is only polled after
# REENUM_SETTLE_S so that the enumeration from before the reset cannot be
# mistaken for the bridge; it then has REENUM_TIMEOUT_S to show up.
REENUM_SETTLE_S = 1.0
REENUM_TIMEOUT_S = 10.0
REENUM_POLL_S = 0.1

# A failed flash write is retried this many times in total.
WRITE_ATTEMPTS = 2

# result.flash_state: what the H2H flash holds at the end of the run.
FLASH_UNTOUCHED = "untouched"  # Not erased: the previous image is intact.
FLASH_WRITTEN = "written"  # The new image is programmed and verified.
# A write started and did not complete (or the read-back differs): the flash
# may be erased or partial, and the bridge may not boot until it is flashed.
FLASH_UNKNOWN = "unknown"

# result.write_mode: how the flash was written.
WRITE_NONE = "none"  # Not written (already up to date, or failed before).
WRITE_SECTORS = "sectors"  # Only the sectors that differ (h2h_begin_range).
WRITE_CHIP = "chip"  # Whole chip erased, whole image written (h2h_begin).

RESULT_SCHEMA = "maui.h2h_serial.v1"


class ExitCode(enum.IntEnum):
    """Exit codes of `maui-flash h2h-serial` (stable interface)."""

    OK = 0
    ERROR = 1  # Unexpected error.
    USAGE = 2  # Bad command line (argparse).
    INVALID_SERIAL = 3  # Bad serial format; nothing touched.
    INVALID_IMAGE = 4  # Base image unusable; nothing erased.
    DEVICE_ERROR = 5  # No RPC port, RPC failure, old MCU firmware.
    FLASH_FAILED = 6  # Erase / program / MCU CRC-32 verify failed.
    VERIFY_FAILED = 7  # Read-back failed or differs from the image.
    BRIDGE_NOT_FOUND = 8  # The bridge did not enumerate after its reset.
    USB_SERIAL_MISMATCH = 9  # The bridge enumerated with another iSerial.
    INTERRUPTED = 130  # Ctrl-C / SIGINT.


class H2hSerialError(RuntimeError):
    """A failed step; code is the exit code."""

    def __init__(self, code: ExitCode, message: str):
        super().__init__(message)
        self.code = code


# ---------------------------------------------------------------------------
# Serial number and image
# ---------------------------------------------------------------------------


def is_valid_serial(serial: Optional[str]) -> bool:
    """True for a well-formed Maui unit serial number (see SERIAL_RE)."""
    return bool(serial) and SERIAL_RE.fullmatch(serial) is not None


def validate_serial(serial: Optional[str]):
    """Raises H2hSerialError(INVALID_SERIAL) for a malformed serial."""
    if not is_valid_serial(serial):
        raise H2hSerialError(
            ExitCode.INVALID_SERIAL,
            f"{serial!r} is not a valid Maui serial number (expected "
            f"{SERIAL_FORMAT}, e.g. {SERIAL_EXAMPLE}).",
        )


@dataclasses.dataclass
class H2hImage:
    """The header and USB strings of a PL27A1 image."""

    vid: int
    pid: int
    bcd_device: int
    manufacturer: str
    product: str
    serial: str
    warnings: List[str] = dataclasses.field(default_factory=list)


def _string_descriptors(data: bytes, count: int = 4) -> List[bytes]:
    """Returns the bodies of the first `count` packed USB string descriptors."""
    bodies = []
    offset = STRING_TABLE_OFFSET
    for index in range(count):
        if offset + 2 > len(data):
            raise ValueError(f"string descriptor #{index} at 0x{offset:02x} missing")
        length, dtype = data[offset], data[offset + 1]
        if (
            dtype != USB_DT_STRING
            or length < 2
            or length % 2
            or offset + length > len(data)
        ):
            raise ValueError(
                f"bad string descriptor #{index} at 0x{offset:02x} "
                f"(bLength 0x{length:02x}, bDescriptorType 0x{dtype:02x})"
            )
        bodies.append(data[offset + 2 : offset + length])
        offset += length
    return bodies


def decode_image(data: bytes) -> H2hImage:
    """Decodes the PL27A1 image header and USB string descriptors.

    Raises ValueError for a blank, unreadable or corrupt image. Deviations
    that do not prevent decoding are listed in H2hImage.warnings.
    """
    if len(data) < STRING_TABLE_OFFSET + 2:
        raise ValueError(f"only {len(data)} bytes")
    if all(b == 0xFF for b in data):
        raise ValueError("all bytes are 0xFF (blank flash)")
    if all(b == 0x00 for b in data):
        raise ValueError("all bytes are 0x00")
    signature, vid, pid, bcd = struct.unpack_from("<2sHHH", data, 0)
    if vid != GOOGLE_VID or pid not in H2H_PIDS:
        raise ValueError(f"unexpected USB ID {vid:04x}:{pid:04x} in the header")
    try:
        strings = _string_descriptors(data)
        manufacturer, product, serial = (s.decode("utf-16-le") for s in strings[1:])
    except (ValueError, UnicodeDecodeError) as e:
        raise ValueError(f"bad USB string descriptors ({e})") from e
    image = H2hImage(vid, pid, bcd, manufacturer, product, serial)
    if signature != IMAGE_SIGNATURE:
        image.warnings.append(
            f"signature {signature.hex()} (expected {IMAGE_SIGNATURE.hex()})"
        )
    if bcd != IMAGE_BCD_DEVICE:
        image.warnings.append(f"bcdDevice {bcd:04x} (expected {IMAGE_BCD_DEVICE:04x})")
    if strings[0][:2] != struct.pack("<H", LANGID_EN_US):
        image.warnings.append(f"LANGID {strings[0].hex()} (expected 0904)")
    for name, value, expected in (
        ("manufacturer", manufacturer, EXPECTED_MANUFACTURER),
        ("product", product, EXPECTED_PRODUCT),
    ):
        if value != expected:
            image.warnings.append(f"{name} {value!r} (expected {expected!r})")
    return image


def _serial_descriptor_end(data: bytes) -> int:
    """End offset of the serial descriptor; ValueError if not patchable."""
    bodies = _string_descriptors(data)
    offset = STRING_TABLE_OFFSET + sum(len(b) + 2 for b in bodies[:3])
    if offset != SERIAL_DESCRIPTOR_OFFSET:
        raise ValueError(
            f"the serial descriptor is at 0x{offset:02x} "
            f"(expected 0x{SERIAL_DESCRIPTOR_OFFSET:02x})"
        )
    end = offset + 2 + len(bodies[3])
    if end > STRING_REGION_END:
        raise ValueError(
            f"the serial descriptor ends at 0x{end:02x}, past 0x{STRING_REGION_END:02x}"
        )
    if any(data[end:STRING_REGION_END]):
        raise ValueError(
            f"0x{end:02x}-0x{STRING_REGION_END - 1:02x} after the serial "
            "descriptor is not zero-filled"
        )
    return end


def check_base_image(data: bytes) -> H2hImage:
    """Checks that the serial of PL27A1 image `data` can be replaced.

    That is a whole raw PL27A1 image (H2H_IMAGE_SIZE bytes, Prolific
    signature, 18d1:506d) with the serial as the last string descriptor at
    SERIAL_DESCRIPTOR_OFFSET. Returns the decoded image; raises
    H2hSerialError(INVALID_IMAGE) otherwise.
    """

    def invalid(why: str) -> H2hSerialError:
        return H2hSerialError(
            ExitCode.INVALID_IMAGE, f"Not a usable PL27A1 H2H image: {why}."
        )

    if len(data) != H2H_IMAGE_SIZE:
        raise invalid(f"{len(data)} bytes, expected {H2H_IMAGE_SIZE}")
    try:
        image = decode_image(data)
        _serial_descriptor_end(data)
    except ValueError as e:
        raise invalid(str(e)) from e
    if data[: len(IMAGE_SIGNATURE)] != IMAGE_SIGNATURE:
        raise invalid(f"signature {data[:2].hex()}, expected {IMAGE_SIGNATURE.hex()}")
    if image.pid != IMAGE_PID:
        raise invalid(
            f"USB ID {image.vid:04x}:{image.pid:04x}, expected "
            f"{GOOGLE_VID:04x}:{IMAGE_PID:04x}"
        )
    return image


def patch_serial(data: bytes, serial: str) -> bytes:
    """Returns a copy of image `data` with the USB serial number `serial`.

    Only the serial string descriptor (and the zero fill after it) changes.
    Raises H2hSerialError (INVALID_SERIAL or INVALID_IMAGE).
    """
    validate_serial(serial)
    check_base_image(data)
    old_end = _serial_descriptor_end(data)
    encoded = serial.encode("utf-16-le")
    descriptor = bytes([2 + len(encoded), USB_DT_STRING]) + encoded
    new_end = SERIAL_DESCRIPTOR_OFFSET + len(descriptor)
    if new_end > STRING_REGION_END:
        raise H2hSerialError(
            ExitCode.INVALID_SERIAL, f"{serial!r} does not fit in the image."
        )
    out = bytearray(data)
    out[SERIAL_DESCRIPTOR_OFFSET:old_end] = bytes(old_end - SERIAL_DESCRIPTOR_OFFSET)
    out[SERIAL_DESCRIPTOR_OFFSET:new_end] = descriptor
    patched = bytes(out)

    # Never flash anything but the intended change.
    changed = [i for i, (a, b) in enumerate(zip(data, patched)) if a != b]
    region_end = max(old_end, new_end)
    if (
        len(patched) != len(data)
        or any(not SERIAL_DESCRIPTOR_OFFSET <= i < region_end for i in changed)
        or decode_image(patched).serial != serial
    ):
        raise H2hSerialError(
            ExitCode.INVALID_IMAGE, "The patched image does not decode to the serial."
        )
    return patched


def changed_sectors(old: bytes, new: bytes) -> Optional[range]:
    """The sector-aligned flash range that covers every byte that differs.

    Returns range(start, end) with start and end multiples of
    H2H_SECTOR_SIZE, or None if nothing differs or the range would reach
    past the end of the image (erasing that last, partial sector would also
    clear flash bytes beyond the image, which are not rewritten).
    """
    if len(old) != len(new):
        raise ValueError("images of different sizes")
    diffs = [i for i, (a, b) in enumerate(zip(old, new)) if a != b]
    if not diffs:
        return None
    start = diffs[0] // H2H_SECTOR_SIZE * H2H_SECTOR_SIZE
    end = (diffs[-1] // H2H_SECTOR_SIZE + 1) * H2H_SECTOR_SIZE
    return range(start, end) if end <= len(new) else None


# ---------------------------------------------------------------------------
# The bridge on USB
# ---------------------------------------------------------------------------


@dataclasses.dataclass
class UsbBridge:
    """An H2H bridge as the host sees it."""

    sys_path: str
    pid: int
    serial: str
    product: str = ""
    devnum: str = ""


def _read_attr(directory: str, name: str) -> str:
    try:
        with open(
            os.path.join(directory, name), "r", encoding="utf-8", errors="replace"
        ) as f:
            return f.read().strip()
    except OSError:
        return ""


def scan_bridges(root: str = SYSFS_USB_DEVICES) -> List[UsbBridge]:
    """Returns every H2H bridge on the USB bus (from sysfs)."""
    try:
        names = sorted(os.listdir(root))
    except OSError:
        return []
    bridges = []
    for name in names:
        if ":" in name:  # An interface, not a device.
            continue
        directory = os.path.join(root, name)
        try:
            vid = int(_read_attr(directory, "idVendor"), 16)
            pid = int(_read_attr(directory, "idProduct"), 16)
        except ValueError:
            continue
        if vid == GOOGLE_VID and pid in H2H_PIDS:
            bridges.append(
                UsbBridge(
                    sys_path=os.path.realpath(directory),
                    pid=pid,
                    serial=_read_attr(directory, "serial"),
                    product=_read_attr(directory, "product"),
                    devnum=_read_attr(directory, "devnum"),
                )
            )
    return bridges


def usb_device_path(location: Optional[str], root: str = SYSFS_USB_DEVICES) -> str:
    """sysfs path of the USB device of a pyserial port location ('1-3.2:1.2')."""
    if not location:
        return ""
    return os.path.realpath(os.path.join(root, location.split(":", 1)[0]))


def _port_chain(path: str) -> str:
    """'.../usb1/1-2/1-2.1' -> '2.1': the hub port chain without the bus."""
    name = os.path.basename(path)
    return name.split("-", 1)[1] if "-" in name else name


def _controller(path: str) -> str:
    """The host controller of a USB device: the sysfs path above 'usbN'."""
    parts = path.split(os.sep)
    for i, part in enumerate(parts):
        if re.fullmatch(r"usb[0-9]+", part):
            return os.sep.join(parts[:i])
    return ""


def _upstream_port(dev_path: str) -> str:
    """sysfs directory of the hub port that the USB device is plugged into.

    '.../usb1/1-2/1-2.3' -> '.../usb1/1-2/1-2:1.0/1-2-port3'
    '.../usb1/1-2'       -> '.../usb1/1-0:1.0/usb1-port2'
    Returns '' for a root hub or an unexpected name.
    """
    m = re.fullmatch(r"([0-9]+)-([0-9.]+)", os.path.basename(dev_path))
    if not m:
        return ""
    bus, chain = m.groups()
    port = chain.rsplit(".", 1)[-1]
    parent = os.path.dirname(dev_path)
    hub = os.path.basename(parent)
    interface = f"{bus}-0:1.0" if hub == f"usb{bus}" else f"{hub}:1.0"
    return os.path.join(parent, interface, f"{hub}-port{port}")


def same_hub(hub_a: str, hub_b: str) -> bool:
    """True if the sysfs USB devices hub_a and hub_b are one physical hub.

    That is the same device, or the USB 2 and USB 3 halves of one USB 3 hub
    (which sit on different buses). The halves are matched through the
    kernel's 'peer' link between the USB 2 and USB 3 ports of one connector;
    without peer links, by the same host controller and hub port chain.
    """
    if hub_a == hub_b:
        return True
    port_a, port_b = _upstream_port(hub_a), _upstream_port(hub_b)
    if port_a and port_b:
        for port, other in ((port_a, port_b), (port_b, port_a)):
            peer = os.path.join(port, "peer")
            if os.path.exists(peer):
                return os.path.realpath(peer) == os.path.realpath(other)
    controller = _controller(hub_a)
    return (
        bool(controller)
        and controller == _controller(hub_b)
        and _port_chain(hub_a) == _port_chain(hub_b)
    )


def pair_bridge(
    mcu_usb_path: str, bridges: Sequence[UsbBridge], board_count: int
) -> Optional[UsbBridge]:
    """Returns the bridge on the same board as the MCU at `mcu_usb_path`.

    The bridge sits on the same on-board hub as the MCU (see same_hub).
    Several matching bridges are ambiguous and return None. A lone bridge is
    accepted without a topology match only when `board_count` (the number of
    Maui boards connected, in any mode) is at most one.
    """
    if mcu_usb_path:
        hub = os.path.dirname(mcu_usb_path)
        matches = [b for b in bridges if same_hub(hub, os.path.dirname(b.sys_path))]
        if len(matches) == 1:
            return matches[0]
        if matches:
            logger.warning(
                "Several H2H bridges match the MCU at %s: %s",
                mcu_usb_path,
                ", ".join(b.sys_path for b in matches),
            )
            return None
    if len(bridges) == 1 and board_count <= 1:
        return bridges[0]
    return None


# ---------------------------------------------------------------------------
# Read and write
# ---------------------------------------------------------------------------


@dataclasses.dataclass
class H2hSerialResult:  # pylint: disable=too-many-instance-attributes
    """Outcome of one `maui-flash h2h-serial` run (the --json output)."""

    operation: str  # "read" or "write".
    success: bool = False
    exit_code: int = int(ExitCode.ERROR)
    message: str = ""
    rpc_port: str = ""
    mcu_usb_serial: str = ""
    jedec_id: str = ""
    flash_size: int = 0
    # Serial in the flash at the end of the run, if known.
    flash_serial: Optional[str] = None
    # write: the serial in the flash before the write.
    previous_serial: Optional[str] = None
    requested_serial: Optional[str] = None
    image_source: Optional[str] = None  # "file" or "flash".
    image_file: Optional[str] = None
    image_sha256: Optional[str] = None  # Of the image written (or to write).
    written: bool = False
    write_mode: str = WRITE_NONE
    write_attempts: int = 0
    # Flash range erased and rewritten ("0x0000-0x0fff"), if written.
    write_range: Optional[str] = None
    flash_state: str = FLASH_UNTOUCHED
    readback_ok: Optional[bool] = None
    usb_serial: Optional[str] = None
    usb_pid: Optional[str] = None
    usb_path: Optional[str] = None
    warnings: List[str] = dataclasses.field(default_factory=list)

    def fail(self, code: ExitCode, message: str):
        """Records a failure."""
        self.success = False
        self.exit_code = int(code)
        self.message = message

    def to_dict(self) -> dict:
        """The JSON object printed by --json."""
        out = {"schema": RESULT_SCHEMA}
        out.update(dataclasses.asdict(self))
        out["flash_serial_valid"] = is_valid_serial(self.flash_serial)
        return out


class H2hSerialTool:  # pylint: disable=too-many-instance-attributes
    """Reads and writes the H2H serial through an H2hFlasher.

    Args:
        flasher: maui_libs.h2h.H2hFlasher (or a fake) on the board's RPC port.
        mcu_usb_path: sysfs path of the board's MCU USB device, to find its
            bridge among several boards.
        board_count: number of Maui boards (RPC ports) connected.
    """

    def __init__(  # pylint: disable=too-many-arguments
        self,
        flasher,
        *,
        mcu_usb_path: str = "",
        board_count: int = 1,
        scan: Optional[Callable[[], List[UsbBridge]]] = None,
        sleep: Optional[Callable[[float], None]] = None,
        clock: Optional[Callable[[], float]] = None,
        reenum_timeout_s: float = REENUM_TIMEOUT_S,
        progress: Optional[ProgressCallback] = None,
    ):
        self._flasher = flasher
        self._mcu_usb_path = mcu_usb_path
        self._board_count = board_count
        # None: scan_bridges, looked up at call time.
        self._scan = scan
        self._sleep = sleep or time.sleep
        self._clock = clock or time.monotonic
        self._reenum_timeout_s = reenum_timeout_s
        self._progress = progress

    # -- operations --------------------------------------------------------

    def read(self, result: H2hSerialResult, usb_only: bool = False):
        """Reads the serial from the flash and from USB into `result`.

        Reading the flash resets the bridge (it drops off USB for a moment).
        With `usb_only` the flash is not touched. Raises H2hSerialError.
        """
        if usb_only:
            bridge = self._wait_bridge(expected=None, settle=False, timeout_s=0)
            if bridge is None:
                raise H2hSerialError(
                    ExitCode.BRIDGE_NOT_FOUND,
                    "No H2H bridge (18d1:506d/5084) of this board on USB.",
                )
            self._record_bridge(result, bridge)
            result.message = f"H2H iSerial on USB: {bridge.serial}"
            return

        self._check_supported("h2h_read_begin")
        data = self._read_flash(result, H2H_IMAGE_SIZE)
        try:
            image = decode_image(data)
        except ValueError as e:
            raise H2hSerialError(
                ExitCode.INVALID_IMAGE,
                f"The H2H flash does not hold a PL27A1 image ({e}).",
            ) from e
        result.flash_serial = image.serial
        result.warnings += [f"Flash image: {w}" for w in image.warnings]
        self._confirm_bridge(result, image.serial, after="its reset")
        result.message = (
            f"H2H serial {image.serial} in the flash and on USB"
            + ("" if is_valid_serial(image.serial) else f" (not {SERIAL_FORMAT})")
            + "."
        )

    def write(  # pylint: disable=too-many-arguments
        self,
        result: H2hSerialResult,
        serial: str,
        *,
        base_image: Optional[bytes] = None,
        force: bool = False,
        full_erase: bool = False,
    ):
        """Writes `serial` to the H2H flash (see the module docstring).

        `base_image` is the image to patch; None patches the image currently
        in the flash. The flash is only rewritten if its content differs from
        the patched image (or with `force`). Only the sectors that differ are
        rewritten when the firmware supports it, unless `full_erase`.
        Raises H2hSerialError.
        """
        result.requested_serial = serial
        validate_serial(serial)
        if base_image is not None:
            check_base_image(base_image)
            result.image_source = result.image_source or "file"
        self._check_supported("h2h_begin")
        self._check_supported("h2h_read_begin")

        current = self._read_flash(result, H2H_IMAGE_SIZE)
        try:
            result.previous_serial = decode_image(current).serial
            result.flash_serial = result.previous_serial
        except ValueError as e:
            if base_image is None:
                raise H2hSerialError(
                    ExitCode.INVALID_IMAGE,
                    f"The H2H flash does not hold a PL27A1 image ({e}): pass the "
                    "image with --file. Nothing was erased.",
                ) from e
            logger.warning("The H2H flash holds no PL27A1 image (%s).", e)
        if base_image is None:
            base_image = current
            result.image_source = "flash"

        image = patch_serial(base_image, serial)
        result.image_sha256 = hashlib.sha256(image).hexdigest()
        result.warnings += [f"Image: {w}" for w in decode_image(image).warnings]

        if image == current and not force:
            logger.info(
                "The H2H flash already holds this image with serial %s: not "
                "rewriting it (--force rewrites it).",
                serial,
            )
            after = "its reset (flash read)"
        else:
            logger.info(
                "Writing H2H serial %s (was %s, image from the %s)...",
                serial,
                result.previous_serial,
                result.image_source,
            )
            self._flash(result, image, self._plan(current, image, full_erase))
            self._read_back(result, image)
            after = "its reset"
        self._confirm_bridge(result, serial, after=after)
        result.message = (
            f"H2H serial {serial} written (was {result.previous_serial})"
            if result.written
            else f"The H2H flash already holds serial {serial} (not rewritten)"
        ) + f"; the bridge enumerated with iSerial {result.usb_serial}."

    # -- steps -------------------------------------------------------------

    def _check_supported(self, request: str):
        try:
            self._flasher.check_supported(request)
        except (RpcError, RpcTransportError, RuntimeError) as e:
            raise H2hSerialError(ExitCode.DEVICE_ERROR, str(e)) from e

    def _read_flash(self, result: H2hSerialResult, size: int) -> bytes:
        """Reads the image; the PL27A1 is reset while the MCU holds the bus."""
        try:
            read = self._flasher.read(size, progress=self._progress)
        except (RpcError, RpcTransportError, RuntimeError, ValueError) as e:
            raise H2hSerialError(
                ExitCode.DEVICE_ERROR, f"Could not read the H2H flash: {e}"
            ) from e
        result.jedec_id = f"0x{read.info.jedec_id:06X}"
        result.flash_size = read.info.flash_size
        if len(read.data) != size:
            raise H2hSerialError(
                ExitCode.DEVICE_ERROR,
                f"Read {len(read.data)} of {size} bytes of the H2H flash.",
            )
        return read.data

    def _plan(self, current: bytes, image: bytes, full_erase: bool) -> Optional[range]:
        """The sectors to rewrite, or None to erase the whole chip."""
        if full_erase:
            logger.info("Erasing the whole H2H flash (--full-erase).")
            return None
        if image == current:
            # --force with nothing to change: rewrite the serial's sector.
            sectors = range(0, H2H_SECTOR_SIZE)
        else:
            sectors = changed_sectors(current, image)
        if sectors is None:
            logger.info(
                "The changes reach the last, partial sector of the image: "
                "erasing the whole H2H flash."
            )
            return None
        if not self._flasher.supports("h2h_begin_range"):
            logger.warning(
                "The MCU App cannot rewrite single H2H flash sectors "
                "(no h2h_begin_range): erasing the whole chip. Update the MCU "
                "App to only rewrite the changed sector."
            )
            return None
        return sectors

    def _flash(self, result: H2hSerialResult, image: bytes, sectors: Optional[range]):
        # The firmware erases the flash (or the sectors) when the session
        # starts.
        result.flash_state = FLASH_UNKNOWN
        if sectors is None:
            result.write_mode = WRITE_CHIP
            result.write_range = f"0x0000-0x{len(image) - 1:04x} (chip erased)"
        else:
            result.write_mode = WRITE_SECTORS
            result.write_range = f"0x{sectors.start:04x}-0x{sectors.stop - 1:04x}"
        # From here on nothing is known about the flash content, also if the
        # write is interrupted (Ctrl-C) or fails unexpectedly.
        result.flash_serial = None
        logger.info("Writing H2H flash %s...", result.write_range)
        errors = []
        for attempt in range(1, WRITE_ATTEMPTS + 1):
            result.write_attempts = attempt
            try:
                if sectors is None:
                    self._flasher.flash(image, progress=self._progress)
                else:
                    self._flasher.flash(
                        image[sectors.start : sectors.stop],
                        progress=self._progress,
                        offset=sectors.start,
                    )
                break
            except (RpcError, RpcTransportError, RuntimeError) as e:
                errors.append(f"attempt {attempt}: {e}")
                if attempt < WRITE_ATTEMPTS:
                    # The patched image is still in memory: a new session
                    # erases again and rewrites it.
                    logger.warning("Writing the H2H flash failed (%s); retrying.", e)
        else:
            raise H2hSerialError(
                ExitCode.FLASH_FAILED,
                f"Writing the H2H flash failed ({'; '.join(errors)}). The flash "
                "may be erased or incomplete and the bridge may not boot until "
                "it is flashed again (the MCU stays reachable): run the command "
                "again with --file.",
            )
        result.written = True
        result.flash_state = FLASH_WRITTEN
        result.flash_serial = result.requested_serial

    def _read_back(self, result: H2hSerialResult, image: bytes):
        """Reads the image back and compares it; also resets the bridge."""
        try:
            data = self._flasher.read(len(image), progress=self._progress).data
        except (RpcError, RpcTransportError, RuntimeError, ValueError) as e:
            result.readback_ok = False
            raise H2hSerialError(
                ExitCode.VERIFY_FAILED,
                f"The image was written and verified by the MCU (CRC-32), but "
                f"reading it back failed: {e}. Power-cycle the board and read "
                "the serial again.",
            ) from e
        result.readback_ok = data == image
        if not result.readback_ok:
            result.flash_state = FLASH_UNKNOWN
            result.flash_serial = None
            diffs = [i for i, (a, b) in enumerate(zip(image, data)) if a != b]
            where = (
                f"{len(diffs)} byte(s) differ, first at 0x{diffs[0]:04x}"
                if diffs
                else f"read {len(data)} of {len(image)} bytes"
            )
            raise H2hSerialError(
                ExitCode.VERIFY_FAILED,
                f"The H2H flash read back does not match the image ({where}): "
                "write it again.",
            )

    def _wait_bridge(
        self, expected: Optional[str], settle: bool, timeout_s: float
    ) -> Optional[UsbBridge]:
        """Polls USB for the board's bridge (with iSerial `expected`).

        With `settle`, waits REENUM_SETTLE_S first and then polls for up to
        `timeout_s`. Returns the bridge as soon as it has the expected
        iSerial, else what the last poll found (a bridge with another
        iSerial, or None) after `timeout_s`.
        """
        if settle and REENUM_SETTLE_S > 0:
            self._sleep(REENUM_SETTLE_S)
        deadline = self._clock() + timeout_s
        while True:
            bridge = pair_bridge(
                self._mcu_usb_path, (self._scan or scan_bridges)(), self._board_count
            )
            if bridge is not None and expected in (None, bridge.serial):
                return bridge
            if self._clock() >= deadline:
                return bridge
            self._sleep(REENUM_POLL_S)

    @staticmethod
    def _record_bridge(result: H2hSerialResult, bridge: UsbBridge):
        result.usb_serial = bridge.serial
        result.usb_pid = f"{GOOGLE_VID:04x}:{bridge.pid:04x}"
        result.usb_path = bridge.sys_path

    def _confirm_bridge(self, result: H2hSerialResult, serial: str, after: str):
        """Waits for the bridge to enumerate with iSerial `serial`."""
        bridge = self._wait_bridge(serial, True, self._reenum_timeout_s)
        if bridge is None:
            raise H2hSerialError(
                ExitCode.BRIDGE_NOT_FOUND,
                f"The H2H bridge did not enumerate on USB within "
                f"{self._reenum_timeout_s:.0f} s of {after}: power-cycle the "
                "board and read the serial again.",
            )
        self._record_bridge(result, bridge)
        if bridge.serial != serial:
            raise H2hSerialError(
                ExitCode.USB_SERIAL_MISMATCH,
                f"The H2H bridge enumerated with iSerial {bridge.serial!r}, "
                f"expected {serial!r}.",
            )
        logger.info(
            "H2H bridge enumerated as %s with iSerial %s (%s).",
            result.usb_pid,
            bridge.serial,
            bridge.sys_path,
        )
