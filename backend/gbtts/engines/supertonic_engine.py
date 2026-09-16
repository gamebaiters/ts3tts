"""Supertonic 3 (ONNX, CPU): instant fallback engine, zero VRAM, 31 languages."""

from __future__ import annotations

import logging
import os
from pathlib import Path
from typing import Iterator

import numpy as np

from .base import Engine, EngineError, StatusFn, SynthRequest

log = logging.getLogger("gbtts.supertonic")

_VOICE_NAMES = {
    "M1": "Uomo 1", "M2": "Uomo 2", "M3": "Uomo 3", "M4": "Uomo 4", "M5": "Uomo 5",
    "F1": "Donna 1", "F2": "Donna 2", "F3": "Donna 3", "F4": "Donna 4", "F5": "Donna 5",
}
_LANGS = set("en ko ja ar bg cs da de el es et fi fr hi hr hu id it lt lv nl pl pt ro ru sk sl sv tr uk vi".split())


class SupertonicEngine(Engine):
    key = "supertonic"
    label = "Supertonic 3 (CPU)"
    native_speed = True

    def __init__(self, models_dir: Path, steps: int = 10, threads: int | None = None):
        self.model_dir = Path(models_dir) / "supertonic3"
        self.steps = int(max(4, min(16, steps)))
        self.threads = threads or max(2, min(8, (os.cpu_count() or 4) // 2))
        self._tts = None
        self._styles: dict[str, object] = {}

    def available(self) -> tuple[bool, str]:
        try:
            import supertonic  # noqa: F401
        except Exception as exc:
            return False, f"supertonic non installato: {exc}"
        return True, ""

    def load(self, status: StatusFn) -> None:
        if self._tts is not None:
            return
        from supertonic import TTS
        present = self.model_dir.exists() and any(self.model_dir.rglob("*.onnx"))
        status("loading", "Caricamento Supertonic 3…" if present else "Download Supertonic 3 (~400 MB)…")
        try:
            self._tts = TTS(model="supertonic-3", model_dir=self.model_dir, auto_download=True,
                            intra_op_num_threads=self.threads)
        except Exception as exc:
            raise EngineError(f"Supertonic non caricato: {exc}") from exc
        log.info("supertonic ready, sr=%s voices=%s", self._tts.sample_rate, self._tts.voice_style_names)

    def unload(self) -> None:
        self._tts = None
        self._styles.clear()

    def is_loaded(self) -> bool:
        return self._tts is not None

    def voices(self) -> list[dict]:
        names = self._tts.voice_style_names if self._tts is not None else list(_VOICE_NAMES.keys())
        out = []
        for n in sorted(names):
            out.append({
                "id": n, "name": _VOICE_NAMES.get(n, n), "kind": "builtin", "lang": "multi",
                "gender": "f" if n.startswith("F") else "m" if n.startswith("M") else "",
                "engine": self.key, "description": "Voce integrata Supertonic (CPU, istantanea)",
            })
        return out

    def _style(self, name: str):
        if name not in self._styles:
            self._styles[name] = self._tts.get_voice_style(name)
        return self._styles[name]

    def stream(self, req: SynthRequest) -> Iterator[tuple[np.ndarray, int]]:
        if self._tts is None:
            raise EngineError("Supertonic non caricato")
        voice = req.voice if req.voice in _VOICE_NAMES else "F1"
        lang = req.lang if req.lang in _LANGS else "na"
        speed = float(min(2.0, max(0.7, req.speed * 1.05)))
        wav, _dur = self._tts.synthesize(
            req.text, voice_style=self._style(voice), total_steps=self.steps, speed=speed,
            max_chunk_length=300, silence_duration=0.12, lang=lang, verbose=False,
        )
        yield np.asarray(wav, dtype=np.float32).reshape(-1), int(self._tts.sample_rate)

    def info(self) -> dict:
        return {"engine": self.key, "label": self.label, "device": f"CPU ({self.threads} thread)",
                "model": "supertonic-3"}
