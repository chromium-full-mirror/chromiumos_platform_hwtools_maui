#!/usr/bin/env python3
# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.
"""Tests for the Maui RPC client and the H2H flasher against a fake device."""

import queue
import threading
import unittest
from unittest import mock
import zlib

from maui_libs import rpc as rpc_mod
from maui_libs.h2h import H2hFlasher
from maui_libs.proto import maui_rpc_pb2 as pb
from maui_libs.rpc import cobs_decode
from maui_libs.rpc import cobs_encode
from maui_libs.rpc import find_rpc_port
from maui_libs.rpc import RpcClient
from maui_libs.rpc import RpcError
from maui_libs.rpc import RpcTransportError
import serial


FLASH_SIZE = 64 * 1024
JEDEC_ID = 0xEF4010
CHUNK = 4096


class FakeDevice:  # pylint: disable=too-few-public-methods
    """Minimal model of the firmware side of the RPC channel (rpc_h2h.c)."""

    def __init__(self):
        self.flash = bytearray(b"\x00" * FLASH_SIZE)
        self.session = None
        self.corrupt_offset = None
        self.short_read_offset = None
        self.requests = []

    def handle(self, req: pb.Request) -> pb.Response:
        """Returns the Response to one Request."""
        name = req.WhichOneof("msg")
        self.requests.append(name)
        resp = pb.Response(status=pb.STATUS_OK)
        if name == "ping":
            resp.ping.token = req.ping.token
            resp.ping.protocol_version = pb.PROTOCOL_VERSION_CURRENT
            resp.ping.supported_requests.extend(
                f.number for f in pb.Request.DESCRIPTOR.fields
            )
        elif name == "h2h_info":
            resp.h2h_info.jedec_id = JEDEC_ID
            resp.h2h_info.flash_size = FLASH_SIZE
        elif name == "h2h_begin":
            if req.h2h_begin.image_size > FLASH_SIZE:
                return pb.Response(
                    status=pb.STATUS_TOO_LARGE, error="too large", error_code=-27
                )
            self.flash[:] = b"\xff" * FLASH_SIZE
            self.session = {"req": req.h2h_begin, "next": 0}
            resp.h2h_begin.jedec_id = JEDEC_ID
            resp.h2h_begin.flash_size = FLASH_SIZE
            resp.h2h_begin.max_chunk_size = CHUNK
        elif name.startswith("h2h_read"):
            return self._handle_read(name, req, resp)
        elif name == "h2h_write":
            w = req.h2h_write
            if not self.session or w.offset != self.session["next"]:
                self.session = None
                return pb.Response(status=pb.STATUS_BAD_STATE, error="order")
            data = bytearray(w.data)
            if self.corrupt_offset is not None and w.offset == self.corrupt_offset:
                data[0] ^= 0xFF
            self.flash[w.offset : w.offset + len(data)] = data
            self.session["next"] += len(data)
            resp.h2h_write.SetInParent()
        elif name == "h2h_finish":
            size = self.session["req"].image_size
            crc = zlib.crc32(self.flash[:size]) & 0xFFFFFFFF
            expected = self.session["req"].image_crc32
            self.session = None
            resp.h2h_finish.flash_crc32 = crc
            resp.h2h_finish.duration_ms = 1
            if crc != expected:
                resp.status = pb.STATUS_VERIFY_FAILED
                resp.error = "flash CRC mismatch"
        elif name == "h2h_abort":
            self.session = None
            resp.h2h_abort.SetInParent()
        else:
            resp.status = pb.STATUS_UNSUPPORTED
        return resp

    def _handle_read(self, name, req, resp):
        """Read session requests (rpc_h2h.c handle_read_*)."""
        if name == "h2h_read_begin":
            self.session = {"read": True}
            resp.h2h_read_begin.jedec_id = JEDEC_ID
            resp.h2h_read_begin.flash_size = FLASH_SIZE
            resp.h2h_read_begin.max_chunk_size = CHUNK
        elif name == "h2h_read":
            r = req.h2h_read
            if not self.session or not self.session.get("read"):
                self.session = None
                return pb.Response(status=pb.STATUS_BAD_STATE, error="no session")
            if not 0 < r.length <= CHUNK or r.offset + r.length > FLASH_SIZE:
                self.session = None
                return pb.Response(status=pb.STATUS_INVALID_ARGUMENT, error="range")
            resp.h2h_read.offset = r.offset
            resp.h2h_read.data = bytes(self.flash[r.offset : r.offset + r.length])
            if self.short_read_offset == r.offset:
                resp.h2h_read.data = resp.h2h_read.data[:-1]
        elif name == "h2h_read_end":
            if not self.session or not self.session.get("read"):
                return pb.Response(status=pb.STATUS_BAD_STATE, error="no session")
            self.session = None
            resp.h2h_read_end.SetInParent()
        return resp


class FakeSerial:
    """Stands in for serial.Serial, routing frames to a FakeDevice."""

    device = None  # set by the fixture

    def __init__(self, *_args, **_kwargs):
        self.is_open = True
        self._rx = bytearray()
        self._out = queue.Queue()
        self._lock = threading.Lock()

    @property
    def in_waiting(self):
        """Bytes ready to read."""
        return self._out.qsize()

    def reset_input_buffer(self):
        """No-op."""

    def write(self, data):
        """Feeds host bytes to the fake firmware."""
        with self._lock:
            self._rx += data
            while b"\x00" in self._rx:
                idx = self._rx.index(b"\x00")
                frame = bytes(self._rx[:idx])
                del self._rx[: idx + 1]
                if frame:
                    self._handle(frame)
        return len(data)

    def _handle(self, frame):
        env = pb.Envelope.FromString(cobs_decode(frame))
        req = env.request
        if req.WhichOneof("msg") == "h2h_begin":
            evt = pb.Envelope(
                seq=0,
                event=pb.Event(
                    timestamp_ms=1234,
                    event_seq=1,
                    h2h_progress=pb.H2hProgressEvent(
                        phase=pb.H2H_PHASE_ERASE, done=FLASH_SIZE, total=FLASH_SIZE
                    ),
                ),
            )
            self._emit(evt)
        resp = self.device.handle(req)
        self._emit(pb.Envelope(seq=env.seq, response=resp))

    def _emit(self, env):
        for b in cobs_encode(env.SerializeToString()) + b"\x00":
            self._out.put(bytes([b]))

    def fail(self):
        """Simulates the device disappearing (USB unplug)."""
        self._out.put(None)

    def read(self, size=1):
        """Returns up to size bytes, waiting briefly for the first one."""
        out = bytearray()
        try:
            item = self._out.get(timeout=0.01)
            while item is not None:
                out += item
                if len(out) >= size:
                    break
                item = self._out.get_nowait()
            if item is None:
                raise serial.SerialException("device disconnected")
        except queue.Empty:
            pass
        return bytes(out)

    def close(self):
        """Closes the fake port."""
        self.is_open = False


class RpcH2hTest(unittest.TestCase):
    """RpcClient and H2hFlasher against FakeDevice."""

    def setUp(self):
        self.device = FakeDevice()
        FakeSerial.device = self.device
        patcher = mock.patch.object(rpc_mod.serial, "Serial", FakeSerial)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_cobs_round_trip(self):
        """COBS encoding is reversible and never emits the delimiter."""
        for data in (
            b"",
            b"\x00",
            b"\x11\x00\x22",
            bytes(range(1, 255)),
            bytes(600),
            bytes(range(256)) * 3,
        ):
            enc = cobs_encode(data)
            self.assertNotIn(b"\x00", enc)
            self.assertEqual(cobs_decode(enc), data)

    def test_cobs_decode_rejects_malformed(self):
        """Embedded zeros and truncated blocks raise ValueError."""
        for bad in (b"\x03\x11\x00", b"\x05\x11\x22"):
            with self.assertRaises(ValueError):
                cobs_decode(bad)

    def test_ping_and_capabilities(self):
        """ping() round-trips and populates supports()."""
        with RpcClient("/dev/fake") as client:
            self.assertEqual(client.ping(token=1234).token, 1234)
            self.assertTrue(client.supports("h2h_begin"))
            self.assertFalse(client.supports("no_such_request"))
        self.assertEqual(self.device.requests, ["ping"])

    def test_h2h_flash_success(self):
        """Image is written sequentially, verified, and progress is reported."""
        image = bytes((i * 7) & 0xFF for i in range(3 * CHUNK + 123))
        phases = set()
        with RpcClient("/dev/fake") as client:
            result = H2hFlasher(client).flash(
                image, progress=lambda phase, done, total: phases.add(phase)
            )
        self.assertEqual(bytes(self.device.flash[: len(image)]), image)
        self.assertEqual(result.crc32, zlib.crc32(image))
        self.assertEqual(result.info.jedec_id, JEDEC_ID)
        self.assertLessEqual({"erase", "program"}, phases)
        self.assertEqual(self.device.requests.count("h2h_write"), 4)
        self.assertEqual(self.device.requests[-1], "h2h_finish")

    def test_h2h_verify_failure_raises(self):
        """A flash CRC mismatch is reported as RpcError(VERIFY_FAILED)."""
        self.device.corrupt_offset = CHUNK
        with RpcClient("/dev/fake") as client:
            with self.assertRaises(RpcError) as err:
                H2hFlasher(client).flash(bytes(2 * CHUNK))
        self.assertEqual(err.exception.status, pb.STATUS_VERIFY_FAILED)
        self.assertEqual(self.device.requests[-1], "h2h_abort")

    def test_h2h_too_large_aborts(self):
        """An oversized image is rejected and the session is aborted."""
        with RpcClient("/dev/fake") as client:
            with self.assertRaises(RpcError) as err:
                H2hFlasher(client).flash(bytes(FLASH_SIZE + 1))
        self.assertEqual(err.exception.status, pb.STATUS_TOO_LARGE)
        self.assertEqual(err.exception.error_code, -27)
        self.assertNotIn("h2h_write", self.device.requests)
        self.assertEqual(self.device.requests[-1], "h2h_abort")

    def test_h2h_info(self):
        """info() returns the JEDEC ID and flash size."""
        with RpcClient("/dev/fake") as client:
            info = H2hFlasher(client).info()
        self.assertEqual((info.jedec_id, info.flash_size), (JEDEC_ID, FLASH_SIZE))
        self.assertIsNone(self.device.session)

    def test_h2h_read_success(self):
        """Reads are pipelined in max_chunk_size pieces and end the session."""
        self.device.flash[:] = bytes((i * 13 + 5) & 0xFF for i in range(FLASH_SIZE))
        size = 2 * CHUNK + 512  # Like an 8704 B PL27A1 image.
        progress = []
        with RpcClient("/dev/fake") as client:
            result = H2hFlasher(client).read(
                size, progress=lambda *a: progress.append(a)
            )
        self.assertEqual(result.data, bytes(self.device.flash[:size]))
        self.assertEqual(result.info.jedec_id, JEDEC_ID)
        self.assertEqual(progress[-1], ("read", size, size))
        reqs = self.device.requests
        self.assertEqual(reqs[0], "h2h_read_begin")
        self.assertEqual(reqs.count("h2h_read"), 3)
        self.assertEqual(reqs[-1], "h2h_read_end")
        self.assertIsNone(self.device.session)

    def test_h2h_read_whole_flash_by_default(self):
        """read() without a size dumps the whole flash."""
        with RpcClient("/dev/fake") as client:
            result = H2hFlasher(client).read()
        self.assertEqual(len(result.data), FLASH_SIZE)

    def test_h2h_read_too_large_aborts(self):
        """Asking for more than the flash holds aborts before reading."""
        with RpcClient("/dev/fake") as client:
            with self.assertRaises(ValueError):
                H2hFlasher(client).read(FLASH_SIZE + 1)
        self.assertNotIn("h2h_read", self.device.requests)
        self.assertEqual(self.device.requests[-1], "h2h_abort")

    def test_h2h_short_read_raises_and_aborts(self):
        """A response with the wrong length is rejected."""
        self.device.short_read_offset = CHUNK
        with RpcClient("/dev/fake") as client:
            with self.assertRaisesRegex(RuntimeError, "returned"):
                H2hFlasher(client).read(2 * CHUNK)
        self.assertEqual(self.device.requests[-1], "h2h_abort")

    def test_disconnect_fails_fast(self):
        """After the device disappears, calls fail at once instead of hanging."""
        client = RpcClient("/dev/fake")
        client.open()
        try:
            client._ser.fail()  # pylint: disable=protected-access
            client._reader.join(timeout=2)  # pylint: disable=protected-access
            with self.assertRaises(RpcTransportError):
                client.ping()
        finally:
            client.close()


def _port(device, location, interface, serial_number="SN1"):
    return mock.Mock(
        device=device,
        location=location,
        interface=interface,
        serial_number=serial_number,
        vid=rpc_mod.MAUI_FINAL_VID,
        pid=rpc_mod.MAUI_FINAL_PID,
    )


class FindRpcPortTest(unittest.TestCase):
    """RPC port discovery."""

    PORTS = [
        _port("/dev/ttyACM0", "1-1:1.0", "Maui Console"),
        _port("/dev/ttyACM1", "1-1:1.2", "Maui RPC"),
        _port("/dev/ttyACM2", "1-2:1.0", "Maui Console"),
        _port("/dev/ttyACM3", "1-2:1.2", "Maui RPC"),
    ]

    def setUp(self):
        patcher = mock.patch.object(
            rpc_mod.serial.tools.list_ports, "comports", return_value=self.PORTS
        )
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_matches_console_port_on_same_usb_device(self):
        """Two boards with the same serial: the console port disambiguates."""
        self.assertEqual(find_rpc_port(console_port="/dev/ttyACM2"), "/dev/ttyACM3")

    def test_multiple_devices_without_hint_raises(self):
        """Ambiguity is reported instead of guessing."""
        with self.assertRaises(RpcTransportError):
            find_rpc_port(serial_number="SN1")


if __name__ == "__main__":
    unittest.main()
