# Copyright 2026 The ChromiumOS Authors
# Use of this source code is governed by a BSD-style license that can be
# found in the LICENSE file.

"""Prolific PL27A1 H2H bridge SPI flash access over the Maui RPC channel.

The MCU holds the PL27A1 in reset and muxes its SPI NOR flash to itself for
the duration of a session. Writing erases the whole chip, programs the image
as it streams in and verifies it with a CRC-32 computed on the MCU (no
read-back over USB). Reading streams the flash back in chunks. See the "h2h"
section of firmware/proto/maui_rpc.proto.
"""

from collections import deque
from dataclasses import dataclass
import logging
import time
from typing import Callable
from typing import Optional
import zlib

from maui_libs.proto import maui_rpc_pb2 as pb
from maui_libs.rpc import RpcClient


logger = logging.getLogger(__name__)

# (phase, done, total) with phase in {"erase", "program", "verify", "read"}.
ProgressCallback = Callable[[str, int, int], None]

# Requests in flight. The firmware buffers 16 KB of incoming frames, so three
# 4 KB chunks keep USB busy while the current chunk is being programmed.
DEFAULT_WINDOW = 3

# Size of a Prolific PL27A1 configuration image.
H2H_IMAGE_SIZE = 8704

# H2hBegin erases the whole chip: a 16 MB part takes ~40-100 s typically and
# far longer at datasheet worst case.
BEGIN_TIMEOUT = 300.0
WRITE_TIMEOUT = 10.0
FINISH_TIMEOUT = 60.0
READ_TIMEOUT = 10.0

_PHASES = {
    pb.H2H_PHASE_ERASE: "erase",
    pb.H2H_PHASE_PROGRAM: "program",
    pb.H2H_PHASE_VERIFY: "verify",
}


@dataclass
class H2hFlashInfo:
    """Identity of the H2H SPI flash."""

    jedec_id: int
    flash_size: int

    def __str__(self):
        return f"JEDEC ID 0x{self.jedec_id:06X}, {self.flash_size // 1024} KB"


@dataclass
class H2hFlashResult:
    """Outcome of a successful flash()."""

    info: H2hFlashInfo
    image_size: int
    crc32: int
    device_ms: int
    host_s: float


@dataclass
class H2hReadResult:
    """Outcome of a successful read()."""

    info: H2hFlashInfo
    data: bytes
    host_s: float


class H2hFlasher:
    """Reads and programs the H2H bridge SPI flash through an open RpcClient."""

    def __init__(self, rpc: RpcClient, window: int = DEFAULT_WINDOW):
        self._rpc = rpc
        self._window = max(1, window)

    def check_supported(self, request: str = "h2h_begin"):
        """Raises RuntimeError if the firmware lacks the given H2H request."""
        if not self._rpc.supports(request):
            raise RuntimeError(
                f"Device firmware does not support {request} over RPC; "
                "update the MCU firmware first."
            )

    def info(self) -> H2hFlashInfo:
        """Reads the flash JEDEC ID and size (briefly resets the PL27A1)."""
        resp = self._rpc.call(pb.Request(h2h_info=pb.H2hInfoRequest()))
        return H2hFlashInfo(resp.h2h_info.jedec_id, resp.h2h_info.flash_size)

    def flash(
        self, image: bytes, progress: Optional[ProgressCallback] = None
    ) -> H2hFlashResult:
        """Erases the whole flash, writes image at offset 0 and verifies it.

        On any failure the session is aborted so the PL27A1 is released from
        reset, and the exception is re-raised.
        """
        if not image:
            raise ValueError("H2H image is empty")

        crc = zlib.crc32(image) & 0xFFFFFFFF
        total = len(image)
        start = time.monotonic()

        def on_event(evt: pb.Event):
            if progress:
                p = evt.h2h_progress
                progress(_PHASES.get(p.phase, "?"), p.done, p.total)

        self._rpc.on_event("h2h_progress", on_event)
        try:
            begin = self._rpc.call(
                pb.Request(
                    h2h_begin=pb.H2hBeginRequest(image_size=total, image_crc32=crc)
                ),
                timeout=BEGIN_TIMEOUT,
            ).h2h_begin
            info = H2hFlashInfo(begin.jedec_id, begin.flash_size)
            logger.info("H2H flash: %s (erased)", info)

            self._stream(image, begin.max_chunk_size, progress)

            fin = self._rpc.call(
                pb.Request(h2h_finish=pb.H2hFinishRequest()),
                timeout=FINISH_TIMEOUT,
            ).h2h_finish
        except BaseException:
            self._abort()
            raise
        finally:
            self._rpc.remove_event("h2h_progress", on_event)

        if fin.flash_crc32 != crc:
            # The firmware already reports this as STATUS_VERIFY_FAILED; this
            # is a belt-and-braces check of the reported value.
            raise RuntimeError(
                f"H2H verify mismatch: flash 0x{fin.flash_crc32:08X} "
                f"!= image 0x{crc:08X}"
            )

        return H2hFlashResult(
            info=info,
            image_size=total,
            crc32=crc,
            device_ms=fin.duration_ms,
            host_s=time.monotonic() - start,
        )

    def _stream(self, image: bytes, chunk: int, progress: Optional[ProgressCallback]):
        """Pipelined sequential writes, at most self._window in flight."""
        if chunk <= 0:
            raise RuntimeError(f"firmware reported invalid chunk size {chunk}")

        total = len(image)
        in_flight = deque()

        def reap_one():
            fut, req = in_flight.popleft()
            self._rpc.result(fut, req, timeout=WRITE_TIMEOUT)

        acked = 0
        for offset in range(0, total, chunk):
            while len(in_flight) >= self._window:
                reap_one()
                acked = min(total, acked + chunk)
                if progress:
                    progress("program", acked, total)
            req = pb.Request(
                h2h_write=pb.H2hWriteRequest(
                    offset=offset, data=image[offset : offset + chunk]
                )
            )
            in_flight.append((self._rpc.call_async(req), req))

        while in_flight:
            reap_one()
            acked = min(total, acked + chunk)
            if progress:
                progress("program", acked, total)

    def read(
        self, size: Optional[int] = None, progress: Optional[ProgressCallback] = None
    ) -> H2hReadResult:
        """Reads `size` bytes from offset 0 (the whole flash if None).

        The PL27A1 is held in reset for the duration of the read. On any
        failure the session is aborted so the PL27A1 is released from reset,
        and the exception is re-raised.
        """
        if size is not None and size <= 0:
            raise ValueError("read size must be positive")

        start = time.monotonic()
        try:
            begin = self._rpc.call(
                pb.Request(h2h_read_begin=pb.H2hReadBeginRequest())
            ).h2h_read_begin
            info = H2hFlashInfo(begin.jedec_id, begin.flash_size)
            total = info.flash_size if size is None else size
            if total > info.flash_size:
                raise ValueError(
                    f"cannot read {total} B from a {info.flash_size} B flash"
                )
            logger.info("H2H flash: %s (reading %d B)", info, total)

            data = self._fetch(total, begin.max_chunk_size, progress)

            self._rpc.call(pb.Request(h2h_read_end=pb.H2hReadEndRequest()))
        except BaseException:
            self._abort()
            raise

        return H2hReadResult(info=info, data=data, host_s=time.monotonic() - start)

    def _fetch(
        self, total: int, chunk: int, progress: Optional[ProgressCallback]
    ) -> bytes:
        """Pipelined sequential reads, at most self._window in flight."""
        if chunk <= 0:
            raise RuntimeError(f"firmware reported invalid chunk size {chunk}")

        data = bytearray()
        in_flight = deque()

        def reap_one():
            fut, req = in_flight.popleft()
            got = self._rpc.result(fut, req, timeout=READ_TIMEOUT).h2h_read
            want = req.h2h_read
            if got.offset != want.offset or len(got.data) != want.length:
                raise RuntimeError(
                    f"H2H read 0x{want.offset:x}+{want.length} returned "
                    f"0x{got.offset:x}+{len(got.data)}"
                )
            data.extend(got.data)
            if progress:
                progress("read", len(data), total)

        for offset in range(0, total, chunk):
            while len(in_flight) >= self._window:
                reap_one()
            req = pb.Request(
                h2h_read=pb.H2hReadRequest(
                    offset=offset, length=min(chunk, total - offset)
                )
            )
            in_flight.append((self._rpc.call_async(req), req))

        while in_flight:
            reap_one()
        return bytes(data)

    def _abort(self):
        try:
            self._rpc.call(pb.Request(h2h_abort=pb.H2hAbortRequest()), timeout=2.0)
        except Exception as e:  # pylint: disable=broad-exception-caught
            logger.warning(
                "H2H abort failed (%s); the MCU releases the bus on its own "
                "after its idle timeout.",
                e,
            )
