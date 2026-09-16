"""Voice changer service inside the backend process.

Threads
    reader (server.main)  configure() / mic(): enqueue only, never blocks, never imports torch
    gbtts-vc (own worker)  downloads, model load, target embedding, conversion, sends 0x04 frames

It is independent of the TTS worker: speaking with the TTS while the voice changer runs is
allowed (both use the CPU; the plugin mixes the TTS on top of the converted voice).
"""

from __future__ import annotations

import logging
import threading
import time
import traceback
from collections import deque
from pathlib import Path
from typing import Callable

import numpy as np

log = logging.getLogger("gbtts.vc")

# End-to-end algorithmic latency of the backend path at 48 kHz (model + resampling), measured
# by vc_lab/engine_bench.py (vault: voice-changer). Used by the plugin to size its hold.
LATENCY_MS = {"40ms": 200, "120ms": 280}     # FIR resampler, p50 (p95 +40/+100 ms)
MAX_PENDING_BLOCKS = 150           # 3 s of 20 ms blocks while the worker is busy loading


class VoiceChangerService:
    def __init__(self, conn, home: Path, store, emit: Callable[..., None]):
        self.conn = conn
        self.home = Path(home)
        self.store = store
        self.emit = emit
        self._lock = threading.Lock()
        self._wake = threading.Event()
        self._desired: dict | None = None
        self._mic: deque = deque()
        self._jobs: deque = deque()            # vc_add_target ops (file I/O off the reader thread)
        self._accepting = False            # mic blocks are queued only when a session is live
        self._conv = None
        self._session = None
        self._applied: dict = {}
        self._last_stats = 0.0
        self._thread = threading.Thread(target=self._loop, name="gbtts-vc", daemon=True)
        self._thread.start()

    # ---- reader thread --------------------------------------------------------------
    def configure(self, op: dict) -> None:
        with self._lock:
            self._desired = {
                "enabled": bool(op.get("enabled", False)),
                "voice": str(op.get("voice", "") or ""),
                "preset": str(op.get("preset", "120ms") or "120ms"),
                "threads": int(op.get("threads", 4) or 4),
            }
            if not self._desired["enabled"]:
                self._accepting = False
                self._mic.clear()
        self._wake.set()

    def list_targets(self, req=None) -> None:
        """Every voice of the library can be a target, whatever TTS engine is active."""
        voices = [{"id": f"clone:{v['id']}", "name": v["name"], "seconds": v.get("seconds", 0),
                   "source": v.get("source", "")} for v in self.store.list()]
        self.emit("vc_targets", voices=voices, req=req)

    def add_target(self, op: dict) -> None:
        with self._lock:
            self._jobs.append(op)
        self._wake.set()

    def mic(self, seq: int, pcm: np.ndarray) -> None:
        with self._lock:
            if not self._accepting:
                return
            self._mic.append((seq, pcm))
            while len(self._mic) > MAX_PENDING_BLOCKS:
                self._mic.popleft()
        self._wake.set()

    # ---- worker ------------------------------------------------------------------------
    def _status(self, state: str, message: str = "", **extra) -> None:
        self.emit("vc_status", state=state, message=message, **extra)

    def _loop(self) -> None:
        while True:
            self._wake.wait(timeout=0.5)
            self._wake.clear()
            with self._lock:
                desired, self._desired = self._desired, None
                jobs = list(self._jobs)
                self._jobs.clear()
            for job in jobs:
                self._add_target(job)
            if desired is not None:
                try:
                    self._apply(desired)
                except Exception as exc:  # noqa: BLE001 - never kill the worker
                    log.error("voice changer configure failed: %s\n%s", exc, traceback.format_exc())
                    self._stop_session()
                    self._status("error", f"Voice changer non avviato: {exc}")
            self._drain()

    def _add_target(self, op: dict) -> None:
        """A recording becomes a voice-library entry usable as a target (no Qwen needed)."""
        import soundfile as sf
        req = op.get("req")
        try:
            path = str(op.get("path", ""))
            wav, sr = sf.read(path, dtype="float32", always_2d=False)
            wav = np.asarray(wav, dtype=np.float32)
            if wav.ndim > 1:
                wav = wav.mean(axis=1)
            name = str(op.get("name", "")).strip() or Path(path).stem
            entry = self.store.add(name=name, audio=wav, sr=int(sr), ref_text="", lang="", source="vc")
            self.emit("vc_target_added", req=req, voice={"id": f"clone:{entry['id']}", "name": entry["name"]})
            self.list_targets()
        except Exception as exc:  # noqa: BLE001
            log.warning("vc target not added: %s", exc)
            self.emit("op_error", op="vc_add_target", req=req, message=f"voce non aggiunta: {exc}")

    def _stop_session(self) -> None:
        with self._lock:
            self._accepting = False
            self._mic.clear()
        self._session = None

    def _apply(self, cfg: dict) -> None:
        from .engine import MODELS, VoiceConverter, download, files_present
        from .session import VcSession

        if not cfg["enabled"]:
            self._stop_session()
            if self._conv is not None:
                self._conv.unload()
                self._conv = None
                self._applied = {}
            self._status("off")
            return
        preset = cfg["preset"] if cfg["preset"] in MODELS else "120ms"
        voice = cfg["voice"]
        if not voice.startswith("clone:"):
            raise ValueError("scegli una voce della tua libreria come voce di destinazione")
        ref = Path(self.store.voice_dir(voice[6:])) / "ref.wav"
        if not ref.is_file():
            raise ValueError("voce di destinazione non trovata")

        models = self.home / "models"
        self._stop_session()
        if not files_present(models, preset, with_speaker=False) or not (ref.with_name("meanvc2_spk.npy")).is_file():
            need_speaker = not ref.with_name("meanvc2_spk.npy").is_file()
            if not files_present(models, preset, with_speaker=need_speaker):
                self._status("downloading", "Download del modello voice changer…")
                download(models, [preset], lambda m: self._status("downloading", m), with_speaker=need_speaker)

        if self._conv is None or self._applied.get("preset") != preset or self._applied.get("threads") != cfg["threads"]:
            if self._conv is not None:
                self._conv.unload()
            self._status("loading", "Caricamento del voice changer…")
            conv = VoiceConverter(models, preset=preset, threads=cfg["threads"], resample_quality="FIR")
            conv.load()
            self._conv = conv
            self._applied = {"preset": preset, "threads": cfg["threads"]}
        if self._conv.target_id != voice:
            emb = self._conv.embedding_for(ref, say=lambda m: self._status("loading", m))
            self._conv.set_target(emb, voice)
        self._session = VcSession(self._conv)
        with self._lock:
            self._mic.clear()
            self._accepting = True
        self._status("ready", "Voice changer attivo", preset=preset, voice=voice, latency_ms=LATENCY_MS[preset])

    def _drain(self) -> None:
        session = self._session
        if session is None:
            return
        while True:
            with self._lock:
                if not self._mic:
                    break
                seq, pcm = self._mic.popleft()
                backlog_ms = sum(p.size for _, p in self._mic) * 1000 // 48000
            out = session.feed(pcm, backlog_ms=backlog_ms)
            if out.size:
                self.conn.send_vc(seq, out)
        now = time.monotonic()
        if now - self._last_stats > 5.0 and session.stats["blocks"]:
            self._last_stats = now
            self.emit("vc_stats", **session.stats)
