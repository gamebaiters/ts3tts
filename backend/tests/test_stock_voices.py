"""Italian stock voices: shipped files and their installation into a voice library.

    <engine venv>\\Scripts\\python.exe backend\\tests\\test_stock_voices.py

No model: checks the package contents (every voice has a clean 24 kHz reference with its
transcript) and VoiceStore.install_stock (fresh library, adoption of the Giulia/Marco that
v1.0-v1.4 designed on the user's PC, a deleted stock voice never comes back).
"""

from __future__ import annotations

import json
import sys
import tempfile
from pathlib import Path

import numpy as np
import soundfile as sf

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from gbtts.voices import REF_SR, VoiceStore  # noqa: E402

STOCK = Path(__file__).resolve().parents[1] / "gbtts" / "stock_voices"
passed = failed = 0


def check(name: str, ok: bool, detail: str = "") -> None:
    global passed, failed
    print(f"  {'PASS' if ok else 'FAIL'} {name:<62} {detail}")
    if ok:
        passed += 1
    else:
        failed += 1


def main() -> int:
    print("GameBaiters TTS - stock voices test")
    index = json.loads((STOCK / "stock_voices.json").read_text(encoding="utf-8"))
    voices = index["voices"]
    slugs = [v["slug"] for v in voices]
    check("ten Italian voices shipped, Giulia first", len(voices) == 10 and slugs[0] == "giulia", ", ".join(slugs))
    check("every voice is Italian with a gender, description and transcript",
          all(v["lang"] == "it" and v["gender"] in ("female", "male") and v["description"] and len(v["ref_text"]) > 40
              for v in voices))
    check("five women and five men",
          sum(v["gender"] == "female" for v in voices) == 5 and sum(v["gender"] == "male" for v in voices) == 5)
    bad = []
    for v in voices:
        info = sf.info(str(STOCK / v["slug"] / "ref.wav"))
        wav, _ = sf.read(str(STOCK / v["slug"] / "ref.wav"), dtype="float32")
        if info.samplerate != REF_SR or info.channels != 1 or not 6.0 <= info.duration <= 15.0 or np.max(np.abs(wav)) < 0.3:
            bad.append(f"{v['slug']} {info.samplerate} Hz {info.channels} ch {info.duration:.1f} s")
    check("references: 24 kHz mono, 6-15 s, not silent", not bad, "; ".join(bad))
    check("measured intelligibility recorded (clone CER <= 0.02)",
          all(v["measured"]["clone_cer"] <= 0.02 for v in voices))
    names = [v["name"] for v in voices]
    check("names unique", len(set(names)) == len(names))

    with tempfile.TemporaryDirectory() as tmp:
        store = VoiceStore(Path(tmp) / "fresh")
        added = store.install_stock(STOCK)
        lib = store.list()
        check("fresh library: all ten added", added == 10 and len(lib) == 10, f"{added} added")
        check("library order follows the index (Giulia first)", [v["name"] for v in lib] == names)
        check("entries marked stock with gender f/m and description",
              all(v["source"] == "stock" and v["gender"] in ("f", "m") and v["description"] for v in lib))
        check("second call adds nothing", store.install_stock(STOCK) == 0 and len(store.list()) == 10)
        sofia = next(v for v in store.list() if v["name"] == "Sofia")
        store.delete(sofia["id"])
        again = VoiceStore(Path(tmp) / "fresh")   # as after a restart
        check("a deleted stock voice does not come back after a restart",
              again.install_stock(STOCK) == 0 and "Sofia" not in [v["name"] for v in again.list()])

        # A v1.4 library: Giulia designed on the PC + a voice cloned by the user.
        old = VoiceStore(Path(tmp) / "v14")
        tone = (0.4 * np.sin(2 * np.pi * 220 * np.arange(REF_SR * 3) / REF_SR)).astype(np.float32)
        giulia = old.add(name="Giulia", audio=tone, sr=REF_SR, ref_text="ciao", lang="it", source="design",
                         instruct="Voce femminile italiana")
        mine = old.add(name="La mia voce", audio=tone, sr=REF_SR, ref_text="ciao", lang="it", source="file")
        added = old.install_stock(STOCK)
        lib = old.list()
        adopted = next(v for v in lib if v["id"] == giulia["id"])
        check("v1.4 library: existing Giulia adopted, not duplicated",
              [v["name"] for v in lib].count("Giulia") == 1 and adopted["source"] == "stock" and adopted["gender"] == "f")
        check("v1.4 library: the other nine added, user's own voice untouched",
              added == 9 and any(v["id"] == mine["id"] and v["source"] == "file" for v in lib), f"{added} added")
        reread = json.loads((Path(tmp) / "v14" / "voices.json").read_text(encoding="utf-8"))
        check("offered slugs persisted in voices.json", sorted(reread["stock_offered"]) == sorted(slugs))

    print(f"\nRESULT: {passed} passed, {failed} failed")
    return 1 if failed else 0


if __name__ == "__main__":
    raise SystemExit(main())
