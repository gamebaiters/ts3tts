# Vendored third-party code

| Path | Origin | License | Changes |
|---|---|---|---|
| `meanvc2/dit.py`, `meanvc2/modules.py`, `meanvc2/dit_modules.py`, `meanvc2/speaker.py` | [ASLP-lab/MeanVC2](https://github.com/ASLP-lab/MeanVC2) `runtime/src/` (cloned 2026-09-15) | Apache License 2.0 (declared in the project README and on Hugging Face `ASLP-lab/MeanVC2`; the repository carries no LICENSE file) | rotary import → `rotary.py`; `speaker.py`: s3prl replaced by `_WavLMHiddenStates` (same hidden states, verified numerically) |
| `wavlm/WavLM.py`, `wavlm/modules.py` | [s3prl](https://github.com/s3prl/s3prl) 0.4.18 `s3prl/upstream/wavlm/`, itself from [microsoft/unilm WavLM](https://github.com/microsoft/unilm/tree/master/wavlm) (fairseq based) | MIT (Microsoft) | none |
| `rotary.py` | `RotaryEmbedding` / `apply_rotary_pos_emb` from [x-transformers](https://github.com/lucidrains/x-transformers) 2.2.11 | MIT (Phil Wang) | reduced to the code paths MeanVC2 uses, einops → torch ops |

Model weights are NOT in this repository; `tools/download_models.py` fetches them:
- `ASLP-lab/MeanVC2` (Hugging Face): `fastu2pp_*.pt`, `meanvc2_*_40ms.safetensors`, `vocos.pt` — Apache-2.0
- WavLM-Large config: shipped as `wavlm/wavlm_large_cfg.json` (the `cfg` dict of `s3prl/converted_ckpts/wavlm_large.pt`, 2 KB) — MIT
- `wavlm_large_finetune.pth` (WavLM-Large + ECAPA-TDNN speaker verification, Microsoft UniSpeech) — MIT. Downloaded from Hugging Face `yfyeung/wavlm-large-speaker-verification` file `wavlm-large.pt`, byte-identical to MeanVC2's Google Drive link (1 301 926 579 bytes, SHA-256 `51f07e3b94d9e0262a6a675ef5a087be3dd09e8c62e9d886827f44f82fe7f94b`, checked 2026-09-15). Needed only to compute a target voice's embedding once; not loaded while converting.

Voice conversion changes how a person sounds: use only your own voice, or a voice you have explicit permission to use.
