"""Streaming voice conversion with MeanVC2 (18 M parameters, zero-shot, Apache-2.0).

Pipeline per call (all on the CPU, see "Why CPU" below):

    48 kHz int16 mic ─soxr→ 16 kHz ─kaldi fbank→ Fast-U2++ streaming ASR (JIT) → bottleneck features
        → 4x linear upsample → DiT mean-flow decoder (KV cache, 2 steps) → 80-bin mel
        → Vocos (JIT) → 16 kHz ─soxr→ 48 kHz int16

The target voice is a 256-dim WavLM-Large/ECAPA-TDNN embedding computed ONCE from a
reference recording and cached next to it; the 1.2 GB speaker model is loaded only for
that and freed again.

Why CPU: measured on an RTX 3080 + i5-13600K (vault: voice-changer), moving the DiT and
vocoder to CUDA gave RTF 0.78 vs 0.75 on 4 CPU threads - the JIT ASR encoder was traced on
CPU (device constants baked in) and dominates. Staying on the CPU also means zero VRAM and
no cuDNN switch that would slow Qwen in the same process.

Fix over upstream runtime/run_rt.py: the BN->mel upsampling produced a FIXED 16 frames per
call, which stretched the 40 ms model's audio 1.95x. Here the size follows the frames
actually produced (4 mel frames per BN frame, as upstream's offline infer_e2e.py does).
"""

from __future__ import annotations

import gc
import json
import logging
import threading
from pathlib import Path
from typing import Callable

import numpy as np

log = logging.getLogger("gbtts.vc")

HERE = Path(__file__).resolve().parent
THIRD = HERE / "third_party"

HF_REPO = "ASLP-lab/MeanVC2"
SPEAKER_REPO = "yfyeung/wavlm-large-speaker-verification"
SPEAKER_FILE = "wavlm-large.pt"
SPEAKER_SIZE = 1_301_926_579
EMBEDDING_FILE = "meanvc2_spk.npy"          # cached next to a voice's reference audio

MODELS = {
    # latency preset: VC chunk + 40 ms future; ASR window / stride in fbank frames
    "40ms": dict(ckpt="meanvc2_40ms_40ms.safetensors", config="config_40ms_40ms.json", asr="fastu2pp_80ms.pt",
                 bn_window=11, bn_stride=8, cache=4, offset_init=4, offset_step=2, chunk=4),
    "120ms": dict(ckpt="meanvc2_120ms_40ms.safetensors", config="config_120ms_40ms.json", asr="fastu2pp_160ms.pt",
                  bn_window=19, bn_stride=16, cache=8, offset_init=8, offset_step=4, chunk=12),
}
BLOCK = 4            # future mel frames the decoder sees
VOCODER = "vocos.pt"


def model_dir(models: Path) -> Path:
    return Path(models) / "meanvc2"


def files_present(models: Path, preset: str, with_speaker: bool = False) -> bool:
    d = model_dir(models)
    p = MODELS[preset]
    need = [p["ckpt"], p["asr"], VOCODER] + (["wavlm_large_finetune.pth"] if with_speaker else [])
    return all((d / f).is_file() and (d / f).stat().st_size > 0 for f in need)


def download(models: Path, presets: list[str], say: Callable[[str], None], with_speaker: bool = True) -> None:
    """Weights into <models>/meanvc2 (no global HF cache)."""
    from huggingface_hub import hf_hub_download

    from ..netfix import use_system_certificates
    use_system_certificates()
    d = model_dir(models)
    d.mkdir(parents=True, exist_ok=True)
    files = {VOCODER}
    for preset in presets:
        files |= {MODELS[preset]["ckpt"], MODELS[preset]["asr"]}
    for name in sorted(files):
        if not (d / name).is_file():
            say(f"Download {name}…")
            hf_hub_download(HF_REPO, name, local_dir=str(d))
    target = d / "wavlm_large_finetune.pth"
    if with_speaker and not (target.is_file() and target.stat().st_size == SPEAKER_SIZE):
        say("Download del modello di riconoscimento voce (1,2 GB, solo la prima volta)…")
        path = hf_hub_download(SPEAKER_REPO, SPEAKER_FILE, local_dir=str(d))
        Path(path).replace(target)
        if target.stat().st_size != SPEAKER_SIZE:
            raise RuntimeError("download incompleto del modello di riconoscimento voce: riprova")


class FirResampler3:
    """Streaming 48 kHz <-> 16 kHz by an integer factor 3 with a short linear-phase FIR.

    soxr's streaming resamplers hold output back (measured with 20 ms blocks: HQ ~31 ms,
    QQ ~10 ms per direction). A 95-tap Kaiser FIR delays by 47 samples at 48 kHz (~1 ms)
    and emits every sample as soon as its input arrives. Cutoff 6.8 kHz: the converted
    voice is 16 kHz band-limited anyway, and the ASR front end only needs < 8 kHz.
    Same `resample_chunk` method as soxr.ResampleStream.
    """

    TAPS = 95

    def __init__(self, direction: str):
        from scipy.signal import firwin
        if direction not in ("down", "up"):
            raise ValueError(direction)
        self.direction = direction
        self.taps = firwin(self.TAPS, 6800.0, fs=48000.0, window=("kaiser", 8.0)).astype(np.float32)
        if direction == "up":
            self.taps *= 3.0                      # compensate the zero stuffing
        self.zi = np.zeros(self.TAPS - 1, dtype=np.float32)
        self.phase = 0                            # index (in the next chunk) of the next kept sample

    def resample_chunk(self, x: np.ndarray) -> np.ndarray:
        from scipy.signal import lfilter
        x = np.asarray(x, dtype=np.float32)
        if x.size == 0:
            return x
        if self.direction == "down":
            y, self.zi = lfilter(self.taps, 1.0, x, zi=self.zi)
            out = y[self.phase::3]
            self.phase = (self.phase - x.size) % 3
            return out.astype(np.float32)
        z = np.zeros(x.size * 3, dtype=np.float32)
        z[::3] = x
        y, self.zi = lfilter(self.taps, 1.0, z, zi=self.zi)
        return y.astype(np.float32)


class VoiceConverter:
    """One streaming conversion session. Not thread-safe: use from one worker thread."""

    SAMPLE_RATE = 48000

    def __init__(self, models: Path, preset: str = "40ms", threads: int = 4, steps: int = 2,
                 resample_quality: str = "FIR"):
        if preset not in MODELS:
            raise ValueError(f"preset sconosciuto: {preset}")
        self.models = Path(models)
        self.preset = preset
        self.threads = int(max(1, threads))
        self.steps = int(max(1, min(4, steps)))
        # Measured on 20 Italian sentences x 2 targets (40 ms preset, 48 kHz path):
        #   HQ  lag 237 ms  CER 0.004-0.008    LQ lag 219 ms  CER 0.004
        #   QQ  lag 200 ms  CER 0.012-0.013    FIR lag 200 ms CER 0.004-0.005  <- default
        # (soxr streams hold output back ~10-31 ms per direction; the FIR does not.)
        self.resample_quality = resample_quality
        self._p = MODELS[preset]
        self._lock = threading.Lock()
        self.asr = self.vc = self.vocoder = None
        self.spk = self.gtm_kv = None
        self.target_id = ""

    # ---- lifecycle ---------------------------------------------------------------------
    def is_loaded(self) -> bool:
        return self.vc is not None

    def load(self) -> None:
        if self.vc is not None:
            return
        import torch
        from safetensors.torch import load_file

        from .third_party.meanvc2.dit import DiT

        torch.set_num_threads(self.threads)
        d = model_dir(self.models)
        self.asr = torch.jit.load(str(d / self._p["asr"]), map_location="cpu").eval()
        cfg = json.loads((THIRD / "meanvc2" / self._p["config"]).read_text(encoding="utf-8"))
        vc = DiT(**cfg["model"])
        missing, unexpected = vc.load_state_dict(load_file(str(d / self._p["ckpt"])), strict=False)
        if unexpected:
            log.warning("MeanVC2: %d unexpected keys in checkpoint", len(unexpected))
        self.vc = vc.float().eval()
        self.vocoder = torch.jit.load(str(d / VOCODER), map_location="cpu").eval()
        self._fade_down = np.linspace(1.0, 0.0, 160, dtype=np.float32)
        self._fade_up = np.linspace(0.0, 1.0, 160, dtype=np.float32)
        self.reset()
        log.info("MeanVC2 %s loaded (%d threads, %d steps)", self.preset, self.threads, self.steps)

    def unload(self) -> None:
        self.asr = self.vc = self.vocoder = None
        self.spk = self.gtm_kv = None
        self.target_id = ""
        gc.collect()

    # ---- target voice ---------------------------------------------------------------------
    def embedding_for(self, reference: Path, say: Callable[[str], None] | None = None) -> np.ndarray:
        """256-dim target embedding, cached as <reference dir>/meanvc2_spk.npy."""
        reference = Path(reference)
        cache = reference.with_name(EMBEDDING_FILE)
        if cache.is_file() and cache.stat().st_mtime >= reference.stat().st_mtime:
            return np.load(cache)
        import torch

        from .third_party.meanvc2.speaker import extract_embedding, init_speaker_model

        ckpt = model_dir(self.models) / "wavlm_large_finetune.pth"
        if not ckpt.is_file():
            raise RuntimeError("modello di riconoscimento voce non scaricato")
        if say:
            say("Analisi della voce di destinazione (una volta sola)…")
        model = init_speaker_model(str(ckpt), "cpu", wavlm_config=str(THIRD / "wavlm" / "wavlm_large_cfg.json"))
        try:
            emb = extract_embedding(model, str(reference), device="cpu").detach().cpu().numpy().astype(np.float32)
        finally:
            del model
            gc.collect()
        np.save(cache, emb)
        return emb

    def set_target(self, embedding: np.ndarray, target_id: str) -> None:
        import torch
        with torch.no_grad():
            self.spk = torch.from_numpy(np.asarray(embedding, dtype=np.float32).reshape(1, -1))
            self.gtm_kv = self.vc.gtm(self.spk)
        self.target_id = target_id
        self.reset()

    # ---- streaming ---------------------------------------------------------------------
    def reset(self) -> None:
        """Drop all streaming state (call after silence or a target change)."""
        import soxr
        import torch
        q = self.resample_quality
        if q == "FIR":
            self._rs_in, self._rs_out = FirResampler3("down"), FirResampler3("up")
        else:
            self._rs_in = soxr.ResampleStream(self.SAMPLE_RATE, 16000, 1, dtype="float32", quality=q)
            self._rs_out = soxr.ResampleStream(16000, self.SAMPLE_RATE, 1, dtype="float32", quality=q)
        self._samples = np.zeros(0, dtype=np.float32)
        self._frames = None
        self._enc_last = None
        self._asr_offset = self._p["offset_init"]
        self._att = torch.zeros(6, 4, self._p["cache"], 128)
        self._cnn = torch.zeros(6, 1, 256, 8)
        self._bn = None
        self._kv = None
        self._vc_offset = 0
        self._noise = None
        self._voc_mel = None
        self._last_wav = None

    def process(self, pcm: np.ndarray) -> np.ndarray:
        """48 kHz int16 mono in -> 48 kHz int16 mono out (whatever is ready; may be empty)."""
        if self.spk is None:
            raise RuntimeError("nessuna voce di destinazione")
        x = pcm.astype(np.float32) / 32768.0
        y16 = self._rs_in.resample_chunk(x)
        out16 = self._process16(y16) if y16.size else np.zeros(0, np.float32)
        y = self._rs_out.resample_chunk(out16) if out16.size else np.zeros(0, np.float32)
        return np.clip(y * 32768.0, -32768, 32767).astype(np.int16)

    def _process16(self, samples: np.ndarray) -> np.ndarray:
        import torch
        bn_new = self._encode(samples)
        if bn_new is None:
            return np.zeros(0, np.float32)
        self._bn = bn_new if self._bn is None else torch.cat([self._bn, bn_new], dim=1)
        need = self._p["chunk"] + BLOCK
        parts = []
        while self._bn is not None and self._bn.shape[1] >= need:
            cond = self._bn[:, :need, :]
            self._bn = self._bn[:, self._p["chunk"]:, :]
            parts.append(self._decode(self._vc_step(cond)))
        return np.concatenate(parts) if parts else np.zeros(0, np.float32)

    def _encode(self, samples: np.ndarray):
        import torch
        import torch.nn.functional as F
        import torchaudio.compliance.kaldi as kaldi
        p = self._p
        with torch.no_grad():
            padded = np.concatenate((self._samples, samples)) if self._samples.size else samples
            if len(padded) < 400:
                self._samples = padded
                return None
            fb = kaldi.fbank(torch.from_numpy(padded * 32768.0).unsqueeze(0), frame_length=25, frame_shift=10,
                             snip_edges=True, num_mel_bins=80, energy_floor=0.0, dither=0.0, sample_frequency=16000)
            self._samples = padded[160 * fb.shape[0]:]
            if self._frames is not None:
                fb = torch.cat([self._frames, fb], dim=0)
            if self._asr_offset >= 4000:
                self._asr_offset = max(self._att.size(2), self._asr_offset - 4000)
            bns, i = [], 0
            while i + p["bn_window"] <= fb.shape[0]:
                out, self._att, self._cnn = self.asr(
                    fb[i:i + p["bn_window"]].unsqueeze(0), torch.tensor(self._asr_offset, dtype=torch.int64),
                    torch.tensor(p["cache"], dtype=torch.int64), self._att, self._cnn)
                bns.append(out.squeeze(0))
                self._asr_offset += p["offset_step"]
                i += p["bn_stride"]
            self._frames = fb[i:]
            if not bns:
                return None
            bn = torch.cat(bns, dim=0).unsqueeze(0)
            if self._enc_last is not None:
                bn = torch.cat([self._enc_last, bn], dim=1)
            self._enc_last = bn[:, -1:, :]
            if bn.shape[1] < 2:
                return None
            n_out = 4 * (bn.shape[1] - 1)
            up = F.interpolate(bn.transpose(1, 2), size=n_out + 1, mode="linear", align_corners=True)
            return up.transpose(1, 2)[:, 1:, :]

    def _vc_step(self, cond):
        import torch
        n = self.steps
        dt = 1.0 / n
        chunk = self._p["chunk"]
        with torch.no_grad():
            x = torch.randn(1, cond.shape[1], 80, dtype=cond.dtype)
            if self._noise is not None:
                x[:, :BLOCK, :] = self._noise
            self._noise = x[:, -BLOCK:, :].clone()
            if self._vc_offset >= 4000:
                self._vc_offset = 0
                self._kv = None
            prev = self._kv
            for i in range(n):
                t_val = 1.0 - i * dt
                t = torch.full((1,), t_val)
                r = torch.full((1,), max(0.0, t_val - dt))
                u, kv = self.vc(x, t, r, cache=None, cond=cond, spks=self.spk, offset=self._vc_offset,
                                is_inference=True, kv_cache=prev, gtm_kv=self.gtm_kv)
                x = x - dt * u
                if i == n - 1:
                    self._kv = kv
            self._vc_offset += chunk
            return x[:, :chunk, :].transpose(1, 2)

    def _decode(self, mel) -> np.ndarray:
        import torch
        with torch.no_grad():
            if self._voc_mel is not None:
                mel = torch.cat([self._voc_mel, mel], dim=-1)
            self._voc_mel = mel[:, :, -2:]
            wav = self.vocoder.decode((mel + 1) / 2).squeeze().numpy()
        if self._last_wav is not None:
            wav = wav[0:]                            # vocoder_wav_pad = (2 - 1) * 160 - 160 = 0
            front = wav[:160]
            out = np.concatenate([self._last_wav * self._fade_down + front * self._fade_up, wav[160:-160]])
        else:
            out = wav[:-160]
        self._last_wav = wav[-160:]
        return out.astype(np.float32)
