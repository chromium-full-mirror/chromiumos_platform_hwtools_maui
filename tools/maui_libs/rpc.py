# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Client for the Maui machine-to-machine RPC channel.

The IT82002 Application exposes a second USB CDC-ACM interface whose interface
string is "Maui RPC" (typically /dev/ttyACM1). It carries COBS-framed Protobuf
Envelopes defined in firmware/proto/maui_rpc.proto (go/maui-ite-design, "Host
Automation & Machine Interface").

RpcClient owns the port and a background reader thread that
  * matches Responses to pending calls by sequence number, and
  * dispatches unsolicited Events to registered callbacks.

Calls can be synchronous (call()) or pipelined (call_async() returns a
Future), which is what makes bulk transfers such as H2H flashing fast.

Domain-specific helpers (e.g. maui_libs.h2h.H2hFlasher) are thin layers on top
of RpcClient; new firmware features only need a new helper, not transport
changes.
"""

from concurrent.futures import Future
from concurrent.futures import TimeoutError as FutureTimeoutError
import itertools
import logging
import os
import threading
from typing import Callable
from typing import Dict
from typing import List
from typing import Optional

from maui_libs.proto import maui_rpc_pb2 as pb
from maui_libs.transport import MAUI_FINAL_PID
from maui_libs.transport import MAUI_FINAL_VID
from maui_libs.transport import MAUI_IT82002_APP_PID
from maui_libs.transport import MAUI_RPC_INTERFACE_NAME
import serial
import serial.tools.list_ports
from serial.tools.list_ports_common import ListPortInfo


logger = logging.getLogger(__name__)

RPC_INTERFACE_NAME = MAUI_RPC_INTERFACE_NAME
DEFAULT_TIMEOUT = 5.0

EventCallback = Callable[[pb.Event], None]


class RpcError(RuntimeError):
    """A request completed with a non-OK status."""

    def __init__(
        self, request_name: str, status: int, message: str, error_code: int = 0
    ):
        self.request_name = request_name
        self.status = status
        self.message = message
        # Negative Zephyr errno reported by the firmware, 0 if none.
        self.error_code = error_code
        status_name = pb.Status.Name(status) if status in pb.Status.values() else status
        super().__init__(f"{request_name}: {status_name}: {message or 'no details'}")


class RpcTransportError(RuntimeError):
    """The RPC link failed (port closed, device unplugged, timeout)."""


# ---------------------------------------------------------------------------
# COBS framing (0x00 delimiter)
# ---------------------------------------------------------------------------


def cobs_encode(data: bytes) -> bytes:
    """COBS-encodes data (without the trailing delimiter)."""
    out = bytearray()
    block = bytearray()
    for byte in data:
        if byte == 0:
            out.append(len(block) + 1)
            out += block
            block.clear()
        else:
            block.append(byte)
            if len(block) == 254:
                out.append(255)
                out += block
                block.clear()
    out.append(len(block) + 1)
    out += block
    return bytes(out)


def cobs_decode(data: bytes) -> bytes:
    """Decodes one COBS frame (without the delimiter). Raises ValueError."""
    out = bytearray()
    idx = 0
    while idx < len(data):
        code = data[idx]
        if code == 0:
            raise ValueError("zero byte inside COBS frame")
        end = idx + code
        if end > len(data):
            raise ValueError("truncated COBS frame")
        block = data[idx + 1 : end]
        if 0 in block:
            raise ValueError("zero byte inside COBS frame")
        out += block
        idx = end
        if code != 255 and idx < len(data):
            out.append(0)
    return bytes(out)


# ---------------------------------------------------------------------------
# Port discovery
# ---------------------------------------------------------------------------


def _usb_device_key(location: Optional[str]) -> Optional[str]:
    """'1-3.2:1.2' -> '1-3.2' (the USB device, without the interface)."""
    if not location:
        return None
    return location.split(":", 1)[0]


def list_rpc_ports() -> List[ListPortInfo]:
    """Returns every "Maui RPC" CDC-ACM port on the system."""
    return [
        p
        for p in serial.tools.list_ports.comports()
        if p.vid == MAUI_FINAL_VID
        and p.pid in (MAUI_FINAL_PID, MAUI_IT82002_APP_PID)
        and (p.interface or "") == RPC_INTERFACE_NAME
    ]


def find_rpc_port(
    serial_number: Optional[str] = None, console_port: Optional[str] = None
) -> str:
    """Finds the RPC port of a Maui device.

    Args:
        serial_number: USB serial number of the device, if known.
        console_port: The device's shell port; the RPC port on the same USB
            device is returned. Most precise when several devices share a
            serial number (e.g. unprovisioned boards).

    Raises:
        RpcTransportError: No (or no unique) matching port.
    """
    candidates = list_rpc_ports()

    if console_port:
        key = None
        # --port may be a udev symlink such as /dev/serial/by-id/...
        target = os.path.realpath(console_port)
        for p in serial.tools.list_ports.comports():
            if os.path.realpath(p.device) == target:
                key = _usb_device_key(p.location)
                break
        if key:
            same = [p for p in candidates if _usb_device_key(p.location) == key]
            if same:
                return same[0].device

    if serial_number:
        candidates = [p for p in candidates if p.serial_number == serial_number]

    if not candidates:
        raise RpcTransportError(
            f"No '{RPC_INTERFACE_NAME}' interface found. Is the device running "
            "an Application firmware with RPC support? If the OS does not "
            "report USB interface names (e.g. macOS), pass --rpc-port."
        )
    if len(candidates) > 1:
        raise RpcTransportError(
            "Multiple Maui RPC ports found ("
            + ", ".join(p.device for p in candidates)
            + "); select the device with --serial or --port."
        )
    return candidates[0].device


# ---------------------------------------------------------------------------
# Client
# ---------------------------------------------------------------------------


class RpcClient:  # pylint: disable=too-many-instance-attributes
    """Connection to one device's RPC channel.

    Usage:
        with RpcClient(find_rpc_port()) as rpc:
            info = rpc.ping()
    """

    def __init__(self, port: str):
        self.port = port
        self._ser: Optional[serial.Serial] = None
        self._reader: Optional[threading.Thread] = None
        self._running = False
        self._write_lock = threading.Lock()
        self._pending_lock = threading.Lock()
        self._pending: Dict[int, Future] = {}
        self._seq = itertools.count(1)
        self._event_lock = threading.Lock()
        self._event_cbs: Dict[str, List[EventCallback]] = {}
        self._supported: Optional[set] = None

    # -- lifecycle ---------------------------------------------------------

    def open(self):
        """Opens the port and starts the reader thread."""
        self._ser = serial.Serial(self.port, timeout=0.05, write_timeout=5.0)
        # Opening asserts DTR, which also enables firmware Events.
        self._ser.reset_input_buffer()
        self._running = True
        self._reader = threading.Thread(
            target=self._read_loop, name="maui-rpc-reader", daemon=True
        )
        self._reader.start()
        # A lone delimiter discards any partial frame left in the firmware.
        self._write(b"\x00")
        logger.debug("RPC connected on %s", self.port)

    def close(self):
        """Stops the reader and closes the port. Pending calls fail."""
        self._running = False
        if self._reader:
            self._reader.join(timeout=1.0)
        if self._ser:
            self._ser.close()
        self._fail_pending(RpcTransportError("RPC connection closed"))

    def __enter__(self):
        self.open()
        return self

    def __exit__(self, *exc):
        self.close()

    # -- events ------------------------------------------------------------

    def on_event(self, name: str, callback: EventCallback):
        """Registers callback for Event oneof field name (e.g. 'h2h_progress').

        Callbacks run on the reader thread, so they must be quick and must not
        make RPC calls themselves (the reader could not deliver the response).
        Hand work off to another thread if needed.
        """
        with self._event_lock:
            self._event_cbs.setdefault(name, []).append(callback)

    def remove_event(self, name: str, callback: EventCallback):
        """Unregisters a callback added with on_event()."""
        with self._event_lock:
            cbs = self._event_cbs.get(name, [])
            if callback in cbs:
                cbs.remove(callback)

    # -- calls -------------------------------------------------------------

    def call_async(self, request: pb.Request) -> Future:
        """Sends a request; the Future resolves to the Response (any status)."""
        if not self._running:
            raise RpcTransportError("RPC connection is closed")
        # uint32 on the wire; 0 is reserved for Events.
        seq = next(self._seq) % 0xFFFFFFFF + 1
        fut: Future = Future()
        with self._pending_lock:
            self._pending[seq] = fut
        if not self._running:
            # The reader died between the check above and registering.
            with self._pending_lock:
                self._pending.pop(seq, None)
            raise RpcTransportError("RPC connection is closed")
        env = pb.Envelope(seq=seq, request=request)
        try:
            self._write(cobs_encode(env.SerializeToString()) + b"\x00")
        except Exception:
            with self._pending_lock:
                self._pending.pop(seq, None)
            raise
        return fut

    def call(
        self, request: pb.Request, timeout: float = DEFAULT_TIMEOUT
    ) -> pb.Response:
        """Sends a request and waits for its Response.

        Raises:
            RpcError: the firmware answered with a non-OK status.
            RpcTransportError: no answer within timeout or link failure.
        """
        return self.result(self.call_async(request), request, timeout)

    def result(
        self, fut: Future, request: pb.Request, timeout: float = DEFAULT_TIMEOUT
    ) -> pb.Response:
        """Waits for a call_async() Future and raises on non-OK status."""
        name = request.WhichOneof("msg") or "unknown"
        try:
            resp = fut.result(timeout=timeout)
        except FutureTimeoutError as e:
            raise RpcTransportError(f"{name}: no response in {timeout:.1f}s") from e
        if resp.status != pb.STATUS_OK:
            raise RpcError(name, resp.status, resp.error, resp.error_code)
        return resp

    # -- system domain -----------------------------------------------------

    def ping(self, token: int = 0x4D415549) -> pb.PingResponse:
        """Round trip; also returns firmware version and capabilities."""
        resp = self.call(pb.Request(ping=pb.PingRequest(token=token)))
        if resp.ping.token != token:
            raise RpcTransportError("ping: token mismatch")
        self._supported = set(resp.ping.supported_requests)
        return resp.ping

    def supports(self, request_name: str) -> bool:
        """True if the firmware implements Request oneof field request_name."""
        if self._supported is None:
            self.ping()
        field = pb.Request.DESCRIPTOR.fields_by_name.get(request_name)
        return field is not None and field.number in self._supported

    # -- internals ---------------------------------------------------------

    def _write(self, data: bytes):
        if not self._ser or not self._ser.is_open:
            raise RpcTransportError("RPC port is not open")
        with self._write_lock:
            try:
                self._ser.write(data)
            except serial.SerialException as e:
                raise RpcTransportError(f"write failed: {e}") from e

    def _fail_pending(self, exc: Exception):
        with self._pending_lock:
            pending = list(self._pending.values())
            self._pending.clear()
        for fut in pending:
            if not fut.done():
                fut.set_exception(exc)

    def _read_loop(self):
        buf = bytearray()
        while self._running:
            try:
                chunk = self._ser.read(max(1, self._ser.in_waiting))
            except (serial.SerialException, OSError, TypeError) as e:
                if self._running:
                    logger.error("RPC read failed: %s", e)
                    self._fail_pending(RpcTransportError(f"read failed: {e}"))
                self._running = False
                break
            if not chunk:
                continue
            buf += chunk
            while True:
                idx = buf.find(b"\x00")
                if idx < 0:
                    break
                frame = bytes(buf[:idx])
                del buf[: idx + 1]
                if frame:
                    self._handle_frame(frame)

    def _handle_frame(self, frame: bytes):
        try:
            env = pb.Envelope.FromString(cobs_decode(frame))
        except Exception as e:  # pylint: disable=broad-exception-caught
            logger.warning("Dropping malformed RPC frame: %s", e)
            return

        kind = env.WhichOneof("kind")
        if kind == "response":
            with self._pending_lock:
                fut = self._pending.pop(env.seq, None)
            if fut is None:
                logger.warning("Response for unknown seq %d", env.seq)
            elif not fut.done():
                fut.set_result(env.response)
        elif kind == "event":
            name = env.event.WhichOneof("msg")
            with self._event_lock:
                callbacks = list(self._event_cbs.get(name, []))
            for cb in callbacks:
                try:
                    cb(env.event)
                except Exception:  # pylint: disable=broad-exception-caught
                    logger.exception("RPC event callback failed")
        else:
            logger.warning("Unexpected RPC envelope kind: %s", kind)
