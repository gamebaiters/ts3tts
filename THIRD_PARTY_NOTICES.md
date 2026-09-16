# Third-party notices — GameBaiters TTS

The plugin package (`gb_tts_win64.dll` and `plugins/gb_tts/`) contains or is built from the following
third-party components.

## Compiled into the plugin DLL

### RP Soundboard / GameBaiters Soundboard (DSP chain, effects editor, theme)
Source: https://github.com/gamebaiters/RP-Soundboard (fork of https://github.com/MGraefe/RP-Soundboard),
compiled in place at a pinned commit.

```
MIT License

Copyright (c) 2020 Marius Gräfe

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
```

### libmysofa 1.3.2 — BSD 3-Clause License
Copyright (c) 2016-2017, Symonics GmbH, Christian Hoene. https://github.com/hoene/libmysofa

### zlib 1.3.1 — zlib License
Copyright (C) 1995-2024 Jean-loup Gailly and Mark Adler. https://zlib.net

### HRTF data (`default.sofa`, embedded resource)
Neumann KU100 far-field HRIR set, TH Köln (Benjamin Bernschütz), converted to SOFA by the Acoustics
Research Institute (OeAW). License declared in the file: Creative Commons Attribution-ShareAlike 3.0
(CC BY-SA 3.0).

### Qt 5.15.2 — LGPL v3
Not distributed: the plugin links dynamically against the Qt libraries shipped with the TeamSpeak 3 client.

### TeamSpeak 3 Client Plugin SDK headers
Used to build the plugin; see https://github.com/TeamSpeak-Systems/ts3client-pluginsdk.

## Voice engine (`plugins/gb_tts/backend`)

- Vendored MeanVC2 / WavLM / x-transformers code: see `backend/gbtts/vc/third_party/NOTICE.md`
  (Apache License 2.0 and MIT).
- Python packages (PyTorch, faster-qwen3-tts, kokoro-onnx, supertonic, …) are **not** shipped: the
  engine installer downloads them from their official indexes onto the user's PC, under their own licenses.
- Models (Qwen3-TTS, Kokoro-82M, Supertonic, MeanVC2, WavLM) are **not** shipped: they are downloaded
  from Hugging Face on the user's PC under the licenses stated on their model cards.
