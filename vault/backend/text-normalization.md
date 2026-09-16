# Text normalization (backend/gbtts/textproc.py)

Order in `normalize(text, lang)`: control chars → **user dictionary** → URLs ("link") → e-mails → emoji strip (keeps `° € $ £ % & @ + =`) → markdown marks → `…` → repeated punctuation → letters repeated ≥3 → laughter → ALL-CAPS ≥5 letters lowercased (models spell caps) → Italian rules → spacing.

Italian rules (`_normalize_it`), in order (order matters: abbreviations before numbers, dates/times before plain numbers):
1. chat abbreviations (`cmq, xké, nn, ke, tvb, raga, qnd, brb, afk, gg…`) and missing accents (`piu, perche, gia, cosi, pero, e'`) — whole-word, toggle "Expand chat abbreviations";
2. dates `14/09/2026` → "quattordici settembre duemilaventisei" (`1` → "primo");
3. times `21:30` → "ventuno e mezza", `:15` "e un quarto", `1:xx` "l'una";
4. money `5,50€` / `€ 5` / `5 euro` → "cinque euro e cinquanta";
5. percent → "per cento"; ordinals `3°`/`2ª` → "terzo"/"seconda";
6. units after numbers (`km, kg, ms, fps, GB, °C, …`);
7. `2+2`, `3x4`, `a=b` between digits;
8. remaining numbers: Italian thousands `1.234`, decimals `3,5` / `2.5`, negatives.

`split_segments`: sentence split; short pieces glued to neighbours (isolated "Ok." sounds clipped); first segment ≤ 140 chars (low TTFA), later ≤ 240; long sentences break at `; : ,` then space. Between segments the server inserts 110 ms of silence **through** the resampler/stretch/leveler (no discontinuity).

Tested cases: `backend` scratch test (see [[testing]]).
Traps found: `°` is Unicode category `So` → was stripped as emoji before the ordinal rule ran ("3°" read "tre").

Related: [[engines-and-models]].
