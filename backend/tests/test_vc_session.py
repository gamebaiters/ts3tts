"""Streaming policy of the voice changer (gbtts/vc/session.py) with a fake converter.

    backend\\.venv\\Scripts\\python.exe backend\\tests\\test_vc_session.py

No model, no audio device: the converter echoes its input, so every output sample can be
traced back to the input block it came from.
"""

from __future__ import annotations

import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from gbtts.vc.session import GateConfig, VcSession, block_dbfs  # noqa: E402

RATE = 48000
BLK = 960
passed = failed = 0


def check(name: str, ok: bool, detail: str = "") -> None:
    global passed, failed
    print(f"  {'PASS' if ok else 'FAIL'} {name}  {detail}")
    passed, failed = passed + ok, failed + (not ok)


class EchoConverter:
    def __init__(self):
        self.resets = 0
        self.processed = 0

    def reset(self):
        self.resets += 1

    def process(self, pcm):
        self.processed += pcm.size
        return pcm.copy()


def block(dbfs: float, marker: int = 0) -> np.ndarray:
    """20 ms of a sine at `dbfs` RMS; sample 0 carries a marker for tracing (silence: 0)."""
    if dbfs <= -100:
        return np.zeros(BLK, np.int16)
    amp = 10 ** (dbfs / 20) * np.sqrt(2) * 32767
    x = (amp * np.sin(2 * np.pi * 220 * np.arange(BLK) / RATE)).astype(np.int16)
    x[0] = marker
    return x


cfg = GateConfig()

# 1 silence never reaches the converter
conv = EchoConverter()
s = VcSession(conv, cfg)
out = [s.feed(block(-70)) for _ in range(50)]
check("silence is not converted", conv.processed == 0 and all(o.size == 0 for o in out))
check("level meter", abs(block_dbfs(block(-30)) - (-30)) < 0.3, f"{block_dbfs(block(-30)):.2f} dBFS")

# 2 onset replays the pre-roll (200 ms = 10 blocks, including the onset block)
conv = EchoConverter()
s = VcSession(conv, cfg)
for i in range(30):
    s.feed(block(-70, marker=0))
quiet_markers = [block(-60, marker=1000 + i) for i in range(9)]
for b in quiet_markers:
    s.feed(b)                                  # -60 dBFS: below the open threshold, kept as pre-roll
onset = s.feed(block(-20, marker=7777))
check("onset opens the gate", s.active and s.stats["phrases"] == 1)
check("pre-roll + onset block delivered", onset.size == 10 * BLK, f"{onset.size / BLK:.0f} blocks")
check("pre-roll keeps the order", onset[0] == 1000 and onset[9 * BLK] == 7777)
check("stream reset at onset", conv.resets == 1)

# 3 short pauses keep the gate open; a long one closes it with a tail flush
before = conv.processed
for _ in range(10):
    s.feed(block(-20))
for _ in range(int(cfg.hang_ms / 20) - 2):     # pause shorter than hang
    s.feed(block(-70))
s.feed(block(-20))
check("pause shorter than hang keeps converting", s.active)
closing = []
for _ in range(int(cfg.hang_ms / 20) + 2):
    closing.append(s.feed(block(-70)))
check("long pause closes the gate", not s.active)
last = [o for o in closing if o.size][-1]
check("tail flush appended", last.size == BLK + RATE * cfg.flush_ms // 1000, f"{last.size} samples")
check("stream reset after the phrase", conv.resets == 2)
check("converter saw the pause blocks while open", conv.processed > before)

# 4 backlog: the worker is behind -> drop and resynchronise
conv = EchoConverter()
s = VcSession(conv, cfg)
s.feed(block(-20))
dropped = s.feed(block(-20), backlog_ms=cfg.max_backlog_ms + 20)
check("backlog drops audio", dropped.size == 0 and s.stats["dropped"] == 1)
check("backlog resets an active stream", not s.active and conv.resets == 2)
again = s.feed(block(-20))
check("next loud block starts a fresh phrase", s.active and again.size > 0 and s.stats["phrases"] == 2)

print(f"\nRESULT: {passed} passed, {failed} failed")
sys.exit(1 if failed else 0)
