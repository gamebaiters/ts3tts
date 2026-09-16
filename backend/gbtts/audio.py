"""Streaming audio post-processing: every stage works chunk by chunk.

Order applied per job (see server.py):
    engine chunk (24 k / 44.1 k float32)
      -> StreamResampler   (soxr HQ, stateful, to 48 k)
      -> WsolaStretch      (optional speed change, pitch preserved)
      -> Leveler           (slow speech AGC so every voice/engine sits at the
                            same loudness in the channel)
      -> to_int16          (soft knee, never a hard clip)
"""

from __future__ import annotations

import math

import numpy as np
import soxr

OUT_SR = 48000


class StreamResampler:
    """Stateful sample-rate converter. Chunk boundaries are inaudible."""

    def __init__(self, in_sr: int, out_sr: int = OUT_SR):
        self.in_sr = int(in_sr)
        self.out_sr = int(out_sr)
        self._rs = None
        if self.in_sr != self.out_sr:
            self._rs = soxr.ResampleStream(self.in_sr, self.out_sr, 1, dtype="float32", quality="HQ")

    def process(self, x: np.ndarray, last: bool = False) -> np.ndarray:
        x = np.ascontiguousarray(x, dtype=np.float32).reshape(-1)
        if self._rs is None:
            return x
        return self._rs.resample_chunk(x, last=last)

    def flush(self) -> np.ndarray:
        return self.process(np.zeros(0, dtype=np.float32), last=True)


class WsolaStretch:
    """Streaming WSOLA time-scale modification (speed without pitch change).

    speed > 1 speaks faster. Frames of 32 ms, 50 % overlap Hann synthesis (sums
    to unity), +-10 ms similarity search against the natural continuation of
    the previously placed frame. At speed == 1 it is an exact passthrough.
    """

    def __init__(self, speed: float, sr: int = OUT_SR):
        self.speed = float(min(2.0, max(0.5, speed)))
        self.bypass = abs(self.speed - 1.0) < 1e-3
        self.n = 1536 if sr >= 44100 else 768
        self.hs = self.n // 2
        self.ha = self.hs * self.speed
        self.tol = self.n // 3
        self.win = (0.5 - 0.5 * np.cos(2.0 * np.pi * np.arange(self.n) / self.n)).astype(np.float32)
        self._in = np.zeros(0, dtype=np.float32)
        self._in_base = 0          # absolute input index of self._in[0]
        self._out = np.zeros(self.n, dtype=np.float32)
        self._out_base = 0         # absolute output index of self._out[0]
        self._k = 0                # next frame index
        self._prev_pos = None      # absolute input start of the last placed frame
        self._in_total = 0

    def _frame_input_pos(self, k: int) -> int:
        return int(round(k * self.ha))

    def _place(self, k: int) -> bool:
        """Place frame k if enough input is buffered. Returns False if not."""
        nominal = self._frame_input_pos(k)
        lo = nominal - self.tol if k > 0 else nominal
        hi = nominal + self.tol if k > 0 else nominal
        lo = max(lo, self._in_base)
        need_end = hi + self.n
        if self._prev_pos is not None:
            need_end = max(need_end, self._prev_pos + self.hs + self.n)
        if need_end > self._in_base + len(self._in):
            return False

        if k == 0 or self._prev_pos is None:
            pos = nominal
        else:
            t0 = self._prev_pos + self.hs - self._in_base
            template = self._in[t0:t0 + self.n]
            r0 = lo - self._in_base
            region = self._in[r0:r0 + (hi - lo) + self.n]
            # Decimated coarse search then exact refinement keeps the numpy cost
            # at a few ms per second of audio.
            step = 4
            corr = np.correlate(region[::step], template[::step], mode="valid")
            best = int(np.argmax(corr)) * step
            a = max(0, best - step)
            b = min(len(region) - self.n, best + step)
            fine = np.correlate(region[a:b + self.n], template, mode="valid")
            pos = lo + a + int(np.argmax(fine))

        s = pos - self._in_base
        frame = self._in[s:s + self.n] * self.win
        o = k * self.hs - self._out_base
        if o + self.n > len(self._out):
            grow = np.zeros(o + self.n - len(self._out), dtype=np.float32)
            self._out = np.concatenate([self._out, grow])
        self._out[o:o + self.n] += frame
        self._prev_pos = pos
        return True

    def _emit(self, upto_abs: int) -> np.ndarray:
        cut = upto_abs - self._out_base
        if cut <= 0:
            return np.zeros(0, dtype=np.float32)
        y = self._out[:cut].copy()
        self._out = self._out[cut:]
        self._out_base = upto_abs
        return y

    def _trim_input(self) -> None:
        keep_from = self._frame_input_pos(self._k) - self.tol
        if self._prev_pos is not None:
            keep_from = min(keep_from, self._prev_pos + self.hs)
        drop = keep_from - self._in_base
        if drop > 4096:
            self._in = self._in[drop:]
            self._in_base += drop

    def process(self, x: np.ndarray) -> np.ndarray:
        x = np.asarray(x, dtype=np.float32).reshape(-1)
        if self.bypass:
            return x
        self._in_total += len(x)
        self._in = np.concatenate([self._in, x]) if len(self._in) else x.copy()
        while self._place(self._k):
            self._k += 1
        self._trim_input()
        # Everything before the start of the next frame is final.
        return self._emit(self._k * self.hs)

    def flush(self) -> np.ndarray:
        if self.bypass:
            return np.zeros(0, dtype=np.float32)
        target_out = int(round(self._in_total / self.speed))
        pad = np.zeros(self.n * 2 + self.tol * 2, dtype=np.float32)
        self._in = np.concatenate([self._in, pad])
        while self._frame_input_pos(self._k) < self._in_total and self._place(self._k):
            self._k += 1
        y = self._emit(self._out_base + len(self._out))
        keep = max(0, target_out - (self._out_base - len(y)))
        return y[:keep]


class Leveler:
    """Slow speech-aware AGC.

    Different voices and engines come out 10+ dB apart; in a voice channel that
    is the difference between inaudible and shouting. The level estimate only
    follows voiced 20 ms frames (gate at -50 dBFS) so pauses never pump the gain
    up; the gain itself moves at most ~6 dB/s and is clamped to [-9, +12] dB.
    """

    def __init__(self, target_dbfs: float = -19.0, sr: int = OUT_SR, enabled: bool = True):
        self.enabled = enabled
        self.sr = sr
        self.frame = sr // 50
        self.target = 10.0 ** (target_dbfs / 20.0)
        self.gate = 10.0 ** (-50.0 / 20.0)
        self.min_g = 10.0 ** (-9.0 / 20.0)
        self.max_g = 10.0 ** (12.0 / 20.0)
        self.ms = None
        self.gain = 1.0
        self.voiced_frames = 0
        self._pending = np.zeros(0, dtype=np.float32)

    def process(self, x: np.ndarray) -> np.ndarray:
        x = np.asarray(x, dtype=np.float32).reshape(-1)
        if not self.enabled or x.size == 0:
            return x
        buf = np.concatenate([self._pending, x]) if self._pending.size else x
        nfull = (len(buf) // self.frame) * self.frame
        self._pending = buf[nfull:].copy()
        if nfull == 0:
            return np.zeros(0, dtype=np.float32)
        frames = buf[:nfull].reshape(-1, self.frame)
        out = np.empty_like(frames)
        for i, fr in enumerate(frames):
            ms = float(np.mean(fr * fr))
            if ms > self.gate * self.gate:
                self.voiced_frames += 1
                # Fast convergence for the first ~0.4 s of speech, then slow.
                alpha = 0.25 if self.voiced_frames < 20 else 0.03
                self.ms = ms if self.ms is None else (1.0 - alpha) * self.ms + alpha * ms
            desired = self.gain
            if self.ms is not None and self.ms > 0.0:
                desired = min(self.max_g, max(self.min_g, self.target / math.sqrt(self.ms)))
            max_step = 10.0 ** ((6.0 if self.voiced_frames >= 20 else 30.0) / 20.0 / 50.0)
            new_gain = min(self.gain * max_step, max(self.gain / max_step, desired))
            ramp = np.linspace(self.gain, new_gain, self.frame, endpoint=False, dtype=np.float32)
            out[i] = fr * ramp
            self.gain = new_gain
        return out.reshape(-1)

    def flush(self) -> np.ndarray:
        y = self._pending * self.gain if self.enabled else self._pending
        self._pending = np.zeros(0, dtype=np.float32)
        return y.astype(np.float32)


def soft_clip(x: np.ndarray, knee: float = 0.891) -> np.ndarray:
    """Identity below the knee (-1 dBFS), smooth tanh saturation above."""
    x = np.asarray(x, dtype=np.float32)
    a = np.abs(x)
    over = a > knee
    if not np.any(over):
        return x
    y = x.copy()
    head = 1.0 - knee
    y[over] = np.sign(x[over]) * (knee + head * np.tanh((a[over] - knee) / head))
    return y


def to_int16(x: np.ndarray) -> np.ndarray:
    y = soft_clip(x)
    return np.clip(np.round(y * 32767.0), -32768, 32767).astype(np.int16)


def fade(x: np.ndarray, fade_in: int = 0, fade_out: int = 0) -> np.ndarray:
    x = np.array(x, dtype=np.float32, copy=True)
    if fade_in > 0 and len(x):
        n = min(fade_in, len(x))
        x[:n] *= np.linspace(0.0, 1.0, n, dtype=np.float32)
    if fade_out > 0 and len(x):
        n = min(fade_out, len(x))
        x[-n:] *= np.linspace(1.0, 0.0, n, dtype=np.float32)
    return x


def trim_silence(x: np.ndarray, sr: int, threshold_db: float = -45.0, pad_ms: float = 60.0) -> np.ndarray:
    """Trim leading/trailing silence, keeping a short natural pad."""
    x = np.asarray(x, dtype=np.float32).reshape(-1)
    if x.size == 0:
        return x
    thr = 10.0 ** (threshold_db / 20.0)
    idx = np.flatnonzero(np.abs(x) > thr)
    if idx.size == 0:
        return x[:0]
    pad = int(sr * pad_ms / 1000.0)
    return x[max(0, idx[0] - pad):min(len(x), idx[-1] + pad)]
