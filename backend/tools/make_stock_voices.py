"""Build the Italian stock voices shipped with the plugin (backend/gbtts/stock_voices).

Every voice is ORIGINAL and synthetic: designed by Qwen3-TTS VoiceDesign from an Italian
description. No real person's recording is used (cloning people found online is
impersonation - the plugin forbids it for users too).

    python tools/make_stock_voices.py design --home <engine home> --out <dir> [--seeds 3] [--only sofia luca]
    python tools/make_stock_voices.py asr    --out <dir> --asr-cache <scratch HF dir>
    python tools/make_stock_voices.py pick   --out <dir> [--write]

design  for each voice x seed: VoiceDesign renders the voice's script (the future reference
        recording), then the Base model CLONES it and reads three test sentences - exactly
        what happens when a user picks the voice.
asr     (separate process: HF_HOME is read at import) Whisper large-v3-turbo CER of every
        clone render and of the reference; median F0 of each reference.
pick    per voice the seed with the lowest mean clone CER whose F0 fits the gender; --write
        copies it to backend/gbtts/stock_voices/<slug>/ref.wav and updates stock_voices.json.
"""

from __future__ import annotations

import argparse
import json
import os
import shutil
import sys
from pathlib import Path

import numpy as np

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT))
STOCK_DIR = ROOT / "gbtts" / "stock_voices"

ACCENT = "accento italiano standard senza inflessioni dialettali"

# slug, name, gender, short Italian description shown in the UI, VoiceDesign instruction, script
VOICES = [
    # Designed on users' PCs at the first start by v1.0-v1.4 (seeds 20260914/15): imported
    # from such an install with `design --import giulia=<ref.wav> marco=<ref.wav>`.
    ("giulia", "Giulia", "female", "Giovane donna sui venticinque anni, calda e cordiale",
     f"Voce femminile italiana, giovane adulta sui venticinque anni, timbro caldo e luminoso, tono cordiale e "
     f"rilassato, dizione chiara, {ACCENT}, ritmo naturale da conversazione.",
     "Ciao a tutti, sono Giulia. Parlo in modo tranquillo e naturale, come se fossimo in chiamata insieme a "
     "giocare. Fatemi sapere se mi sentite bene."),
    ("marco", "Marco", "male", "Uomo sui trent'anni, amichevole e sicuro",
     f"Voce maschile italiana, uomo sui trent'anni, timbro medio-grave e pieno, tono amichevole e sicuro, dizione "
     f"chiara, {ACCENT}, ritmo naturale da conversazione.",
     "Ciao ragazzi, sono Marco. Parlo con calma e in modo naturale, come in una chiamata tra amici. Ditemi pure "
     "se l'audio arriva bene."),
    ("sofia", "Sofia", "female", "Ragazza sui vent'anni, vivace ed entusiasta",
     f"Voce femminile italiana di una ragazza di vent'anni, timbro chiaro e squillante, tono vivace ed entusiasta, "
     f"parla con energia e col sorriso, dizione chiara, {ACCENT}.",
     "Ehi, ciao a tutti! Sono Sofia. Stasera si gioca fino a tardi, quindi preparatevi, perché questa volta "
     "vinciamo noi, ve lo prometto."),
    ("elena", "Elena", "female", "Donna sui quarant'anni, calda e calma, da narratrice",
     f"Voce femminile italiana di una donna sui quarant'anni, timbro caldo, morbido e profondo, tono calmo e "
     f"rassicurante, ritmo lento e posato come una narratrice di audiolibri, dizione impeccabile, {ACCENT}.",
     "Buonasera, sono Elena. Mi piace raccontare le cose con calma, una parola alla volta, lasciando il tempo "
     "di ascoltare ogni dettaglio."),
    ("chiara", "Chiara", "female", "Giornalista sui trent'anni, limpida e professionale",
     f"Voce femminile italiana di una giornalista sui trent'anni, timbro limpido e professionale, tono sicuro e "
     f"neutro da conduttrice del telegiornale, articolazione precisa, ritmo sostenuto, {ACCENT}.",
     "Buongiorno, sono Chiara. Ecco le ultime notizie: la partita di stasera inizierà alle nove, con tutte le "
     "squadre già pronte a scendere in campo."),
    ("rosa", "Rosa", "female", "Signora anziana, dolce e affettuosa",
     f"Voce femminile italiana di una signora anziana sui settant'anni, timbro dolce e leggermente velato, tono "
     f"affettuoso e gentile come una nonna, ritmo tranquillo, {ACCENT}.",
     "Ciao tesoro, sono Rosa. Siediti un momento, che ti preparo qualcosa di caldo, e intanto mi racconti com'è "
     "andata la giornata."),
    # 2026-09-16: "ragazzo di vent'anni, timbro giovane e brillante, entusiasmo" gave 262-362 Hz on
    # 3/3 seeds and "voce da uomo ... per niente acuto" still 4/5 above 245 Hz: VoiceDesign reads
    # "ragazzo" as a boy. "giovane uomo" + "baritonale" is what the model follows.
    ("luca", "Luca", "male", "Giovane uomo sui venticinque anni, allegro e spigliato",
     f"Voce maschile italiana di un giovane uomo sui venticinque anni, timbro baritonale caldo e maschile, voce "
     f"grave, tono allegro, disinvolto e spigliato, ritmo naturale, {ACCENT}.",
     "Raga, sono Luca! Avete visto quella giocata? Pazzesca! Dai, facciamone subito un'altra, che sono "
     "carichissimo."),
    ("alessandro", "Alessandro", "male", "Uomo sui quarantacinque anni, grave e autorevole, da speaker radio",
     f"Voce maschile italiana di un uomo sui quarantacinque anni, timbro molto grave, profondo e vellutato, tono "
     f"autorevole e sicuro da speaker radiofonico, dizione perfetta, ritmo misurato, {ACCENT}.",
     "Buonasera, sono Alessandro. Benvenuti a questa serata speciale: mettetevi comodi, perché il meglio deve "
     "ancora arrivare."),
    ("giorgio", "Giorgio", "male", "Signore anziano, bonario e paziente",
     f"Voce maschile italiana di un signore anziano sui settant'anni, timbro caldo e un po' roco, tono bonario, "
     f"saggio e paziente come un nonno, ritmo lento, {ACCENT}.",
     "Ciao ragazzo, sono Giorgio. Ai miei tempi si giocava all'aperto, ma devo ammettere che anche questi giochi "
     "hanno il loro fascino."),
    ("davide", "Davide", "male", "Uomo sui trentacinque anni, cordiale e rilassato",
     f"Voce maschile italiana di un uomo sui trentacinque anni, timbro tenorile chiaro e luminoso, tono cordiale, "
     f"positivo e rilassato, ritmo naturale da conversazione, {ACCENT}.",
     "Ciao a tutti, sono Davide. Tranquilli, nessuna fretta: facciamo le cose per bene e ci divertiamo, come "
     "sempre."),
]

TEST_SENTENCES = [
    "Ciao ragazzi, arrivo tra due minuti, aspettatemi in lobby.",
    "Attenzione, c'è un cecchino sul tetto dell'edificio a nord, restate coperti.",
    "Perfetto, allora ci vediamo stasera alle nove e mezza, non fate tardi!",
]

SEED_BASE = 20260916

# Pitch each character should have (Hz): young women ~250-300, adult women ~200, men ~110-140.
TARGET_F0 = {"giulia": 250, "marco": 150, "sofia": 290, "elena": 205, "chiara": 215, "rosa": 200,
             "luca": 140, "alessandro": 115, "giorgio": 130, "davide": 135}


def design(args) -> int:
    home = Path(args.home).resolve()
    os.environ["HF_HOME"] = str(home / "models" / "hf")
    os.environ.setdefault("HF_HUB_OFFLINE", "1")
    from gbtts.netfix import use_system_certificates
    use_system_certificates()
    import soundfile as sf
    import torch
    from gbtts import textproc
    from gbtts.audio import trim_silence
    from gbtts.engines.base import SynthRequest
    from gbtts.engines.qwen_engine import QwenEngine
    from gbtts.voices import VoiceStore

    out = Path(args.out).resolve()
    (out / "work_voices").mkdir(parents=True, exist_ok=True)
    store = VoiceStore(out / "work_voices")
    eng = QwenEngine(store, size="1.7B")
    status = lambda s, m: print(f"  [{s}] {m}", flush=True)  # noqa: E731
    results_path = out / "results.json"
    results = json.loads(results_path.read_text(encoding="utf-8")) if results_path.exists() else []
    done = {(r["slug"], r["seed"]) for r in results}

    imports = dict(item.split("=", 1) for item in args.imports)
    todo = [v for v in VOICES if (not args.only or v[0] in args.only) and (v[0] in imports or v[0] not in ("giulia", "marco"))]
    candidates = []
    for vi, (slug, name, gender, _label, instruct, script) in enumerate(todo):
        if slug in imports:
            seed = 0   # not designed here
            ref_file = out / f"{slug}_s0_ref.wav"
            wav, sr = sf.read(imports[slug], dtype="float32")
            sf.write(str(ref_file), wav, sr)
            print(f"imported {slug}: {wav.size / sr:.1f} s", flush=True)
            candidates.append((slug, name, gender, seed, ref_file, script, instruct))
            continue
        for k in range(args.seeds):
            designed = [x[0] for x in VOICES if x[0] not in ("giulia", "marco")]
            seed = SEED_BASE + designed.index(slug) * 100 + k   # stable: a re-run finds the same candidates
            ref_file = out / f"{slug}_s{seed}_ref.wav"
            if (slug, seed) in done and ref_file.exists():
                continue
            torch.manual_seed(seed)
            audio, sr = eng.design(script, instruct, "it", status)
            audio = trim_silence(audio, sr, pad_ms=80.0)
            sf.write(str(ref_file), audio, sr)
            print(f"designed {slug} seed {seed}: {audio.size / sr:.1f} s", flush=True)
            candidates.append((slug, name, gender, seed, ref_file, script, instruct))

    eng.ensure("base", status)
    for slug, name, gender, seed, ref_file, script, instruct in candidates:
        wav, sr = sf.read(str(ref_file), dtype="float32")
        entry = store.add(name=f"{name} {seed}", audio=wav, sr=sr, ref_text=script, lang="it", source="design",
                          instruct=instruct)
        renders = []
        for i, sentence in enumerate(TEST_SENTENCES):
            text = textproc.normalize(sentence, "it")
            req = SynthRequest(text=text, lang="it", voice=f"clone:{entry['id']}")
            parts = [c for c, _ in eng.stream(req)]
            audio = np.concatenate(parts) if parts else np.zeros(1, np.float32)
            f = out / f"{slug}_s{seed}_clone{i}.wav"
            sf.write(str(f), audio, 24000)
            renders.append({"file": f.name, "text": text, "seconds": round(audio.size / 24000, 2)})
        results = [r for r in results if not (r["slug"] == slug and r["seed"] == seed)]
        results.append({"slug": slug, "name": name, "gender": gender, "seed": seed, "ref": ref_file.name,
                        "ref_text": script, "instruct": instruct,
                        "ref_seconds": round(sf.info(str(ref_file)).duration, 2), "renders": renders})
        results_path.write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
        print(f"cloned {slug} seed {seed}: " + ", ".join(f"{r['seconds']} s" for r in renders), flush=True)
    eng.unload()
    return 0


def cer(ref: str, hyp: str) -> float:
    from bench import cer as bench_cer
    return bench_cer(ref, hyp)


def median_f0(x: np.ndarray, sr: int) -> float:
    """Median F0 of voiced frames: librosa pYIN (60-500 Hz) when available."""
    try:
        import librosa
        y = librosa.resample(x, orig_sr=sr, target_sr=16000)
        f0, voiced, _prob = librosa.pyin(y, fmin=60, fmax=500, sr=16000, frame_length=1024)
        v = f0[voiced & ~np.isnan(f0)]
        return float(np.median(v)) if v.size else 0.0
    except ImportError:
        return _autocorr_f0(x, sr)


def _autocorr_f0(x: np.ndarray, sr: int) -> float:
    """Fallback: autocorrelation, 60-450 Hz (agreed with pYIN within ~10% on these voices)."""
    frame, hop = int(0.04 * sr), int(0.01 * sr)
    lo, hi = int(sr / 450), int(sr / 60)
    energy_gate = 0.1 * np.sqrt(np.mean(x ** 2) + 1e-12)
    f0s = []
    for s in range(0, x.size - frame, hop):
        w = x[s:s + frame] * np.hanning(frame)
        if np.sqrt(np.mean(w ** 2)) < energy_gate:
            continue
        ac = np.correlate(w, w, mode="full")[frame - 1:]
        if ac[0] <= 0:
            continue
        lag = lo + int(np.argmax(ac[lo:hi]))
        if ac[lag] / ac[0] > 0.45:
            f0s.append(sr / lag)
    return float(np.median(f0s)) if f0s else 0.0


def asr(args) -> int:
    if args.asr_cache:
        os.environ["HF_HOME"] = args.asr_cache
    sys.path.insert(0, str(ROOT / "tools"))
    from gbtts.netfix import use_system_certificates
    use_system_certificates()
    import soundfile as sf
    import torch
    from transformers import pipeline
    from gbtts.audio import StreamResampler

    out = Path(args.out).resolve()
    results = json.loads((out / "results.json").read_text(encoding="utf-8"))
    pipe = pipeline("automatic-speech-recognition", model="openai/whisper-large-v3-turbo", dtype=torch.float16,
                    device="cuda:0")

    def transcribe(path: Path) -> str:
        wav, sr = sf.read(str(path), dtype="float32")
        rs = StreamResampler(sr, 16000)
        x = np.concatenate([rs.process(wav), rs.flush()])
        return pipe({"raw": x, "sampling_rate": 16000},
                    generate_kwargs={"language": "italian", "task": "transcribe"})["text"].strip()

    for r in results:
        if args.only and r["slug"] not in args.only:
            continue
        wav, sr = sf.read(str(out / r["ref"]), dtype="float32")
        r["ref_f0"] = round(median_f0(wav, sr), 1)
        hyp = transcribe(out / r["ref"])
        r["ref_asr"], r["ref_cer"] = hyp, round(cer(r["ref_text"], hyp), 3)
        for x in r["renders"]:
            hyp = transcribe(out / x["file"])
            x["asr"], x["cer"] = hyp, round(cer(x["text"], hyp), 3)
            cw, csr = sf.read(str(out / x["file"]), dtype="float32")
            x["f0"] = round(median_f0(cw, csr), 1)
        r["clone_cer"] = round(float(np.mean([x["cer"] for x in r["renders"]])), 3)
        r["clone_f0"] = round(float(np.median([x["f0"] for x in r["renders"]])), 1)
        print(f"{r['slug']:>10} seed {r['seed']}: ref CER {r['ref_cer']:.3f} F0 {r['ref_f0']:.0f} Hz | "
              f"clone CER {r['clone_cer']:.3f} F0 {r['clone_f0']:.0f} Hz", flush=True)
    (out / "results.json").write_text(json.dumps(results, ensure_ascii=False, indent=2), encoding="utf-8")
    return 0


def pick(args) -> int:
    out = Path(args.out).resolve()
    results = json.loads((out / "results.json").read_text(encoding="utf-8"))
    forced = {k: int(v) for k, v in (item.split("=", 1) for item in args.choose)}
    chosen = []
    for slug, name, gender, label, instruct, script in VOICES:
        rows = [r for r in results if r["slug"] == slug and "clone_cer" in r]
        if not rows:
            print(f"{slug}: no measured candidate")
            continue
        # F0 must fit the gender in the reference AND in the clone (the clone is what users hear);
        # among those: clearest speech first, then the pitch closest to the voice's character.
        def plausible(r):
            f_ok = lambda f: f >= 150 if gender == "female" else 0 < f <= 165  # noqa: E731
            return f_ok(r["ref_f0"]) and f_ok(r["clone_f0"])
        # Imported voices (seed 0: Giulia, Marco) are what users already know: kept as they are.
        pool = [r for r in rows if plausible(r) or r["seed"] == 0]
        if not pool:
            print(f"{slug}: NO candidate with a plausible pitch - redesign it")
            continue
        target = TARGET_F0[slug]
        best = min(pool, key=lambda r: (round(r["clone_cer"], 2), round(r["ref_cer"], 2),
                                        abs(r["clone_f0"] - target) + abs(r["ref_f0"] - target)))
        if slug in forced:   # e.g. chosen for distinctness (ECAPA), see vault engines-and-models
            best = next(r for r in rows if r["seed"] == forced[slug])
        chosen.append((best, label))
        flag = "" if plausible(best) else "  (!) F0 outside the expected range"
        print(f"{slug:>10}: seed {best['seed']}  clone CER {best['clone_cer']:.3f}  ref CER {best['ref_cer']:.3f}  "
              f"F0 ref {best['ref_f0']:.0f} / clone {best['clone_f0']:.0f} Hz  {best['ref_seconds']} s{flag}")
    if not args.write:
        return 0
    index_path = STOCK_DIR / "stock_voices.json"
    index = json.loads(index_path.read_text(encoding="utf-8")) if index_path.exists() else {"version": 1, "voices": []}
    by_slug = {v["slug"]: v for v in index["voices"]}
    for best, label in chosen:
        d = STOCK_DIR / best["slug"]
        d.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(out / best["ref"], d / "ref.wav")
        by_slug[best["slug"]] = {
            "slug": best["slug"], "name": best["name"], "gender": best["gender"], "lang": "it",
            "description": label, "ref_text": best["ref_text"], "instruct": best["instruct"],
            "design_seed": best["seed"], "measured": {"clone_cer": best["clone_cer"], "ref_cer": best["ref_cer"],
                                                      "f0_hz": best["clone_f0"], "seconds": best["ref_seconds"]},
        }
    order = [v[0] for v in VOICES]
    index["voices"] = sorted(by_slug.values(), key=lambda v: order.index(v["slug"]) if v["slug"] in order else 99)
    index_path.write_text(json.dumps(index, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    print(f"written {len(chosen)} voices to {STOCK_DIR}")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser()
    sub = ap.add_subparsers(dest="cmd", required=True)
    d = sub.add_parser("design")
    d.add_argument("--home", required=True)
    d.add_argument("--out", required=True)
    d.add_argument("--seeds", type=int, default=3)
    d.add_argument("--only", nargs="*", default=[])
    d.add_argument("--import", dest="imports", nargs="*", default=[], metavar="SLUG=REF.WAV")
    a = sub.add_parser("asr")
    a.add_argument("--out", required=True)
    a.add_argument("--asr-cache", default="")
    a.add_argument("--only", nargs="*", default=[])
    p = sub.add_parser("pick")
    p.add_argument("--out", required=True)
    p.add_argument("--write", action="store_true")
    p.add_argument("--choose", nargs="*", default=[], metavar="SLUG=SEED")
    args = ap.parse_args()
    return {"design": design, "asr": asr, "pick": pick}[args.cmd](args)


if __name__ == "__main__":
    raise SystemExit(main())
