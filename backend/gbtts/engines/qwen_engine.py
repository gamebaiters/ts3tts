"""Qwen3-TTS (12 Hz codec) through faster-qwen3-tts CUDA graphs.

Model roles
    Base          voice cloning from a reference clip  -> every user voice
    VoiceDesign   voice from a text description        -> only while designing
    CustomVoice   9 stock speakers + style instructions

Only ONE Qwen model lives in VRAM at a time (a 1.7B model plus its captured
graphs is ~5 GB; two would not leave room for a game on a 10 GB card). Daily
use only ever needs Base, so swaps happen only while designing a voice.
"""

from __future__ import annotations

import gc
import logging
import os
import time
from pathlib import Path
from typing import Iterator

import numpy as np

from ..textproc import LANGUAGES
from ..voices import VoiceStore
from .base import Engine, EngineError, StatusFn, SynthRequest

log = logging.getLogger("gbtts.qwen")

REPO = "Qwen/Qwen3-TTS-12Hz-{size}-{role}"
ROLES = {"base": "Base", "design": "VoiceDesign", "custom": "CustomVoice"}
CUSTOM_SPEAKERS = {
    "vivian": "Vivian", "serena": "Serena", "uncle_fu": "Uncle Fu", "dylan": "Dylan", "eric": "Eric",
    "ryan": "Ryan", "aiden": "Aiden", "ono_anna": "Ono Anna", "sohee": "Sohee",
}
# (gender, native language) of the stock speakers, from the CustomVoice model card.
CUSTOM_SPEAKER_INFO = {
    "vivian": ("f", "zh"), "serena": ("f", "zh"), "uncle_fu": ("m", "zh"), "dylan": ("m", "zh"),
    "eric": ("m", "zh"), "ryan": ("m", "en"), "aiden": ("m", "en"), "ono_anna": ("f", "ja"), "sohee": ("f", "ko"),
}


class QwenEngine(Engine):
    key = "qwen"
    label = "Qwen3-TTS (GPU)"
    native_speed = False

    def __init__(self, voices: VoiceStore, size: str = "1.7B", chunk_size: int = 4,
                 temperature: float = 0.8, top_k: int = 50, repetition_penalty: float = 1.05,
                 full_text_prefill: bool = True):
        self.store = voices
        self.size = size if size in ("1.7B", "0.6B") else "1.7B"
        self.chunk_size = int(max(1, min(12, chunk_size)))
        self.temperature = float(temperature)
        self.top_k = int(top_k)
        self.repetition_penalty = float(repetition_penalty)
        self.full_text_prefill = bool(full_text_prefill)
        self._model = None
        self._role: str | None = None
        self._model_size: str | None = None
        self._prompts: dict[tuple[str, str], tuple[dict, str]] = {}

    # ---- availability / lifecycle ----------------------------------------------
    def available(self) -> tuple[bool, str]:
        # Deliberately import-free: listing engines must not pull PyTorch (~1 GB of
        # RAM) into a backend that only runs Kokoro. The real CUDA check happens
        # when the model is loaded.
        import importlib.util
        for mod in ("torch", "faster_qwen3_tts"):
            if importlib.util.find_spec(mod) is None:
                return False, f"{mod} non installato"
        if os.name == "nt":
            import ctypes
            try:
                ctypes.WinDLL("nvcuda.dll")
            except OSError:
                return False, "Nessuna GPU NVIDIA CUDA disponibile"
        return True, ""

    def is_loaded(self) -> bool:
        return self._model is not None

    def loaded_role(self) -> str | None:
        return self._role

    def load(self, status: StatusFn) -> None:
        self.ensure("base", status)

    def unload(self) -> None:
        if self._model is None:
            return
        import torch
        self._model = None
        self._role = None
        self._model_size = None
        self._prompts.clear()
        gc.collect()
        torch.cuda.empty_cache()
        log.info("qwen model unloaded")

    def ensure(self, role: str, status: StatusFn) -> None:
        size = "1.7B" if role == "design" else self.size
        if self._model is not None and self._role == role and self._model_size == size:
            return
        ok, why = self.available()
        if not ok:
            raise EngineError(why)
        self.unload()
        import torch
        if not torch.cuda.is_available():
            raise EngineError("PyTorch non vede nessuna GPU CUDA")
        from faster_qwen3_tts import FasterQwen3TTS

        repo = REPO.format(size=size, role=ROLES[role])
        human = {"base": "voci", "design": "progettazione voci", "custom": "voci integrate"}[role]
        status("loading", f"Caricamento Qwen3-TTS {size} ({human})…")
        t0 = time.perf_counter()
        kwargs = dict(device="cuda", dtype=torch.bfloat16, attn_implementation="sdpa", max_seq_len=2048)
        # Load from the local snapshot DIRECTORY when it exists: with a repo id,
        # qwen-tts does not propagate local_files_only to its processor/tokenizer
        # loads and every start made ~20 HTTP requests to Hugging Face (v1.0 log).
        from huggingface_hub import snapshot_download
        try:
            source = snapshot_download(repo, local_files_only=True)
        except Exception:  # noqa: BLE001 - not downloaded yet
            source = None
        if source is None:
            status("loading", f"Download {repo} (solo la prima volta)…")
            try:
                source = snapshot_download(repo)
            except Exception as exc:  # noqa: BLE001
                raise EngineError(f"Impossibile scaricare {repo}: {exc}") from exc
        try:
            model = FasterQwen3TTS.from_pretrained(source, local_files_only=True, **kwargs)
        except Exception as exc:  # noqa: BLE001
            raise EngineError(f"Impossibile caricare {repo}: {exc}") from exc
        status("loading", "Preparazione grafi CUDA…")
        model.warmup(prefill_len=100)
        self._model, self._role, self._model_size = model, role, size
        free, total = torch.cuda.mem_get_info()
        log.info("loaded %s in %.1fs, VRAM used %.0f/%.0f MB, languages=%s", repo,
                 time.perf_counter() - t0, (total - free) / 2**20, total / 2**20,
                 model.model.get_supported_languages())

    # ---- voices --------------------------------------------------------------------
    @staticmethod
    def model_downloaded(role: str, size: str) -> bool:
        try:
            from huggingface_hub import try_to_load_from_cache
            path = try_to_load_from_cache(REPO.format(size=size, role=ROLES[role]), "config.json")
            return isinstance(path, str)
        except Exception:
            return False

    def voices(self) -> list[dict]:
        out = []
        for v in self.store.list():
            desc = v.get("description") or v.get("instruct") or ("Clonata da file audio" if v.get("source") == "file" else "")
            out.append({"id": f"clone:{v['id']}", "name": v["name"], "kind": "clone", "lang": v.get("lang", "it"),
                        "gender": v.get("gender", ""), "source": v.get("source", ""),
                        "engine": self.key, "description": desc, "seconds": v.get("seconds", 0)})
        custom_ready = self.model_downloaded("custom", self.size)
        for sid, name in CUSTOM_SPEAKERS.items():
            desc = "Voce integrata Qwen (accento non italiano, segue lo stile)"
            if not custom_ready:
                desc += f" - richiede il download del modello CustomVoice {self.size} (~4 GB) al primo uso"
            gender, native = CUSTOM_SPEAKER_INFO.get(sid, ("", "multi"))
            out.append({"id": f"spk:{sid}", "name": name, "kind": "builtin" if custom_ready else "builtin_download",
                        "lang": native, "gender": gender, "engine": self.key, "description": desc})
        return out

    # v1.0-v1.4 designed Giulia and Marco on the user's GPU at the first start (a VoiceDesign
    # model load, ~3 min on a cold cache). Since v1.5 they ship ready-made with the other
    # Italian stock voices (gbtts/stock_voices, tools/make_stock_voices.py).

    def _language(self, code: str) -> str:
        return LANGUAGES.get(code, "Auto") if code != "auto" else "Auto"

    def _max_tokens(self, text: str) -> int:
        # 12 codec frames per second; speech is ~14 chars/s -> ~0.85 frames/char.
        # 2.5x headroom stops a run-away generation without clipping slow reads.
        return int(min(2000, 48 + len(text) * 2.2))

    def _prompt_for(self, voice_id: str, status: StatusFn) -> tuple[dict, str]:
        import torch
        key = (voice_id, self.size)
        if key in self._prompts:
            return self._prompts[key]
        entry = self.store.get(voice_id)
        if entry is None:
            raise EngineError("Voce non trovata")
        path = self.store.prompt_path(voice_id, self.size)
        prompt = None
        if path.exists():
            try:
                data = torch.load(str(path), map_location="cpu", weights_only=False)
                prompt = data
            except Exception as exc:
                log.warning("prompt cache unreadable (%s), rebuilding", exc)
        if prompt is None:
            status("busy", f"Preparazione voce “{entry['name']}”…")
            wav, sr = self.store.load_reference(voice_id)
            ref_text = entry.get("ref_text") or ""
            items = self._model.model.create_voice_clone_prompt(
                ref_audio=(wav, sr), ref_text=ref_text or None, x_vector_only_mode=not bool(ref_text))
            it = items[0]
            prompt = {
                "ref_code": it.ref_code.detach().cpu() if it.ref_code is not None else None,
                "ref_spk_embedding": it.ref_spk_embedding.detach().cpu(),
                "icl": bool(it.icl_mode),
                "ref_text": ref_text,
            }
            try:
                torch.save(prompt, str(path))
            except Exception as exc:
                log.warning("cannot cache prompt: %s", exc)
        dev = self._model.device
        vcp = {
            "ref_code": [prompt["ref_code"].to(dev) if prompt["ref_code"] is not None else None],
            "ref_spk_embedding": [prompt["ref_spk_embedding"].to(dev)],
            "x_vector_only_mode": [not prompt["icl"]],
            "icl_mode": [bool(prompt["icl"])],
        }
        self._prompts[key] = (vcp, prompt["ref_text"])
        return self._prompts[key]

    def create_voice_prompt(self, voice_id: str, status: StatusFn) -> None:
        """Build + cache the prompt now so the first real use is instant."""
        self.ensure("base", status)
        self._prompt_for(voice_id, status)

    # ---- synthesis ------------------------------------------------------------------
    def stream(self, req: SynthRequest) -> Iterator[tuple[np.ndarray, int]]:
        import torch
        noop = lambda *_a: None  # noqa: E731
        language = self._language(req.lang)
        common = dict(
            max_new_tokens=self._max_tokens(req.text), temperature=self.temperature, top_k=self.top_k,
            repetition_penalty=self.repetition_penalty, chunk_size=self.chunk_size,
            non_streaming_mode=self.full_text_prefill,
        )
        if req.voice.startswith("spk:"):
            self.ensure("custom", noop)
            gen = self._model.generate_custom_voice_streaming(
                text=req.text, speaker=req.voice[4:], language=language,
                instruct=req.instruct or None, **common)
        elif req.voice.startswith("clone:"):
            self.ensure("base", noop)
            vcp, ref_text = self._prompt_for(req.voice[6:], noop)
            gen = self._model.generate_voice_clone_streaming(
                text=req.text, language=language, voice_clone_prompt=vcp, ref_text=ref_text,
                instruct=req.instruct or None, **common)
        else:
            raise EngineError(f"Voce Qwen sconosciuta: {req.voice}")

        max_audio_s = 4.0 + len(req.text) / 6.0
        produced = 0
        sr_out = 24000
        with torch.inference_mode():
            try:
                for chunk, sr, _timing in gen:
                    if req.cancel.is_set():
                        break
                    sr_out = int(sr)
                    a = np.asarray(chunk, dtype=np.float32).reshape(-1)
                    produced += a.size
                    yield a, sr_out
                    if produced / sr_out > max_audio_s:
                        log.warning("run-away generation stopped after %.1fs for %d chars",
                                    produced / sr_out, len(req.text))
                        break
            finally:
                gen.close()

    def design(self, text: str, instruct: str, lang: str, status: StatusFn) -> tuple[np.ndarray, int]:
        import torch
        self.ensure("design", status)
        status("busy", "Generazione anteprima voce…")
        with torch.inference_mode():
            wavs, sr = self._model.generate_voice_design(
                text=text, instruct=instruct, language=self._language(lang),
                max_new_tokens=self._max_tokens(text), temperature=0.9, top_k=50, repetition_penalty=1.05)
        return np.asarray(wavs[0], dtype=np.float32).reshape(-1), int(sr)

    def info(self) -> dict:
        d = {"engine": self.key, "label": self.label, "model": f"Qwen3-TTS {self._model_size or self.size}",
             "role": self._role or ""}
        try:
            import torch
            if torch.cuda.is_available():
                free, total = torch.cuda.mem_get_info()
                d["device"] = torch.cuda.get_device_name(0)
                d["vram_used_mb"] = int((total - free) / 2**20)
                d["vram_total_mb"] = int(total / 2**20)
        except Exception:
            pass
        return d
