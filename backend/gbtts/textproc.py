"""Text normalisation for chat-style input, tuned for Italian.

Neural TTS models read numbers, symbols, URLs, emoji and chat abbreviations
badly or not at all ("xke", "14:30", "5,50EUR", "https://..."). Everything here
turns what people actually type in a voice-chat into what a person would say.

Rules are conservative on purpose: anything ambiguous is left for the model.
"""

from __future__ import annotations

import re
import unicodedata

try:
    from num2words import num2words
except Exception:  # pragma: no cover - optional dependency
    num2words = None

# ---------------------------------------------------------------------------
# Language table. Codes are what the plugin sends.
# ---------------------------------------------------------------------------
LANGUAGES = {
    "it": "Italian",
    "en": "English",
    "es": "Spanish",
    "fr": "French",
    "de": "German",
    "pt": "Portuguese",
    "ru": "Russian",
    "ja": "Japanese",
    "ko": "Korean",
    "zh": "Chinese",
}

_ITALIAN_HINTS = set(
    "il lo la gli le un una di da in con su per tra fra che non sono sei è e ma "
    "anche come cosa perché perche quando dove chi mi ti ci vi si ho hai ha abbiamo "
    "avete hanno questo questa quello quella ciao grazie sì si no bene male molto "
    "poco oggi domani ieri adesso ora sempre mai più piu tutto tutti ragazzi".split()
)
_ENGLISH_HINTS = set(
    "the a an of to in on for with and but or not is are was were be been i you he "
    "she it we they this that what when where who why how hello thanks yes no good "
    "bad very today tomorrow now always never all guys".split()
)


def detect_language(text: str, default: str = "it") -> str:
    words = re.findall(r"[a-zàèéìòù']+", text.lower())
    if not words:
        return default
    it = sum(w in _ITALIAN_HINTS for w in words)
    en = sum(w in _ENGLISH_HINTS for w in words)
    if it == en:
        return default
    return "it" if it > en else "en"


# ---------------------------------------------------------------------------
# Generic cleanup (all languages)
# ---------------------------------------------------------------------------
_URL = re.compile(r"(?:https?://|www\.)\S+", re.IGNORECASE)
_EMAIL = re.compile(r"\b[\w.+-]+@[\w-]+\.[\w.-]+\b")
_MD = re.compile(r"(\*\*|__|~~|`+)")
_REPEAT_PUNCT = re.compile(r"([!?.,;:])\1{1,}")
_ELLIPSIS = re.compile(r"\.{3,}|…")
_REPEAT_CHAR = re.compile(r"([a-zàèéìòù])\1{2,}", re.IGNORECASE)
_SPACES = re.compile(r"[ \t ]+")
_LAUGH = re.compile(r"\b(?:a?h[ae]h[ae](?:h[ae])*|a?ha(?:ha)+h?|xd+|lol+)\b", re.IGNORECASE)


_KEEP_SYMBOLS = set("°€$£%&@+=")


def _strip_emoji(text: str) -> str:
    out = []
    for ch in text:
        if ch in _KEEP_SYMBOLS:
            out.append(ch)
            continue
        cat = unicodedata.category(ch)
        cp = ord(ch)
        if cat in ("So", "Cs", "Co", "Cn") or 0x1F000 <= cp <= 0x1FAFF or 0x2600 <= cp <= 0x27BF \
                or 0xFE00 <= cp <= 0xFE0F or cp == 0x200D:
            out.append(" ")
        else:
            out.append(ch)
    return "".join(out)


def _control_cleanup(text: str) -> str:
    text = unicodedata.normalize("NFC", text)
    return "".join(ch if (ch == "\n" or unicodedata.category(ch)[0] != "C") else " " for ch in text)


def _fix_shouting(text: str) -> str:
    """ALL-CAPS words (>= 5 letters) are read letter by letter by some models."""
    def repl(m: re.Match) -> str:
        w = m.group(0)
        return w.lower() if len(w) >= 5 else w
    return re.sub(r"\b[A-ZÀÈÉÌÒÙ]{5,}\b", repl, text)


# ---------------------------------------------------------------------------
# Italian
# ---------------------------------------------------------------------------
_IT_SLANG = [
    (r"x(?:ch|k)[eéè]", "perché"),
    (r"perk[eèé]", "perché"),
    (r"cmq", "comunque"),
    (r"nn", "non"),
    (r"ke", "che"),
    (r"tvb", "ti voglio bene"),
    (r"tvtb", "ti voglio tanto bene"),
    (r"grz", "grazie"),
    (r"pls|plz", "per favore"),
    (r"raga", "ragazzi"),
    (r"qlc", "qualcosa"),
    (r"qlcn", "qualcuno"),
    (r"qnd|qndo", "quando"),
    (r"dv", "dove"),
    (r"sn", "sono"),
    (r"bn", "bene"),
    (r"msg", "messaggio"),
    (r"nsomma", "insomma"),
    (r"ok+", "ok"),
    (r"brb", "torno subito"),
    (r"afk", "lontano dalla tastiera"),
    (r"gg", "gi gi"),
    (r"wp", "ben giocato"),
    (r"idk", "non lo so"),
    (r"asap", "il prima possibile"),
    (r"btw", "comunque"),
    (r"ecc", "eccetera"),
    (r"piu", "più"),
    (r"perche", "perché"),
    (r"poiche", "poiché"),
    (r"benche", "benché"),
    (r"gia", "già"),
    (r"cosi", "così"),
    (r"pero", "però"),
    (r"citta", "città"),
    (r"e'", "è"),
    (r"es\.", "esempio"),
    (r"sig\.", "signor"),
    (r"dott\.", "dottor"),
]
_IT_SLANG_RE = [(re.compile(r"(?<![\w'])" + p + r"(?![\w'])", re.IGNORECASE), r) for p, r in _IT_SLANG]

_IT_MONTHS = ["gennaio", "febbraio", "marzo", "aprile", "maggio", "giugno", "luglio",
              "agosto", "settembre", "ottobre", "novembre", "dicembre"]

_IT_UNITS = {
    "km/h": "chilometri orari", "km": "chilometri", "kg": "chili", "cm": "centimetri",
    "mm": "millimetri", "m": "metri", "g": "grammi", "h": "ore", "min": "minuti",
    "sec": "secondi", "s": "secondi", "ms": "millisecondi", "gb": "gigabyte",
    "mb": "megabyte", "tb": "terabyte", "kb": "kilobyte", "fps": "fotogrammi al secondo",
    "hz": "hertz", "khz": "kilohertz", "w": "watt", "°c": "gradi", "°": "gradi",
    "mbps": "megabit al secondo", "ping": "di ping",
}


def _it_int(n: int) -> str:
    if num2words is None:
        return str(n)
    try:
        return num2words(n, lang="it")
    except Exception:
        return str(n)


def _it_number_token(raw: str) -> str:
    """'1.234,5' -> 'milleduecentotrentaquattro virgola cinque'."""
    s = raw
    neg = s.startswith("-")
    if neg:
        s = s[1:]
    if re.fullmatch(r"\d{1,3}(?:\.\d{3})+(?:,\d+)?", s):
        s = s.replace(".", "")
    if "," in s:
        ip, dp = s.split(",", 1)
    elif re.fullmatch(r"\d+\.\d{1,2}", s) or re.fullmatch(r"\d+\.\d{4,}", s):
        ip, dp = s.split(".", 1)
    else:
        ip, dp = s, ""
    ip = ip.replace(".", "")
    if not ip.isdigit():
        return raw
    words = _it_int(int(ip))
    if dp:
        if len(dp) <= 2 and not dp.startswith("0"):
            words += " virgola " + _it_int(int(dp))
        else:
            words += " virgola " + " ".join(_it_int(int(c)) for c in dp)
    return ("meno " if neg else "") + words


def _normalize_it(text: str) -> str:
    # Chat abbreviations first (they may contain digits-free tokens only).
    for rx, rep in _IT_SLANG_RE:
        text = rx.sub(rep, text)
    text = re.sub(r"(?<=\s)x(?=\s)", "per", text)

    # Dates dd/mm/yyyy or dd-mm-yyyy
    def date_repl(m: re.Match) -> str:
        d, mo, y = int(m.group(1)), int(m.group(2)), m.group(3)
        if not (1 <= d <= 31 and 1 <= mo <= 12):
            return m.group(0)
        day = "primo" if d == 1 else _it_int(d)
        out = f"{day} {_IT_MONTHS[mo - 1]}"
        if y:
            yy = int(y)
            if len(y) == 2:
                yy += 2000
            out += " " + _it_int(yy)
        return out
    text = re.sub(r"\b(\d{1,2})[/-](\d{1,2})(?:[/-](\d{2}|\d{4}))?\b", date_repl, text)

    # Times hh:mm
    def time_repl(m: re.Match) -> str:
        h, mi = int(m.group(1)), int(m.group(2))
        hs = "l'una" if h == 1 else _it_int(h)
        if mi == 0:
            return hs
        if mi == 30:
            return f"{hs} e mezza"
        if mi == 15:
            return f"{hs} e un quarto"
        return f"{hs} e {_it_int(mi)}"
    text = re.sub(r"\b([01]?\d|2[0-3]):([0-5]\d)\b", time_repl, text)

    # Money: 5€ / € 5,50 / 5,50 euro
    def money(amount: str) -> str:
        a = amount.replace(".", "") if re.fullmatch(r"\d{1,3}(?:\.\d{3})+(?:,\d{1,2})?", amount) else amount
        a = a.replace(".", ",")
        if "," in a:
            ip, dp = a.split(",", 1)
            dp = (dp + "0")[:2]
            euros = f"{_it_int(int(ip))} euro"
            if int(dp):
                euros += f" e {_it_int(int(dp))}"
            return euros
        return f"{_it_int(int(a))} euro"
    text = re.sub(r"€\s*(\d[\d.,]*)", lambda m: money(m.group(1).rstrip(".,")), text)
    text = re.sub(r"(\d[\d.,]*)\s*(?:€|eur(?:o)?\b)", lambda m: money(m.group(1).rstrip(".,")), text, flags=re.IGNORECASE)

    # Percent
    text = re.sub(r"(-?\d[\d.,]*)\s*%", lambda m: _it_number_token(m.group(1)) + " per cento", text)

    # Ordinals 1° 2ª
    def ordinal(m: re.Match) -> str:
        n = int(m.group(1))
        if num2words is None:
            return m.group(0)
        try:
            w = num2words(n, lang="it", to="ordinal")
        except Exception:
            return m.group(0)
        if m.group(2) in ("ª", "a") and w.endswith("o"):
            w = w[:-1] + "a"
        return w
    text = re.sub(r"\b(\d{1,4})([°ºª])(?!\s*[cC]\b)", ordinal, text)

    # Units after numbers
    unit_keys = sorted(_IT_UNITS.keys(), key=len, reverse=True)
    unit_rx = re.compile(r"(\d[\d.,]*)\s?(" + "|".join(re.escape(u) for u in unit_keys) + r")(?![\w/])", re.IGNORECASE)
    text = unit_rx.sub(lambda m: f"{_it_number_token(m.group(1))} {_IT_UNITS[m.group(2).lower()]}", text)

    # Simple arithmetic symbols between numbers
    text = re.sub(r"(?<=\d)\s*\+\s*(?=\d)", " più ", text)
    text = re.sub(r"(?<=\d)\s*=\s*(?=\d)", " uguale ", text)
    text = re.sub(r"(?<=\d)\s*[xX*]\s*(?=\d)", " per ", text)

    # Remaining numbers
    text = re.sub(r"(?<![\w])-?\d[\d.,]*\d|(?<![\w])-?\d", lambda m: _it_number_token(m.group(0)), text)

    text = text.replace("&", " e ").replace("@", " chiocciola ")
    return text


def _normalize_generic(text: str) -> str:
    text = text.replace("&", " and ")
    return text


# ---------------------------------------------------------------------------
# Public API
# ---------------------------------------------------------------------------
def apply_dictionary(text: str, dictionary: dict[str, str] | None) -> str:
    """User pronunciation dictionary (whole-word, case-insensitive)."""
    if not dictionary:
        return text
    for src in sorted(dictionary.keys(), key=len, reverse=True):
        dst = dictionary[src]
        if not src.strip():
            continue
        rx = re.compile(r"(?<![\w])" + re.escape(src) + r"(?![\w])", re.IGNORECASE)
        text = rx.sub(lambda _m, d=dst: d, text)
    return text


def normalize(text: str, lang: str, *, chat_slang: bool = True, numbers: bool = True,
              dictionary: dict[str, str] | None = None) -> str:
    text = _control_cleanup(text)
    text = apply_dictionary(text, dictionary)
    text = _URL.sub(" link ", text)
    text = _EMAIL.sub(" indirizzo email " if lang == "it" else " email address ", text)
    text = _strip_emoji(text)
    text = _MD.sub("", text)
    text = _ELLIPSIS.sub("…", text)
    text = _REPEAT_PUNCT.sub(r"\1", text)
    text = _REPEAT_CHAR.sub(r"\1\1", text)
    text = _LAUGH.sub("ahah", text)
    text = _fix_shouting(text)
    if lang == "it":
        if not chat_slang:
            saved = list(_IT_SLANG_RE)
            _IT_SLANG_RE.clear()
            try:
                text = _normalize_it(text) if numbers else text
            finally:
                _IT_SLANG_RE.extend(saved)
        elif numbers:
            text = _normalize_it(text)
        else:
            for rx, rep in _IT_SLANG_RE:
                text = rx.sub(rep, text)
    else:
        text = _normalize_generic(text)
    text = _SPACES.sub(" ", text)
    text = re.sub(r"\s+([,.;:!?…])", r"\1", text)
    text = re.sub(r"\n\s*\n+", "\n", text)
    return text.strip()


_SENT_END = re.compile(r"(?<=[.!?…])\s+|\n+")


def split_segments(text: str, first_max: int = 140, max_len: int = 240, min_len: int = 30) -> list[str]:
    """Split into speakable segments.

    Short first segment => low time-to-first-audio; later segments longer so the
    model keeps prosody across clauses. Very long sentences break at the last
    comma/semicolon/colon, then at a space.
    """
    raw = [s.strip() for s in _SENT_END.split(text) if s and s.strip()]
    merged: list[str] = []
    for s in raw:
        # A short piece is glued to its neighbour: isolated one-word segments
        # ("link", "Ok.") get a flat, clipped reading from every model.
        if merged and (len(merged[-1]) < min_len or len(s) < min_len // 2) \
                and len(merged[-1]) + 1 + len(s) <= max_len:
            merged[-1] = merged[-1] + " " + s
        else:
            merged.append(s)

    out: list[str] = []
    for s in merged:
        limit = first_max if not out else max_len
        while len(s) > limit:
            cut = -1
            for sep in (";", ":", ",", " – ", " - "):
                cut = s.rfind(sep, int(limit * 0.4), limit)
                if cut != -1:
                    cut += len(sep)
                    break
            if cut == -1:
                cut = s.rfind(" ", int(limit * 0.4), limit)
            if cut == -1:
                cut = limit
            out.append(s[:cut].strip())
            s = s[cut:].strip()
            limit = max_len
        if s:
            out.append(s)
    return [s for s in out if re.search(r"\w", s)]
