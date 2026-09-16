"""User voice library.

A voice is a short reference recording (<= 15 s, 24 kHz mono, a few hundred
KB) plus its transcript. The model-specific clone prompt computed from it is
cached next to it (prompt_<model>.pt, tens of KB) so switching between the
0.6B and 1.7B model never needs the user's original file again.

Layout:
    <home>/voices/voices.json
    <home>/voices/<id>/ref.wav
    <home>/voices/<id>/prompt_1.7B.pt

Stock voices (gbtts/stock_voices, v1.5): original synthetic Italian voices shipped with the
plugin. install_stock() copies each one into the library ONCE; the slugs already offered are
remembered in voices.json, so a stock voice the user deleted does not come back.
"""

from __future__ import annotations

import json
import os
import re
import shutil
import threading
import time
import uuid
from pathlib import Path

import numpy as np
import soundfile as sf

REF_SR = 24000
MAX_REF_SECONDS = 15.0


def _slug_ok(voice_id: str) -> bool:
    return bool(re.fullmatch(r"[a-f0-9]{12}", voice_id))


class VoiceStore:
    def __init__(self, root: Path):
        self.root = Path(root)
        self.root.mkdir(parents=True, exist_ok=True)
        self._index_path = self.root / "voices.json"
        self._lock = threading.Lock()
        self._voices: list[dict] = []
        self._stock_offered: list[str] = []
        self._load()

    # ---- persistence --------------------------------------------------------
    def _load(self) -> None:
        if not self._index_path.exists():
            self._voices = []
            return
        try:
            data = json.loads(self._index_path.read_text(encoding="utf-8"))
            voices = data.get("voices", []) if isinstance(data, dict) else []
            self._voices = [v for v in voices if isinstance(v, dict) and _slug_ok(str(v.get("id", "")))
                            and (self.root / v["id"] / "ref.wav").exists()]
            offered = data.get("stock_offered", []) if isinstance(data, dict) else []
            self._stock_offered = [s for s in offered if isinstance(s, str)]
        except Exception:
            self._voices = []

    def _save(self) -> None:
        tmp = self._index_path.with_suffix(".json.tmp")
        tmp.write_text(json.dumps({"version": 1, "voices": self._voices, "stock_offered": self._stock_offered},
                                  ensure_ascii=False, indent=2), encoding="utf-8")
        os.replace(tmp, self._index_path)

    # ---- queries --------------------------------------------------------------
    def list(self) -> list[dict]:
        with self._lock:
            return [dict(v) for v in self._voices]

    def get(self, voice_id: str) -> dict | None:
        with self._lock:
            for v in self._voices:
                if v["id"] == voice_id:
                    return dict(v)
        return None

    def voice_dir(self, voice_id: str) -> Path:
        if not _slug_ok(voice_id):
            raise ValueError("invalid voice id")
        return self.root / voice_id

    def prompt_path(self, voice_id: str, model_tag: str) -> Path:
        tag = re.sub(r"[^A-Za-z0-9_.-]", "_", model_tag)
        return self.voice_dir(voice_id) / f"prompt_{tag}.pt"

    def load_reference(self, voice_id: str) -> tuple[np.ndarray, int]:
        wav, sr = sf.read(str(self.voice_dir(voice_id) / "ref.wav"), dtype="float32", always_2d=False)
        if wav.ndim > 1:
            wav = wav.mean(axis=1)
        return wav.astype(np.float32), int(sr)

    # ---- mutations -------------------------------------------------------------
    def add(self, *, name: str, audio: np.ndarray, sr: int, ref_text: str, lang: str,
            source: str, instruct: str = "", gender: str = "", description: str = "", stock: str = "") -> dict:
        from .audio import StreamResampler, trim_silence

        x = np.asarray(audio, dtype=np.float32).reshape(-1)
        if sr != REF_SR:
            rs = StreamResampler(sr, REF_SR)
            x = np.concatenate([rs.process(x), rs.flush()])
        x = trim_silence(x, REF_SR, threshold_db=-50.0, pad_ms=120.0)
        if x.size < REF_SR * 1.5:
            raise ValueError("reference too short (need at least 1.5 s of speech)")
        x = x[: int(REF_SR * MAX_REF_SECONDS)]
        peak = float(np.max(np.abs(x))) or 1.0
        x = (x * (0.707 / peak)).astype(np.float32)

        voice_id = uuid.uuid4().hex[:12]
        vdir = self.root / voice_id
        vdir.mkdir(parents=True, exist_ok=False)
        sf.write(str(vdir / "ref.wav"), x, REF_SR, subtype="PCM_16")
        entry = {
            "id": voice_id,
            "name": name.strip()[:60] or "Voce",
            "lang": lang,
            "ref_text": ref_text.strip(),
            "source": source,
            "instruct": instruct.strip(),
            "created": int(time.time()),
            "seconds": round(x.size / REF_SR, 2),
        }
        if gender in ("f", "m"):
            entry["gender"] = gender
        if description:
            entry["description"] = description.strip()
        if stock:
            entry["stock"] = stock
        with self._lock:
            self._voices.append(entry)
            if stock and stock not in self._stock_offered:
                self._stock_offered.append(stock)
            self._save()
        return dict(entry)

    def install_stock(self, stock_dir: Path) -> int:
        """Copy the shipped Italian voices not offered yet. Returns how many were added."""
        index = Path(stock_dir) / "stock_voices.json"
        if not index.exists():
            return 0
        data = json.loads(index.read_text(encoding="utf-8"))
        added = 0
        for sv in data.get("voices", []):
            slug = str(sv.get("slug", ""))
            if not slug or slug in self._stock_offered:
                continue
            gender = {"female": "f", "male": "m"}.get(sv.get("gender", ""), "")
            with self._lock:
                # Giulia and Marco were designed on the user's PC by v1.0-v1.4: adopt them
                # instead of adding a second voice with the same name.
                twin = next((v for v in self._voices if v["name"] == sv["name"] and v.get("source") == "design"
                             and not v.get("stock")), None)
                if twin is not None:
                    twin.update({"source": "stock", "stock": slug, "description": sv.get("description", "")})
                    if gender:
                        twin["gender"] = gender
                    self._stock_offered.append(slug)
                    self._save()
                    continue
            ref = Path(stock_dir) / slug / "ref.wav"
            if not ref.exists():
                continue
            wav, sr = sf.read(str(ref), dtype="float32", always_2d=False)
            self.add(name=sv["name"], audio=wav, sr=int(sr), ref_text=sv.get("ref_text", ""), lang=sv.get("lang", "it"),
                     source="stock", instruct=sv.get("instruct", ""), gender=gender,
                     description=sv.get("description", ""), stock=slug)
            added += 1
        return added

    def rename(self, voice_id: str, name: str) -> bool:
        with self._lock:
            for v in self._voices:
                if v["id"] == voice_id:
                    v["name"] = name.strip()[:60] or v["name"]
                    self._save()
                    return True
        return False

    def delete(self, voice_id: str) -> bool:
        with self._lock:
            before = len(self._voices)
            self._voices = [v for v in self._voices if v["id"] != voice_id]
            if len(self._voices) == before:
                return False
            self._save()
        shutil.rmtree(self.voice_dir(voice_id), ignore_errors=True)
        return True

    def drop_prompts(self, voice_id: str) -> None:
        for p in self.voice_dir(voice_id).glob("prompt_*.pt"):
            try:
                p.unlink()
            except OSError:
                pass
