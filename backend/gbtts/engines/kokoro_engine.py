"""Kokoro-82M (ONNX, CPU): the light engine.

~325 MB fp32 model + 28 MB voices, no PyTorch, no GPU. Two native Italian voices
(if_sara, im_nicola) plus English/Spanish/French/Portuguese/Hindi ones.

Two details matter for latency:
  * phonemizer.phonemize() builds a new espeak backend on EVERY call (~250 ms
    measured). One EspeakBackend per language is kept and reused (few ms).
  * the ONNX session is created here with an explicit thread count, so a game
    running on the same CPU keeps its cores.
"""

from __future__ import annotations

import importlib.util
import logging
import os
import threading
import time
import urllib.request
from pathlib import Path
from typing import Callable, Iterator

import numpy as np

from .base import Engine, EngineError, StatusFn, SynthRequest

log = logging.getLogger("gbtts.kokoro")

RELEASE = "https://github.com/thewh1teagle/kokoro-onnx/releases/download/model-files-v1.1/"
MODEL_FILES = {
    "int8": ("kokoro-v1.0.int8.onnx", 114_119_327),
    "fp32": ("kokoro-v1.0.onnx", 325_505_369),
}
VOICES_FILE = ("voices-v1.0.bin", 28_214_398)

# Voice id prefix letter -> (espeak language, label). Japanese and Chinese voices
# need the misaki G2P, which is not shipped: they are not offered.
VOICE_LANGS = {
    "i": ("it", "IT"), "a": ("en-us", "EN-US"), "b": ("en-gb", "EN-GB"), "e": ("es", "ES"),
    "f": ("fr-fr", "FR"), "p": ("pt-br", "PT-BR"), "h": ("hi", "HI"),
}
ESPEAK_LANG = {"it": "it", "en": "en-us", "es": "es", "fr": "fr-fr", "pt": "pt-br", "de": "de", "ru": "ru"}
STATIC_VOICES = ["if_sara", "im_nicola", "af_heart", "af_bella", "am_michael", "bf_emma", "bm_george",
                 "ef_dora", "em_alex", "ff_siwis", "pf_dora", "pm_alex"]


def kokoro_dir(models_dir: Path) -> Path:
    return Path(models_dir) / "kokoro"


def files_present(models_dir: Path, variant: str) -> bool:
    d = kokoro_dir(models_dir)
    for name, size in (MODEL_FILES[variant], VOICES_FILE):
        p = d / name
        if not p.exists() or p.stat().st_size != size:
            return False
    return True


def download_kokoro(models_dir: Path, variant: str, say: Callable[[str], None]) -> None:
    """Resumable download with exact size verification (GitHub release assets)."""
    from ..netfix import use_system_certificates
    use_system_certificates()
    d = kokoro_dir(models_dir)
    d.mkdir(parents=True, exist_ok=True)
    for name, size in (MODEL_FILES[variant], VOICES_FILE):
        target = d / name
        if target.exists() and target.stat().st_size == size:
            continue
        part = d / (name + ".part")
        have = part.stat().st_size if part.exists() else 0
        if have > size:
            part.unlink()
            have = 0
        req = urllib.request.Request(RELEASE + name, headers={"User-Agent": "gbtts"})
        if have:
            req.add_header("Range", f"bytes={have}-")
        say(f"Download {name} ({size / 2**20:.0f} MB)…")
        with urllib.request.urlopen(req, timeout=60) as resp:
            if have and resp.status != 206:
                have = 0          # server ignored the range: start over
            mode = "ab" if have else "wb"
            with open(part, mode) as fh:
                done, last = have, -1
                while True:
                    chunk = resp.read(1 << 20)
                    if not chunk:
                        break
                    fh.write(chunk)
                    done += len(chunk)
                    pct = int(done * 100 / size)
                    if pct // 10 != last:
                        last = pct // 10
                        say(f"{name}: {pct}%")
        if part.stat().st_size != size:
            raise EngineError(f"download incompleto di {name}: rilancia per riprendere")
        os.replace(part, target)


class KokoroEngine(Engine):
    key = "kokoro"
    label = "Kokoro 82M (CPU)"
    native_speed = True

    # fp32 is the default on purpose: on an i5-13600K the int8 export runs at
    # RTF 0.35 (its dynamic-quantisation kernels use ~one core) vs 2.5 for fp32.
    # int8 only saves download size.
    def __init__(self, models_dir: Path, variant: str = "fp32", threads: int | None = None):
        self.models_dir = Path(models_dir)
        self.variant = variant if variant in MODEL_FILES else "fp32"
        # Measured on a quiet i5-13600K (fp32, 6.5 s sentence): all cores RTF 5.5,
        # 8 threads 5.1, 6 threads 4.4, 4 threads 3.9. Eight keeps most of the speed
        # and leaves half the CPU to a game.
        self.threads = threads or max(2, min(8, (os.cpu_count() or 4) // 2))
        self._kokoro = None
        self._loaded_variant: str | None = None
        self._backends: dict[str, object] = {}
        self._lock = threading.Lock()

    def available(self) -> tuple[bool, str]:
        for mod in ("kokoro_onnx", "onnxruntime", "phonemizer"):
            if importlib.util.find_spec(mod) is None:
                return False, f"{mod} non installato"
        return True, ""

    def is_loaded(self) -> bool:
        return self._kokoro is not None

    def load(self, status: StatusFn) -> None:
        if self._kokoro is not None and self._loaded_variant == self.variant:
            return
        self.unload()
        ok, why = self.available()
        if not ok:
            raise EngineError(why)
        if not files_present(self.models_dir, self.variant):
            download_kokoro(self.models_dir, self.variant, lambda m: status("loading", m))
        status("loading", "Caricamento Kokoro…")
        t0 = time.perf_counter()
        import onnxruntime as rt
        from kokoro_onnx import Kokoro

        so = rt.SessionOptions()
        so.intra_op_num_threads = self.threads
        so.inter_op_num_threads = 1
        so.graph_optimization_level = rt.GraphOptimizationLevel.ORT_ENABLE_ALL
        # Every sentence has a different token count. With the default memory
        # pattern ORT re-plans for each new shape: measured RTF 3.4 with chunks as
        # slow as 2.0x, vs a steady 5.0-5.1 (worst chunk 4.1x) with it disabled.
        so.enable_mem_pattern = False
        so.enable_cpu_mem_arena = True
        # Spinning worker threads burnt 2.2-2.4 cores for a second after EVERY
        # inference (measured) and were not even faster: RTF 5.1 on vs 5.6 off.
        so.add_session_config_entry("session.intra_op.allow_spinning", "0")
        d = kokoro_dir(self.models_dir)
        try:
            sess = rt.InferenceSession(str(d / MODEL_FILES[self.variant][0]), sess_options=so,
                                       providers=["CPUExecutionProvider"])
            self._kokoro = Kokoro.from_session(sess, str(d / VOICES_FILE[0]))
        except Exception as exc:  # noqa: BLE001
            self._kokoro = None
            raise EngineError(f"Kokoro non caricato: {exc}") from exc
        self._loaded_variant = self.variant
        # First inference allocates the ORT arenas: pay it now, not on the first message.
        try:
            for _ in self.stream(SynthRequest(text="Ciao.", lang="it", voice="if_sara")):
                pass
        except Exception as exc:  # noqa: BLE001
            log.warning("kokoro warm-up failed: %s", exc)
        log.info("kokoro %s ready in %.1fs (%d threads)", self.variant, time.perf_counter() - t0, self.threads)

    def unload(self) -> None:
        self._kokoro = None
        self._loaded_variant = None
        self._backends.clear()

    def voices(self) -> list[dict]:
        names = STATIC_VOICES
        if self._kokoro is not None:
            names = self._kokoro.get_voices()
        else:
            vf = kokoro_dir(self.models_dir) / VOICES_FILE[0]
            if vf.exists():
                try:
                    names = sorted(np.load(str(vf)).files)
                except Exception:
                    names = STATIC_VOICES
        out = []
        # Italian first, then the rest alphabetically.
        for n in sorted(names, key=lambda v: (not v.startswith("i"), v)):
            if n[:1] not in VOICE_LANGS or "_" not in n:
                continue
            espeak_lang, label = VOICE_LANGS[n[0]]
            gender = "f" if n[1:2] == "f" else "m"
            base = n.split("_", 1)[1].replace("_", " ").title()
            # The UI groups by language; only English/Portuguese need the accent in the name.
            region = {"a": "US", "b": "UK", "p": "BR"}.get(n[0])
            out.append({
                "id": n, "name": f"{base} ({region})" if region else base, "kind": "builtin",
                "lang": espeak_lang.split("-")[0], "gender": gender, "engine": self.key,
                "description": f"Voce Kokoro integrata, {'donna' if gender == 'f' else 'uomo'}, {label} "
                               "— leggera, solo CPU",
            })
        return out

    def _backend(self, lang: str):
        be = self._backends.get(lang)
        if be is None:
            from phonemizer.backend import EspeakBackend
            be = EspeakBackend(language=lang, preserve_punctuation=True, with_stress=True,
                               language_switch="remove-flags")
            self._backends[lang] = be
        return be

    def _phonemes(self, text: str, lang: str) -> str:
        # espeak-ng keeps process-global state: serialise (kokoro_onnx does the same).
        from kokoro_onnx.tokenizer import _espeak_lock
        with _espeak_lock:
            ph = self._backend(lang).phonemize([text], strip=True)[0]
        vocab = self._kokoro.tokenizer.vocab
        return "".join(p for p in ph if p in vocab).strip()

    @staticmethod
    def _first_clause_split(text: str) -> list[str]:
        """Kokoro is not streaming: the first audio appears only once a whole
        segment is synthesised (~1.7 s for a sentence, measured). Splitting off
        the first clause at a comma/semicolon/colon makes the voice start after
        the short clause while the rest is generated (at RTF ~5 a 1.3 s clause
        is ready in ~0.25 s). Very short texts stay whole."""
        if len(text) < 30:
            return [text]
        for i, ch in enumerate(text):
            if ch in ",;:" and 8 <= i <= 90 and len(text) - i > 10:
                return [text[: i + 1].strip(), text[i + 1:].strip()]
        return [text]

    def stream(self, req: SynthRequest) -> Iterator[tuple[np.ndarray, int]]:
        if self._kokoro is None:
            raise EngineError("Kokoro non caricato")
        voice = req.voice if req.voice in self._kokoro.voices else "if_sara"
        if req.lang in ESPEAK_LANG:
            lang = ESPEAK_LANG[req.lang]
        else:
            lang = VOICE_LANGS.get(voice[:1], ("it", ""))[0]
        speed = float(min(2.0, max(0.5, req.speed)))
        parts = self._first_clause_split(req.text)
        for idx, part in enumerate(parts):
            if req.cancel.is_set():
                return
            phonemes = self._phonemes(part, lang)
            if not phonemes:
                continue
            with self._lock:
                audio, sr = self._kokoro.create(phonemes, voice=voice, speed=speed, is_phonemes=True, trim=True,
                                                sentence_pause=0.22, clause_pause=0.08)
            audio = np.asarray(audio, dtype=np.float32).reshape(-1)
            if idx + 1 < len(parts):
                # trim=True removed the model's own trailing silence: put back the
                # natural pause a comma asks for.
                audio = np.concatenate([audio, np.zeros(int(sr * 0.09 / speed), dtype=np.float32)])
            yield audio, int(sr)

    def info(self) -> dict:
        return {"engine": self.key, "label": self.label, "device": f"CPU ({self.threads} thread)",
                "model": f"Kokoro 82M {self.variant}"}
