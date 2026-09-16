"""Turns the live microphone stream into converted voice, only while the user speaks.

Model-independent streaming policy around a converter with
    reset() / process(int16 48 k) -> int16 48 k
so it can be tested with a fake converter (tests/test_vc_session.py).

* Energy gate with hysteresis. The converter costs ~0.3-0.75 of real time on 4 CPU threads
  whether you speak or not; silence is not converted at all.
* Pre-roll: the last `preroll_ms` of gated-out audio is replayed on onset, so the first
  consonant is not eaten by the gate.
* Tail flush: when speech ends, `flush_ms` of zeros push out what the model still holds
  (future frames, vocoder overlap), then the stream is reset for the next phrase.
* Bounded latency: the caller reports its queue backlog; above `max_backlog_ms` the worker
  is not keeping up, so audio is dropped and the stream re-synchronised instead of letting
  the delay grow without end.
"""

from __future__ import annotations

from collections import deque
from dataclasses import dataclass

import numpy as np


@dataclass
class GateConfig:
    open_dbfs: float = -42.0      # a block louder than this opens the gate
    close_dbfs: float = -50.0     # the gate closes after `hang_ms` below this
    hang_ms: int = 350
    preroll_ms: int = 200
    flush_ms: int = 240
    max_backlog_ms: int = 400


def block_dbfs(pcm: np.ndarray) -> float:
    if pcm.size == 0:
        return -120.0
    rms = float(np.sqrt(np.mean(np.square(pcm.astype(np.float32) / 32768.0))))
    return 20.0 * np.log10(max(rms, 1e-6))


class VcSession:
    RATE = 48000

    def __init__(self, converter, config: GateConfig | None = None):
        self.conv = converter
        self.cfg = config or GateConfig()
        self.active = False
        self._quiet_ms = 0
        self._pre = deque()
        self._pre_ms = 0
        self.stats = {"blocks": 0, "converted": 0, "dropped": 0, "phrases": 0}

    def _ms(self, pcm: np.ndarray) -> int:
        return int(round(pcm.size * 1000 / self.RATE))

    def reset(self) -> None:
        self.active = False
        self._quiet_ms = 0
        self._pre.clear()
        self._pre_ms = 0
        self.conv.reset()

    def feed(self, pcm: np.ndarray, backlog_ms: int = 0) -> np.ndarray:
        """One mic block in, whatever converted audio is ready out (int16, maybe empty)."""
        self.stats["blocks"] += 1
        if backlog_ms > self.cfg.max_backlog_ms:
            self.stats["dropped"] += 1
            if self.active:
                self.reset()
            return np.zeros(0, np.int16)

        level = block_dbfs(pcm)
        if not self.active:
            self._pre.append(pcm)
            self._pre_ms += self._ms(pcm)
            while self._pre and self._pre_ms - self._ms(self._pre[0]) >= self.cfg.preroll_ms:
                self._pre_ms -= self._ms(self._pre.popleft())
            if level < self.cfg.open_dbfs:
                return np.zeros(0, np.int16)
            # onset: fresh stream, pre-roll first (it contains this block)
            self.active = True
            self._quiet_ms = 0
            self.stats["phrases"] += 1
            self.conv.reset()
            audio = np.concatenate(list(self._pre))
            self._pre.clear()
            self._pre_ms = 0
            self.stats["converted"] += 1
            return self.conv.process(audio)

        self._quiet_ms = self._quiet_ms + self._ms(pcm) if level < self.cfg.close_dbfs else 0
        out = self.conv.process(pcm)
        self.stats["converted"] += 1
        if self._quiet_ms >= self.cfg.hang_ms:
            tail = self.conv.process(np.zeros(self.RATE * self.cfg.flush_ms // 1000, np.int16))
            out = np.concatenate([out, tail]) if tail.size else out
            self.active = False
            self._quiet_ms = 0
            self.conv.reset()
        return out
