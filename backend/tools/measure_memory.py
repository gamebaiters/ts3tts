"""Resident memory (and VRAM) of ONE engine in a fresh process.

    python tools/measure_memory.py --home <backend home> --engine kokoro [--variant int8]
    python tools/measure_memory.py --home <backend home> --engine qwen --size 0.6B

Run once per engine: a process that imported PyTorch never gives that RAM back,
so measuring several engines in one process would blame the light ones.
Prints one JSON line.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))


def rss_mb() -> float:
    import psutil
    return psutil.Process().memory_info().rss / 2**20


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--home", required=True)
    ap.add_argument("--engine", required=True, choices=["qwen", "kokoro", "supertonic"])
    ap.add_argument("--variant", default="int8")
    ap.add_argument("--size", default="1.7B")
    args = ap.parse_args()

    home = Path(args.home).resolve()
    os.environ["HF_HOME"] = str(home / "models" / "hf")
    os.environ["SUPERTONIC_CACHE_DIR"] = str(home / "models" / "supertonic3")

    # Count outgoing HTTP requests (httpx logs one INFO record per request): a
    # model that is already on disk must load with zero.
    import logging

    class Counter(logging.Handler):
        n = 0

        def emit(self, record):
            if "HTTP Request" in record.getMessage():
                Counter.n += 1

    httpx_log = logging.getLogger("httpx")
    httpx_log.setLevel(logging.INFO)
    httpx_log.addHandler(Counter())
    base = rss_mb()

    from gbtts.engines.base import SynthRequest
    status = lambda *_a: None  # noqa: E731
    text = "Ciao ragazzi, arrivo tra due minuti, aspettatemi in lobby che poi partiamo tutti insieme."
    if args.engine == "kokoro":
        from gbtts.engines.kokoro_engine import KokoroEngine
        eng, voice = KokoroEngine(home / "models", variant=args.variant), "if_sara"
    elif args.engine == "supertonic":
        from gbtts.engines.supertonic_engine import SupertonicEngine
        eng, voice = SupertonicEngine(home / "models"), "F1"
    else:
        from gbtts.engines.qwen_engine import QwenEngine
        from gbtts.voices import VoiceStore
        store = VoiceStore(home / "voices")
        eng = QwenEngine(store, size=args.size)
        voice = "clone:" + store.list()[0]["id"]

    t0 = time.perf_counter()
    eng.load(status)
    load_s = time.perf_counter() - t0
    loaded = rss_mb()
    times = []
    for _ in range(3):
        t0 = time.perf_counter()
        n = sum(chunk.size for chunk, _sr in eng.stream(SynthRequest(text=text, lang="it", voice=voice)))
        times.append(time.perf_counter() - t0)
    peak = rss_mb()
    out = {"engine": args.engine, "variant": args.variant if args.engine == "kokoro" else args.size,
           "python_base_mb": round(base), "rss_loaded_mb": round(loaded), "rss_after_synth_mb": round(peak),
           "load_s": round(load_s, 2), "synth_s": [round(t, 3) for t in times],
           "torch_imported": "torch" in sys.modules, "http_requests": Counter.n}
    if "torch" in sys.modules:
        import torch
        if torch.cuda.is_available():
            out["vram_peak_mb"] = round(torch.cuda.max_memory_allocated() / 2**20)
    print(json.dumps(out))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
