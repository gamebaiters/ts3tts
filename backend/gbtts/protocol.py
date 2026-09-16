"""Wire protocol shared with the C++ plugin (plugin/src/net/BackendLink.cpp).

Every frame on the loopback socket is:

    u32 little-endian  length   (= 1 + len(payload))
    u8                 type
    bytes              payload

type 0x01 JSON   UTF-8 object, both directions ("op" plugin->backend,
                 "ev" backend->plugin)
type 0x02 PCM    backend->plugin only: u32 LE job id + int16 LE mono samples
                 at 48000 Hz
type 0x03 MIC    plugin->backend only: u32 LE sequence + int16 LE mono 48000 Hz,
                 the user's microphone for the real-time voice changer (20 ms blocks)
type 0x04 VC     backend->plugin only: u32 LE sequence of the newest mic block that
                 went into it + int16 LE mono 48000 Hz converted voice

The plugin is the TCP server (127.0.0.1, ephemeral port) and hands the port and
a random token to this process on the command line. The first frame this side
sends is {"ev": "hello", "token": ...}; anything else gets the socket closed.
"""

from __future__ import annotations

import json
import socket
import struct
import threading

import numpy as np

T_JSON = 0x01
T_PCM = 0x02
T_MIC = 0x03
T_VC = 0x04

MAX_FRAME = 16 * 1024 * 1024
SAMPLE_RATE = 48000

_HDR = struct.Struct("<IB")
_JOB = struct.Struct("<I")


class ConnectionClosed(Exception):
    pass


class FrameConnection:
    """Blocking, thread-safe framed connection.

    send_* may be called from any thread (a lock serialises whole frames).
    recv_frame() must only be called from one reader thread.

    sendall() blocking is intentional: when the plugin's ring buffer is full it
    stops reading, the kernel buffer fills, and generation pauses right here.
    That is the flow control - memory on both sides stays bounded no matter how
    much faster than real time the GPU is.
    """

    def __init__(self, sock: socket.socket):
        self._sock = sock
        self._wlock = threading.Lock()
        self._closed = False

    @classmethod
    def connect(cls, port: int, timeout: float = 10.0) -> "FrameConnection":
        sock = socket.create_connection(("127.0.0.1", port), timeout=timeout)
        sock.settimeout(None)
        sock.setsockopt(socket.IPPROTO_TCP, socket.TCP_NODELAY, 1)
        return cls(sock)

    # ---- send ---------------------------------------------------------------
    def send_json(self, obj: dict) -> None:
        payload = json.dumps(obj, ensure_ascii=False, separators=(",", ":")).encode("utf-8")
        self._send(T_JSON, payload)

    def send_pcm(self, job_id: int, pcm: np.ndarray) -> None:
        if pcm.size == 0:
            return
        if pcm.dtype != np.int16:
            raise TypeError("PCM must be int16")
        body = np.ascontiguousarray(pcm, dtype="<i2").tobytes()
        self._send(T_PCM, _JOB.pack(job_id & 0xFFFFFFFF) + body)

    def send_vc(self, seq: int, pcm: np.ndarray) -> None:
        if pcm.size == 0:
            return
        body = np.ascontiguousarray(pcm, dtype="<i2").tobytes()
        self._send(T_VC, _JOB.pack(seq & 0xFFFFFFFF) + body)

    @staticmethod
    def parse_audio(payload: bytes) -> tuple[int, np.ndarray]:
        """u32 LE sequence/job + int16 LE samples (frame types 0x02/0x03/0x04)."""
        if len(payload) < 4:
            raise ValueError("short audio frame")
        (seq,) = _JOB.unpack_from(payload, 0)
        n = (len(payload) - 4) // 2
        return seq, np.frombuffer(payload, dtype="<i2", count=n, offset=4).astype(np.int16)

    def _send(self, ftype: int, payload: bytes) -> None:
        if self._closed:
            raise ConnectionClosed()
        header = _HDR.pack(len(payload) + 1, ftype)
        with self._wlock:
            try:
                self._sock.sendall(header)
                self._sock.sendall(payload)
            except OSError as exc:
                self._closed = True
                raise ConnectionClosed() from exc

    # ---- receive -------------------------------------------------------------
    def recv_frame(self) -> tuple[int, bytes]:
        header = self._recv_exact(_HDR.size)
        length, ftype = _HDR.unpack(header)
        if length < 1 or length > MAX_FRAME:
            raise ConnectionClosed(f"bad frame length {length}")
        payload = self._recv_exact(length - 1)
        return ftype, payload

    def recv_json(self) -> dict:
        while True:
            ftype, payload = self.recv_frame()
            if ftype == T_JSON:
                obj = json.loads(payload.decode("utf-8"))
                if isinstance(obj, dict):
                    return obj

    def _recv_exact(self, n: int) -> bytes:
        buf = bytearray()
        while len(buf) < n:
            try:
                chunk = self._sock.recv(n - len(buf))
            except OSError as exc:
                self._closed = True
                raise ConnectionClosed() from exc
            if not chunk:
                self._closed = True
                raise ConnectionClosed()
            buf += chunk
        return bytes(buf)

    def close(self) -> None:
        self._closed = True
        try:
            self._sock.shutdown(socket.SHUT_RDWR)
        except OSError:
            pass
        try:
            self._sock.close()
        except OSError:
            pass
