"""Engine benchmark + intelligibility check, no plugin needed.

    python tools/bench.py --home <backend home> [--size 1.7B] [--chunks 2 4 8]
                          [--out <dir>] [--asr]

Measures, per configuration and sentence: time to first audio (TTFA), real-time
factor (RTF = audio seconds / wall seconds) and peak VRAM. With --asr every
rendered sentence is transcribed by Whisper and the character error rate vs the
normalised input is reported - an objective proxy for "reads Italian right".
WAVs are written to --out so they can be listened to.
"""

from __future__ import annotations

import argparse
import json
import os
import sys
import time
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))

SENTENCES_IT = [
    "Ciao ragazzi, arrivo tra due minuti, aspettatemi in lobby.",
    "Qualcuno ha visto dove sono finite le munizioni? Io sono rimasto a secco.",
    "Perfetto, allora ci vediamo stasera alle nove e mezza, non fate tardi!",
    "Scusate, stavo mangiando. Adesso sono pronto, possiamo ricominciare la partita.",
    "Attenzione, c'è un cecchino sul tetto dell'edificio a nord, restate coperti.",
]


def cer(ref: str, hyp: str) -> float:
    import re
    norm = lambda s: re.sub(r"[^a-zàèéìòù0-9 ]", "", s.lower()).split()  # noqa: E731
    r, h = " ".join(norm(ref)), " ".join(norm(hyp))
    if not r:
        return 0.0
    d = list(range(len(h) + 1))
    for i in range(1, len(r) + 1):
        prev, d[0] = d[0], i
        for j in range(1, len(h) + 1):
            cur = min(d[j] + 1, d[j - 1] + 1, prev + (r[i - 1] != h[j - 1]))
            prev, d[j] = d[j], cur
    return d[len(h)] / len(r)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--home", required=True)
    ap.add_argument("--size", default="1.7B")
    ap.add_argument("--chunks", nargs="*", type=int, default=[4])
    ap.add_argument("--prefill", nargs="*", type=int, default=[1])
    ap.add_argument("--voice", default="")
    ap.add_argument("--design", default="Voce maschile italiana, sui trent'anni, calda e naturale, tono amichevole, "
                                         "dizione chiara, accento italiano neutro.")
    ap.add_argument("--engines", nargs="*", default=["qwen", "supertonic"])
    ap.add_argument("--kokoro-variants", nargs="*", default=["int8", "fp32"])
    ap.add_argument("--out", default=str(ROOT / "bench_out"))
    ap.add_argument("--asr-only", action="store_true",
                    help="transcribe the WAVs listed in <out>/results.json (separate process: HF_HOME is read at import)")
    ap.add_argument("--asr-cache", default="")
    args = ap.parse_args()

    home = Path(args.home).resolve()
    if args.asr_only:
        return asr_only(Path(args.out), args.asr_cache)
    os.environ["HF_HOME"] = str(home / "models" / "hf")
    os.environ["SUPERTONIC_CACHE_DIR"] = str(home / "models" / "supertonic3")
    from gbtts.netfix import use_system_certificates
    use_system_certificates()

    import soundfile as sf
    import torch
    from gbtts import textproc
    from gbtts.audio import Leveler, StreamResampler
    from gbtts.engines.base import SynthRequest
    from gbtts.voices import VoiceStore

    out = Path(args.out)
    out.mkdir(parents=True, exist_ok=True)
    store = VoiceStore(home / "voices")
    status = lambda s, m: print(f"  [{s}] {m}", flush=True)  # noqa: E731
    results = []

    if "qwen" in args.engines:
        from gbtts.engines.qwen_engine import QwenEngine
        eng = QwenEngine(store, size=args.size)
        voice = args.voice
        if not voice:
            existing = [v for v in store.list() if v.get("source") == "design"]
            if existing:
                voice = existing[0]["id"]
            else:
                print("designing a reference Italian voice…", flush=True)
                ref_text = ("Ciao a tutti, questa è la mia voce. Parlo in modo naturale e tranquillo, "
                            "come se fossi in chiamata con gli amici.")
                audio, sr = eng.design(ref_text, args.design, "it", status)
                entry = store.add(name="Bench IT", audio=audio, sr=sr, ref_text=ref_text, lang="it",
                                  source="design", instruct=args.design)
                voice = entry["id"]
                sf.write(str(out / "design_reference.wav"), audio, sr)
        eng.ensure("base", status)
        eng.create_voice_prompt(voice, status)
        for prefill in args.prefill:
            for cs in args.chunks:
                eng.chunk_size = cs
                eng.full_text_prefill = bool(prefill)
                for i, s in enumerate(SENTENCES_IT):
                    text = textproc.normalize(s, "it")
                    torch.cuda.reset_peak_memory_stats()
                    req = SynthRequest(text=text, lang="it", voice=f"clone:{voice}")
                    t0 = time.perf_counter()
                    ttfa = None
                    parts = []
                    sr = 24000
                    for chunk, sr in eng.stream(req):
                        if ttfa is None:
                            ttfa = time.perf_counter() - t0
                        parts.append(chunk)
                    wall = time.perf_counter() - t0
                    audio = np.concatenate(parts) if parts else np.zeros(1, np.float32)
                    dur = audio.size / sr
                    name = f"qwen_{args.size}_cs{cs}_pf{prefill}_{i}.wav"
                    sf.write(str(out / name), audio, sr)
                    r = {"engine": "qwen", "size": args.size, "chunk": cs, "prefill": prefill, "i": i,
                         "ttfa_ms": round((ttfa or wall) * 1000), "rtf": round(dur / wall, 2), "audio_s": round(dur, 2),
                         "vram_peak_mb": int(torch.cuda.max_memory_allocated() / 2**20), "file": name, "text": text}
                    results.append(r)
                    print(json.dumps(r, ensure_ascii=False), flush=True)
        eng.unload()

    if "kokoro" in args.engines:
        from gbtts.engines.kokoro_engine import KokoroEngine
        for variant in args.kokoro_variants:
            ko = KokoroEngine(home / "models", variant=variant)
            ko.load(status)
            for voice in ("if_sara", "im_nicola"):
                for i, s in enumerate(SENTENCES_IT):
                    text = textproc.normalize(s, "it")
                    t0 = time.perf_counter()
                    (audio, sr), = list(ko.stream(SynthRequest(text=text, lang="it", voice=voice)))
                    wall = time.perf_counter() - t0
                    name = f"kokoro_{variant}_{voice}_{i}.wav"
                    sf.write(str(out / name), audio, sr)
                    r = {"engine": "kokoro", "size": variant, "voice": voice, "i": i, "ttfa_ms": round(wall * 1000),
                         "rtf": round(audio.size / sr / wall, 2), "audio_s": round(audio.size / sr, 2), "file": name,
                         "text": text}
                    results.append(r)
                    print(json.dumps(r, ensure_ascii=False), flush=True)
            ko.unload()

    if "supertonic" in args.engines:
        from gbtts.engines.supertonic_engine import SupertonicEngine
        st = SupertonicEngine(home / "models")
        st.load(status)
        for voice in ("M1", "F1"):
            for i, s in enumerate(SENTENCES_IT):
                text = textproc.normalize(s, "it")
                t0 = time.perf_counter()
                (audio, sr), = list(st.stream(SynthRequest(text=text, lang="it", voice=voice)))
                wall = time.perf_counter() - t0
                name = f"supertonic_{voice}_{i}.wav"
                sf.write(str(out / name), audio, sr)
                r = {"engine": "supertonic", "voice": voice, "i": i, "ttfa_ms": round(wall * 1000),
                     "rtf": round(audio.size / sr / wall, 2), "audio_s": round(audio.size / sr, 2), "file": name,
                     "text": text}
                results.append(r)
                print(json.dumps(r, ensure_ascii=False), flush=True)

    prev = out / "results.json"
    if prev.exists() and args.engines:
        try:
            old = json.loads(prev.read_text(encoding="utf-8"))
            keep = [r for r in old if r["file"] not in {x["file"] for x in results}]
            results = keep + results
        except Exception:
            pass
    prev.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    summarize(results)
    return 0


def asr_only(out: Path, cache: str) -> int:
    if cache:
        os.environ["HF_HOME"] = cache
    from gbtts.netfix import use_system_certificates
    use_system_certificates()
    import soundfile as sf
    import torch
    from transformers import pipeline
    from gbtts.audio import StreamResampler

    results = json.loads((out / "results.json").read_text(encoding="utf-8"))
    asr = pipeline("automatic-speech-recognition", model="openai/whisper-large-v3-turbo",
                   dtype=torch.float16, device="cuda:0")
    for r in results:
        wav, sr = sf.read(str(out / r["file"]), dtype="float32")
        rs = StreamResampler(sr, 16000)
        x = np.concatenate([rs.process(wav), rs.flush()])
        hyp = asr({"raw": x, "sampling_rate": 16000},
                  generate_kwargs={"language": "italian", "task": "transcribe"})["text"]
        r["asr"] = hyp.strip()
        r["cer"] = round(cer(r["text"], hyp), 3)
        print(f"CER {r['cer']:.3f} [{r['file']}] {hyp.strip()}", flush=True)
    (out / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    summarize(results)
    return 0


def summarize(results: list[dict]) -> None:
    groups: dict[str, list] = {}
    for r in results:
        key = f"{r['engine']} size={r.get('size', '-')} chunk={r.get('chunk', '-')} prefill={r.get('prefill', '-')} voice={r.get('voice', '-')}"
        groups.setdefault(key, []).append(r)
    print("\n==== SUMMARY (median over sentences, first sentence of each run excluded from TTFA) ====")
    for key, rs in groups.items():
        tt = [x["ttfa_ms"] for x in rs[1:]] or [rs[0]["ttfa_ms"]]
        line = f"{key}: TTFA {int(np.median(tt))} ms, RTF {np.median([x['rtf'] for x in rs]):.2f}"
        if "vram_peak_mb" in rs[0]:
            line += f", VRAM peak {max(x['vram_peak_mb'] for x in rs)} MB"
        if "cer" in rs[0]:
            line += f", CER {np.mean([x['cer'] for x in rs]):.3f}"
        print(line)


if __name__ == "__main__":
    raise SystemExit(main())
