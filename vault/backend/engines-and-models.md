# Engines and models

## Choice (researched 2026-09-14)
| candidate | Italian | local GPU | streaming | license | verdict |
|---|---|---|---|---|---|
| **Qwen3-TTS 12Hz 1.7B/0.6B** (Alibaba, 2026) | native | yes, CUDA graphs via faster-qwen3-tts | yes | Apache-2.0 (wrapper MIT) | **primary** |
| Supertonic 3 (99M ONNX) | yes (31 langs) | CPU only (GPU "not supported yet") | per sentence | open weights | **fallback, no GPU needed** |
| Voxtral 4B TTS (Mistral) | yes | 4B ≈ 8 GB bf16 on a 10 GB card | yes | CC BY-NC 4.0 | rejected: non-commercial, VRAM |
| Chatterbox Multilingual | yes | yes | first chunk ~0.47 s on 4090 | MIT | rejected: slower, weaker Italian prosody reports |
| Kokoro-82M | 2 voices, espeak G2P | CPU/GPU | yes | Apache-2.0 | rejected: flat Italian intonation |
| XTTS-v2 | yes | yes | yes | CPML non-commercial | rejected: licence, older quality |

## Qwen model roles (only one in VRAM, `QwenEngine::ensure`)
- **Base** — voice cloning; every user voice and the two first-run Italian voices use it. Daily use loads only this.
- **VoiceDesign** (1.7B only) — create a voice from a description; loaded while designing, then swapped out.
- **CustomVoice** — 9 stock speakers (non-Italian natives) + `instruct`; downloaded only if selected (`kind: builtin_download`).

A voice = `voices/<id>/ref.wav` (≤ 15 s, 24 kHz) + transcript; prompt cached as `prompt_<size>.pt` (ICL mode when transcript present, x-vector otherwise).
Up to v1.4 the first run designed "Giulia" and "Marco" on the user's GPU (`create_default_voices`, VoiceDesign load 171 s on a cold cache). Since v1.5 see [[#Italian stock voices (v1.5)]].

## Italian stock voices (v1.5)
Ten ORIGINAL synthetic voices ship in `backend/gbtts/stock_voices/<slug>/ref.wav` + `stock_voices.json` (4.4 MB); `VoiceStore.install_stock` copies each one into the library once (`stock_offered` in voices.json: a deleted one never returns; a v1.4 "Giulia"/"Marco" of source design is adopted, not duplicated). Group "Italian voices (included)" in the UI; also voice-changer targets. The VoiceDesign model is no longer part of the default install (downloads on first design).
**No real person's recording is cloned** (request 2026-09-16 "trovandole anche in rete"): impersonation, the plugin forbids it for users too.

Built by `backend/tools/make_stock_voices.py` (design → asr → pick) on the RTX 3080, 2026-09-16: VoiceDesign 1.7B renders each voice's script with 3 seeds, the Base 1.7B model clones each candidate and reads 3 gaming sentences (the real use), Whisper large-v3-turbo CER, pYIN median F0, then SpeechBrain ECAPA (isolated eval venv) for distinctness.

| Voice | Seed | Clone CER | F0 ref/clone | ECAPA ref↔clone |
|---|---|---|---|---|
| Giulia (f, imported) | v1.x | 0.000 | 270/247 | 0.85 |
| Marco (m, imported) | v1.x | 0.000 | 185/189 (kept: users know it) | 0.93 |
| Sofia (f, 20) | 20260916 | 0.012 | 379/362 (pYIN) | 0.86 |
| Elena (f, 40, narrator) | 20261016 | 0.000 | 224/211 | 0.94 |
| Chiara (f, news anchor) | 20261116 | 0.000 | 225/218 | 0.90 |
| Rosa (f, elderly) | 20261217 | 0.000 | 198/193 | 0.91 |
| Luca (m, 25) | 20261317 | 0.000 | 133/147 | 0.84 |
| Alessandro (m, radio) | 20261418 | 0.000 | 143/152 | 0.90 |
| Giorgio (m, elderly) | 20261517 | 0.000 | 144/146 | 0.92 |
| Davide (m, 35) | 20261616 | 0.000 | 124/128 | 0.88 |

Findings:
- All 26 candidates were intelligible (ref CER ≤ 0.065, the non-zero ones are Whisper writing "9" for "nove"). Selection is about **pitch and distinctness**, not intelligibility.
- **"ragazzo" reads as a boy**: "ragazzo di vent'anni, timbro giovane e brillante, entusiasmo" → 262-362 Hz on 3/3 seeds; "voce da uomo ... per niente acuto" still 4/5 above 245 Hz; "giovane uomo sui venticinque anni, timbro baritonale" → one seed at 133/147 Hz. Excited/enthusiastic wording raises pitch in general.
- **The Qwen x-vector (`ref_spk_embedding`) saturates**: cosine 0.93-0.99 for every pair, woman vs man included — useless to tell voices apart (same as WavLM-base-plus-sv in [[voice-changer]]). ECAPA separates them: own clone 0.84-0.94, other voices 0.07-0.68.
- Seeds chosen by brute force over the plausible candidates to minimise the maximum pairwise ECAPA similarity: 0.628 (Luca–Alessandro, single candidates). Keeping Sofia's clearest seed (918) would have raised Sofia–Elena to 0.66.

## Measured — RTX 3080 10 GB, torch 2.11 cu128, transformers 5.15.1 (tools/bench.py)
| config | TTFA (median) | RTF | VRAM peak |
|---|---|---|---|
| Qwen 1.7B chunk 2, prefill | 225 ms | 1.37 | 4.7 GB |
| **Qwen 1.7B chunk 4, prefill (default)** | 311 ms | 1.42 | 4.7 GB |
| Qwen 1.7B chunk 8, prefill | 477 ms | 1.78 | 4.7 GB |
| Qwen 0.6B chunk 4, prefill | 259 ms | 1.81 | 2.7 GB |
| Supertonic 3 CPU (i5-13600K, 10 steps) | 1.8 s / sentence | 3.3 | 0 |

Intelligibility: Whisper large-v3-turbo transcription of all 45 renders → **CER 0.000** for every configuration (Italian gaming-chat sentences, questions rendered with interrogative intonation — Whisper punctuated them with "?").
RTF on a 3080 is ~1.4–1.8 while streaming (4090 reports 4.8 non-streaming): real-time with margin; if a game saturates the GPU raise the start buffer or use 0.6B.

## Kokoro-82M (v1.1) — the light engine (`engines/kokoro_engine.py`)
kokoro-onnx 0.6.1 on onnxruntime 1.30 CPU, espeak-ng bundled by `espeakng-loader` (no system install), models from the GitHub release `model-files-v1.1` (`kokoro-v1.0.onnx` 325 505 369 B, `kokoro-v1.0.int8.onnx` 114 119 327 B, `voices-v1.0.bin` 28 214 398 B; resumable download, exact size check). Native Italian voices `if_sara`, `im_nicola` (VOICES.md grade C, target B); ids `ko:<voice>`; ja/zh voices hidden (need misaki G2P).

Measured (i5-13600K, 6 intra-op threads, tools/bench.py, 5 Italian sentences × 2 voices):
| variant | RTF | time for a sentence | CER (Whisper) |
|---|---|---|---|
| **fp32 (default)** | 2.6–2.9 | ~1.7 s | 0.000 Sara / 0.012 Nicola |
| int8 | 0.35–0.38 | ~12–13 s | same |
The 0.012 is Whisper writing "9" for "nove", not a reading error. (Bench RTF above was taken with other processes busy.)

Session micro-benchmark on a **quiet** CPU (fp32, 6.5 s sentence, scratch `kokoro_micro.py`): ORT default threads RTF 5.5–6.0 · **8 threads 5.1 (default since v1.1)** · 6 threads 4.4 · 4 threads 3.9. Graph optimisation level (all/basic) and `trim` make no measurable difference: time is the graph.

Real use = a different length every sentence (scratch `kokoro_micro2/3.py`, 8 threads):
| ORT option | RTF | notes |
|---|---|---|
| defaults (mem_pattern on) | 3.4–5.0 | re-plans per shape, worst chunk 2.0× |
| `enable_mem_pattern=False` | 5.0–5.1 | worst chunk 4.1× |
| + `session.intra_op.allow_spinning=0` (**shipped**) | 5.6–5.7 | **0 core-s** idle CPU after an inference (spinning burnt 2.2–2.4 core-s per second) |
Phonemizer with the cached backend: 0.1 ms per text. First clause "Adesso parla Nicola," ≈ 0.32 s.
Backend e2e through the protocol (v1.1 final): time to first PCM **292 ms**, 348 ms at speed 1.25, 502 ms right after a Qwen→Kokoro switch.

Memory, one engine per fresh process (tools/measure_memory.py):
| engine | RSS after synthesis | VRAM | torch imported | HTTP requests at load |
|---|---|---|---|---|
| Kokoro fp32 | 595 MB | 0 | no | 0 |
| Kokoro int8 | 428 MB | 0 | no | 0 |
| Supertonic 3 | 514 MB | 0 | no | 0 |
| Qwen 0.6B | 2 225 MB | 2 746 MB | yes | 0 |
| Qwen 1.7B | 2 183 MB | 4 662 MB | yes | 0 |

Traps:
- **int8 export is 7× slower than fp32 on CPU**: its dynamic-quantisation kernels (ConvInteger/MatMulInteger) run on ~one core (8 % load on a 20-thread CPU while synthesising). Known ORT behaviour. int8 only saves download size → fp32 is the default everywhere (engine, server cfg, plugin Settings, installer `kokoro` key).
- `phonemizer.phonemize()` builds a new espeak backend on every call: **~250 ms per call**. The engine keeps one `EspeakBackend` per language and calls `Kokoro.create(..., is_phonemes=True)`; lock `kokoro_onnx.tokenizer._espeak_lock` (espeak global state).
- Not streaming: first audio = whole segment. `_first_clause_split` synthesises the first clause (comma/semicolon/colon between chars 12–90, texts ≥ 60 chars) first, then the rest, with a 90 ms pause re-inserted (trim removed the model's own).
- `trim.py`/`pauses.py` in kokoro_onnx are vectorised numpy — not a bottleneck (checked).

## Resource policy (v1.1)
- The **voice decides the engine** (plugin `Controller::setVoice` → `settings.engine` → `configure`); on `configure` the backend **unloads every engine except the selected one**.
- Engine availability checks are import-free (`importlib.util.find_spec`, `ctypes.WinDLL("nvcuda.dll")`): a Kokoro-only backend never imports PyTorch (~1 GB RAM once imported, never returned).
- Qwen loads from the local snapshot **directory** (`snapshot_download(local_files_only=True)`): with a repo id, qwen-tts ignored `local_files_only` for its processor/tokenizer and every start made ~20 HTTP requests to Hugging Face (seen in the v1.0 user log, 10 s load).

## Version pins (backend/requirements.txt)
- `transformers==5.15.1` — 5.17.0 breaks `_init_weights` for `MimiConfig` (`rope_theta` moved). Symptom: `'MimiConfig' object has no attribute 'rope_theta'` on every load.
- `faster-qwen3-tts==0.4.0`, `qwen-tts-hf==0.1.1.post1`, torch 2.11.0 cu128 (driver CUDA 13.3 OK).
- `sox` warning "SoX could not be found!" at import is harmless (qwen-tts imports the wrapper, never calls it).

## Network trap
Kaspersky "Anti-Virus Personal Root" intercepts HTTPS: pip works (truststore), huggingface_hub/httpx with certifi fails `CERTIFICATE_VERIFY_FAILED: self-signed certificate in certificate chain`. Fix: `gbtts/netfix.py` → `truststore.inject_into_ssl()` (Windows store, verification stays on).

Related: [[text-normalization]], [[testing]], [[backend-protocol]].
