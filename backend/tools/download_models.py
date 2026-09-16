"""Download the model weights into <home>/models (no global HF cache pollution).

    python tools/download_models.py --home <backend home> [--models base-1.7b design-1.7b ...] [--progress]

--progress prints machine-readable lines for the installer while downloading:
    @@progress {"done": <bytes on disk>, "total": <bytes expected>, "item": "<model key>"}
(bytes already downloaded by an earlier, interrupted run count as done).

Keys:
    base-1.7b     Qwen/Qwen3-TTS-12Hz-1.7B-Base         voice cloning, best quality
    base-0.6b     Qwen/Qwen3-TTS-12Hz-0.6B-Base         voice cloning, fastest
    design-1.7b   Qwen/Qwen3-TTS-12Hz-1.7B-VoiceDesign  create voices from a description
    custom-1.7b   Qwen/Qwen3-TTS-12Hz-1.7B-CustomVoice  9 stock speakers + style instructions
    custom-0.6b   Qwen/Qwen3-TTS-12Hz-0.6B-CustomVoice
    supertonic    Supertone/supertonic-3                CPU engine, 31 languages
    kokoro        Kokoro-82M fp32 ONNX + voices        light CPU engine (~350 MB), recommended
    kokoro-int8   Kokoro-82M int8 ONNX + voices        smallest download (~140 MB) but ~7x slower on CPU
    vc            MeanVC2 real-time voice changer       both latency presets + speaker model (~1.9 GB)
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import threading
import time
from pathlib import Path

REPOS = {
    "base-1.7b": "Qwen/Qwen3-TTS-12Hz-1.7B-Base",
    "base-0.6b": "Qwen/Qwen3-TTS-12Hz-0.6B-Base",
    "design-1.7b": "Qwen/Qwen3-TTS-12Hz-1.7B-VoiceDesign",
    "custom-1.7b": "Qwen/Qwen3-TTS-12Hz-1.7B-CustomVoice",
    "custom-0.6b": "Qwen/Qwen3-TTS-12Hz-0.6B-CustomVoice",
}
DEFAULT = ["base-1.7b", "design-1.7b", "supertonic"]


def dir_bytes(path: Path) -> int:
    """Bytes on disk under `path`, symlinks skipped (the HF cache links snapshots to blobs)."""
    total = 0
    for root, _dirs, files in os.walk(path):
        for name in files:
            p = os.path.join(root, name)
            try:
                if not os.path.islink(p):
                    total += os.stat(p).st_size
            except OSError:
                pass
    return total


def expected(key: str, models: Path) -> tuple[Path, int]:
    """(directory the key downloads into, expected bytes or 0 if unknown)."""
    try:
        if key in REPOS:
            from huggingface_hub import HfApi
            info = HfApi().model_info(REPOS[key], files_metadata=True)
            size = sum(s.size or 0 for s in info.siblings or [])
            return models / "hf" / "hub" / ("models--" + REPOS[key].replace("/", "--")), size
        if key in ("kokoro", "kokoro-int8"):
            from gbtts.engines.kokoro_engine import MODEL_FILES, VOICES_FILE, kokoro_dir
            variant = "int8" if key == "kokoro-int8" else "fp32"
            return kokoro_dir(models), MODEL_FILES[variant][1] + VOICES_FILE[1]
        if key == "vc":
            from huggingface_hub import HfApi
            from gbtts.vc.engine import HF_REPO, MODELS, SPEAKER_SIZE, VOCODER, model_dir
            names = {VOCODER} | {MODELS[p]["ckpt"] for p in MODELS} | {MODELS[p]["asr"] for p in MODELS}
            infos = HfApi().get_paths_info(HF_REPO, sorted(names))
            return model_dir(models), sum(getattr(i, "size", 0) or 0 for i in infos) + SPEAKER_SIZE
        if key == "supertonic":
            return models / "supertonic3", 0
    except Exception as exc:  # noqa: BLE001 - progress is best effort, never fatal
        print(f"[download] size of {key} unknown: {exc}", flush=True)
    return models, 0


class Progress(threading.Thread):
    def __init__(self, parts: list[tuple[str, Path, int]]):
        super().__init__(daemon=True)
        self.parts = parts
        self.item = ""
        self.stop = threading.Event()

    def emit(self) -> None:
        done = total = 0
        for _key, path, size in self.parts:
            if size > 0:
                done += min(dir_bytes(path), size)
                total += size
        print("@@progress " + json.dumps({"done": done, "total": total, "item": self.item}), flush=True)

    def run(self) -> None:
        while not self.stop.wait(1.0):
            self.emit()


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--home", required=True)
    ap.add_argument("--models", nargs="*", default=DEFAULT)
    ap.add_argument("--progress", action="store_true")
    args = ap.parse_args()

    home = Path(args.home).resolve()
    models = home / "models"
    models.mkdir(parents=True, exist_ok=True)
    os.environ["HF_HOME"] = str(models / "hf")
    os.environ.setdefault("HF_HUB_DISABLE_TELEMETRY", "1")
    os.environ["SUPERTONIC_CACHE_DIR"] = str(models / "supertonic3")

    sys.path.insert(0, str(Path(__file__).resolve().parent.parent))
    from gbtts.netfix import use_system_certificates
    use_system_certificates()

    from huggingface_hub import snapshot_download

    progress = None
    if args.progress:
        progress = Progress([(key, *expected(key, models)) for key in args.models])
        progress.emit()
        progress.start()

    for key in args.models:
        t0 = time.time()
        if progress:
            progress.item = key
        if key in ("kokoro", "kokoro-int8"):
            from gbtts.engines.kokoro_engine import download_kokoro
            variant = "int8" if key == "kokoro-int8" else "fp32"
            download_kokoro(models, variant, lambda msg: print(f"[download] {msg}", flush=True))
        elif key == "vc":
            from gbtts.vc.engine import download as download_vc
            download_vc(models, ["40ms", "120ms"], lambda msg: print(f"[download] {msg}", flush=True))
        elif key == "supertonic":
            from supertonic import TTS
            print(f"[download] supertonic-3 -> {os.environ['SUPERTONIC_CACHE_DIR']}", flush=True)
            TTS(model="supertonic-3", auto_download=True)
        elif key in REPOS:
            print(f"[download] {REPOS[key]}", flush=True)
            path = snapshot_download(REPOS[key])
            print(f"[download]   -> {path}", flush=True)
        else:
            print(f"[download] unknown model key {key!r}", file=sys.stderr)
            return 2
        print(f"[download] {key} done in {time.time() - t0:.0f}s", flush=True)
    if progress:
        progress.stop.set()
        progress.join(2)
        progress.emit()
    print("[download] ALL DONE", flush=True)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
