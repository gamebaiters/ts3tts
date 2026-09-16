"""Backend process entry point.

    python -m gbtts.server --port <p> --token <t> --home <dir> [--parent-pid <pid>]

Threads
    main    reads frames from the plugin; cancel/ping/shutdown handled inline so
            they are never stuck behind a long synthesis
    worker  runs every other op in order (model loads, jobs, voice ops) -
            the GPU is used from this one thread only
    parent  waits on the TeamSpeak process handle and exits with it (the plugin
            also puts us in a kill-on-close Job Object; this is belt and braces)
"""

from __future__ import annotations

import argparse
import json
import logging
import os
import queue
import sys
import threading
import time
import traceback
from pathlib import Path

import numpy as np

from . import PROTOCOL_VERSION, __version__
from .protocol import SAMPLE_RATE, T_JSON, T_MIC, ConnectionClosed, FrameConnection

log = logging.getLogger("gbtts")


# ---------------------------------------------------------------------------
def _watch_parent(pid: int) -> None:
    if pid <= 0:
        return
    if sys.platform == "win32":
        import ctypes
        SYNCHRONIZE = 0x00100000
        k32 = ctypes.windll.kernel32
        handle = k32.OpenProcess(SYNCHRONIZE, False, pid)
        if not handle:
            log.warning("cannot open parent process %d", pid)
            return
        k32.WaitForSingleObject(handle, 0xFFFFFFFF)
        log.info("parent process exited, shutting down")
        os._exit(0)
    else:
        while True:
            try:
                os.kill(pid, 0)
            except OSError:
                os._exit(0)
            time.sleep(1.0)


class Job:
    __slots__ = ("id", "text", "lang", "voice", "speed", "instruct", "local", "cancel")

    def __init__(self, op: dict):
        self.id = int(op.get("id", 0)) & 0xFFFFFFFF
        self.text = str(op.get("text", ""))
        self.lang = str(op.get("lang", "it") or "it")
        self.voice = str(op.get("voice", ""))
        self.speed = float(op.get("speed", 1.0) or 1.0)
        self.instruct = str(op.get("instruct", "") or "")
        self.local = bool(op.get("local", False))
        self.cancel = threading.Event()


class Backend:
    def __init__(self, conn: FrameConnection, home: Path):
        from .voices import VoiceStore

        self.conn = conn
        self.home = home
        self.store = VoiceStore(home / "voices")
        try:
            added = self.store.install_stock(Path(__file__).resolve().parent / "stock_voices")
            if added:
                log.info("added %d Italian stock voices to the library", added)
        except Exception as exc:  # noqa: BLE001 - a convenience, never fatal
            log.warning("stock voices not installed: %s", exc)
        self.queue: "queue.Queue[dict]" = queue.Queue()
        self.lock = threading.Lock()
        self.current: Job | None = None
        self.cancelled: set[int] = set()
        self.epoch = 0
        self.cfg = {
            "engine": "qwen", "qwen_size": "1.7B", "chunk_size": 4, "full_text_prefill": True,
            "temperature": 0.8, "leveler": True, "chat_slang": True, "numbers": True,
            "dictionary": {}, "supertonic_steps": 10, "idle_unload_min": 0, "kokoro_variant": "fp32",
        }
        self.engines: dict[str, object] = {}
        self.pending_design: dict[str, dict] = {}
        self.vc = None                      # VoiceChangerService, created on first vc_configure
        self.last_activity = time.monotonic()
        self.worker = threading.Thread(target=self._worker_loop, name="gbtts-worker", daemon=True)

    # ---- outbound --------------------------------------------------------------
    def emit(self, ev: str, **fields) -> None:
        fields["ev"] = ev
        try:
            self.conn.send_json(fields)
        except ConnectionClosed:
            os._exit(0)

    def status(self, state: str, message: str = "") -> None:
        info = {}
        eng = self.engines.get(self.cfg["engine"])
        if eng is not None:
            try:
                info = eng.info()
            except Exception:
                info = {}
        # PyTorch never gives its memory back once imported (~2 GB RSS + CUDA
        # context): the plugin restarts a backend that switched to a light engine.
        self.emit("status", state=state, message=message, torch_loaded="torch" in sys.modules,
                  selected_engine=self.cfg["engine"], **info)

    # ---- inbound (reader thread) ---------------------------------------------------
    def on_message(self, op: dict) -> None:
        name = op.get("op")
        if name == "cancel":
            jid = int(op.get("id", 0)) & 0xFFFFFFFF
            with self.lock:
                self.cancelled.add(jid)
                if self.current is not None and self.current.id == jid:
                    self.current.cancel.set()
        elif name == "cancel_all":
            with self.lock:
                self.epoch += 1
                if self.current is not None:
                    self.current.cancel.set()
        elif name == "ping":
            self.emit("pong", t=op.get("t"))
        elif name in ("vc_configure", "vc_list_targets", "vc_add_target"):
            # Own worker: a model load for the voice changer never waits behind a TTS job.
            if self.vc is None:
                from .vc.service import VoiceChangerService
                self.vc = VoiceChangerService(self.conn, self.home, self.store, self.emit)
            if name == "vc_configure":
                self.vc.configure(op)
            elif name == "vc_list_targets":
                self.vc.list_targets(op.get("req"))
            else:
                self.vc.add_target(op)
        elif name == "shutdown":
            log.info("shutdown requested")
            self.conn.close()
            os._exit(0)
        else:
            op["_epoch"] = self.epoch
            self.queue.put(op)

    def on_mic(self, payload: bytes) -> None:
        if self.vc is None:
            return
        try:
            seq, pcm = FrameConnection.parse_audio(payload)
        except ValueError:
            return
        self.vc.mic(seq, pcm)

    # ---- worker ------------------------------------------------------------------------
    def _worker_loop(self) -> None:
        while True:
            try:
                op = self.queue.get(timeout=5.0)
            except queue.Empty:
                self._idle_tick()
                continue
            self.last_activity = time.monotonic()
            name = op.get("op")
            try:
                handler = getattr(self, f"op_{name}", None)
                if handler is None:
                    self.emit("op_error", op=name, req=op.get("req"), message=f"operazione sconosciuta: {name}")
                    continue
                handler(op)
            except Exception as exc:  # noqa: BLE001 - never kill the worker
                log.error("op %s failed: %s\n%s", name, exc, traceback.format_exc())
                if name == "speak":
                    self.emit("job_error", id=int(op.get("id", 0)), message=str(exc))
                else:
                    self.emit("op_error", op=name, req=op.get("req"), message=str(exc))
                if "out of memory" in str(exc).lower():
                    self._unload_all()
                    self.status("error", "Memoria GPU esaurita: modello scaricato. Chiudi altre app che usano la GPU.")
            self.last_activity = time.monotonic()

    def _idle_tick(self) -> None:
        minutes = float(self.cfg.get("idle_unload_min", 0) or 0)
        if minutes <= 0:
            return
        eng = self.engines.get("qwen")
        if eng is not None and eng.is_loaded() and time.monotonic() - self.last_activity > minutes * 60:
            eng.unload()
            self.status("idle", "Modello scaricato dalla GPU per inattività: si ricarica al prossimo messaggio.")

    def _unload_all(self) -> None:
        for eng in self.engines.values():
            try:
                eng.unload()
            except Exception:
                pass

    def _engine(self, key: str):
        eng = self.engines.get(key)
        if eng is not None:
            return eng
        if key == "qwen":
            from .engines.qwen_engine import QwenEngine
            eng = QwenEngine(self.store, size=self.cfg["qwen_size"], chunk_size=self.cfg["chunk_size"],
                             temperature=self.cfg["temperature"], full_text_prefill=self.cfg["full_text_prefill"])
        elif key == "supertonic":
            from .engines.supertonic_engine import SupertonicEngine
            eng = SupertonicEngine(self.home / "models", steps=self.cfg["supertonic_steps"])
        elif key == "kokoro":
            from .engines.kokoro_engine import KokoroEngine
            eng = KokoroEngine(self.home / "models", variant=self.cfg["kokoro_variant"])
        else:
            raise ValueError(f"motore sconosciuto: {key}")
        self.engines[key] = eng
        return eng

    @staticmethod
    def _engine_key_for_voice(voice: str) -> str:
        if voice.startswith("st:"):
            return "supertonic"
        if voice.startswith("ko:"):
            return "kokoro"
        return "qwen"

    def _engine_for_voice(self, voice: str):
        return self._engine(self._engine_key_for_voice(voice))

    def _voices(self) -> list[dict]:
        """Voices of the SELECTED engine only. Offering another engine's voices
        would invite loading that engine too - RAM/VRAM the user chose not to spend."""
        key = self.cfg["engine"]
        prefix = {"kokoro": "ko:", "supertonic": "st:"}.get(key, "")
        out = []
        try:
            eng = self._engine(key)
            if eng.available()[0]:
                for v in eng.voices():
                    v = dict(v)
                    v["id"] = prefix + v["id"]
                    v.setdefault("gender", "")
                    out.append(v)
        except Exception as exc:  # noqa: BLE001
            log.warning("voices from %s failed: %s", key, exc)
        return out

    def _emit_voices(self, req=None) -> None:
        # "engine" lets the plugin drop a list that belongs to an engine it already left.
        self.emit("voices", engine=self.cfg["engine"], voices=self._voices(), req=req)

    def _require_qwen(self) -> None:
        if self.cfg["engine"] != "qwen":
            raise ValueError("la creazione di voci richiede il motore Qwen3-TTS (Impostazioni › Motore)")

    # ---- ops -------------------------------------------------------------------------
    def op_configure(self, op: dict) -> None:
        changed_qwen = False
        for k in list(self.cfg.keys()):
            if k in op:
                if k in ("qwen_size", "chunk_size", "full_text_prefill", "temperature") and op[k] != self.cfg[k]:
                    changed_qwen = True
                self.cfg[k] = op[k]
        if isinstance(self.cfg.get("dictionary"), dict) is False:
            self.cfg["dictionary"] = {}

        if "kokoro" in self.engines and self.engines["kokoro"].variant != self.cfg["kokoro_variant"]:
            self.engines["kokoro"].variant = self.cfg["kokoro_variant"]   # reloaded lazily by load()

        if changed_qwen and "qwen" in self.engines:
            eng = self.engines["qwen"]
            eng.size = self.cfg["qwen_size"] if self.cfg["qwen_size"] in ("1.7B", "0.6B") else "1.7B"
            eng.chunk_size = int(max(1, min(12, int(self.cfg["chunk_size"]))))
            eng.full_text_prefill = bool(self.cfg["full_text_prefill"])
            eng.temperature = float(self.cfg["temperature"])
            if eng.is_loaded() and eng._model_size != eng.size:
                eng.unload()

        engines = []
        for key in ("qwen", "kokoro", "supertonic"):
            eng = self._engine(key)
            ok, why = eng.available()
            engines.append({"key": key, "label": eng.label, "available": ok, "reason": why})
        self.emit("engines", engines=engines)

        key = self.cfg["engine"] if self.cfg["engine"] in ("qwen", "kokoro", "supertonic") else "kokoro"
        eng = self._engine(key)
        ok, why = eng.available()
        if not ok:
            # Same order as the plugin (Controller, "engines" event): lightest first.
            for alt in ("kokoro", "supertonic", "qwen"):
                if alt != key and self._engine(alt).available()[0]:
                    self.status("warning", f"{why}: uso {self._engine(alt).label}.")
                    key = alt
                    break
            self.cfg["engine"] = key
            eng = self._engine(key)

        # Only the selected engine stays in memory: that is the whole point of
        # choosing a light one. Voices of other engines are not even listed.
        for other_key, other in self.engines.items():
            if other_key != key and other.is_loaded():
                other.unload()
                log.info("unloaded %s to free memory", other_key)
        eng.load(self.status)
        self._emit_voices()
        self.status("ready", "Pronto")

    def op_list_voices(self, op: dict) -> None:
        self._emit_voices(req=op.get("req"))

    def op_unload(self, op: dict) -> None:
        self._unload_all()
        self.status("idle", "GPU liberata: il modello si ricarica al prossimo messaggio.")

    def op_speak(self, op: dict) -> None:
        from . import textproc
        from .audio import Leveler, StreamResampler, WsolaStretch, fade, to_int16
        from .engines.base import EngineError, SynthRequest

        job = Job(op)
        with self.lock:
            stale = op.get("_epoch", 0) != self.epoch or job.id in self.cancelled
            self.cancelled.discard(job.id)
            if not stale:
                self.current = job
        if stale:
            self.emit("job_end", id=job.id, cancelled=True, audio_ms=0, gen_ms=0)
            return

        try:
            lang = job.lang if job.lang != "auto" else textproc.detect_language(job.text, "it")
            text = textproc.normalize(job.text, lang, chat_slang=bool(self.cfg["chat_slang"]),
                                      numbers=bool(self.cfg["numbers"]), dictionary=self.cfg.get("dictionary") or {})
            segments = textproc.split_segments(text)
            if not segments:
                self.emit("job_end", id=job.id, cancelled=False, audio_ms=0, gen_ms=0, empty=True)
                return

            voice_engine = self._engine_key_for_voice(job.voice)
            if voice_engine != self.cfg["engine"]:
                raise EngineError(f"la voce {job.voice} appartiene a {voice_engine}, "
                                  f"ma il motore attivo è {self.cfg['engine']}")
            eng = self._engine(voice_engine)
            if not eng.is_loaded() or (eng.key == "qwen" and eng.loaded_role() not in ("base", "custom")):
                eng.load(self.status)
                self.status("ready", "Pronto")
            voice = job.voice[3:] if job.voice.startswith(("st:", "ko:")) else job.voice

            self.emit("job_start", id=job.id, text=text, segments=len(segments), lang=lang)
            t0 = time.perf_counter()
            ttfa = None
            sent = 0
            resampler: StreamResampler | None = None
            stretch = WsolaStretch(1.0 if eng.native_speed else job.speed)
            leveler = Leveler(enabled=bool(self.cfg["leveler"]))
            first_chunk = True

            def send(y: np.ndarray, final: bool = False) -> None:
                nonlocal ttfa, sent, first_chunk
                if y.size == 0 or job.cancel.is_set():
                    return
                if first_chunk:
                    y = fade(y, fade_in=144)
                    first_chunk = False
                if final:
                    y = fade(y, fade_out=240)
                pcm = to_int16(y)
                for i in range(0, pcm.size, 4800):
                    if job.cancel.is_set():
                        return
                    self.conn.send_pcm(job.id, pcm[i:i + 4800])
                if ttfa is None:
                    ttfa = (time.perf_counter() - t0) * 1000.0
                    self.emit("job_first_audio", id=job.id, ttfa_ms=round(ttfa, 1))
                sent += pcm.size

            for idx, seg in enumerate(segments):
                if job.cancel.is_set():
                    break
                req = SynthRequest(text=seg, lang=lang, voice=voice, speed=job.speed,
                                   instruct=job.instruct, cancel=job.cancel)
                for chunk, sr in eng.stream(req):
                    if job.cancel.is_set():
                        break
                    if resampler is None or resampler.in_sr != sr:
                        if resampler is not None:
                            send(leveler.process(stretch.process(resampler.flush())))
                        resampler = StreamResampler(sr, SAMPLE_RATE)
                    send(leveler.process(stretch.process(resampler.process(chunk))))
                if idx + 1 < len(segments) and resampler is not None and not job.cancel.is_set():
                    pause = np.zeros(int(resampler.in_sr * 0.11), dtype=np.float32)
                    send(leveler.process(stretch.process(resampler.process(pause))))

            if resampler is not None and not job.cancel.is_set():
                tail = leveler.process(stretch.process(resampler.flush()))
                tail = np.concatenate([tail, leveler.process(stretch.flush()), leveler.flush()])
                send(tail, final=True)

            gen_ms = (time.perf_counter() - t0) * 1000.0
            audio_ms = sent / SAMPLE_RATE * 1000.0
            self.emit("job_end", id=job.id, cancelled=job.cancel.is_set(), audio_ms=round(audio_ms),
                      gen_ms=round(gen_ms), ttfa_ms=round(ttfa or 0.0, 1),
                      rtf=round(audio_ms / gen_ms, 2) if gen_ms > 0 else 0)
        finally:
            with self.lock:
                if self.current is job:
                    self.current = None

    # ---- voice library ------------------------------------------------------------------
    def op_voice_from_file(self, op: dict) -> None:
        import soundfile as sf
        self._require_qwen()
        req = op.get("req")
        path = str(op.get("path", ""))
        name = str(op.get("name", "")).strip() or Path(path).stem
        ref_text = str(op.get("ref_text", "")).strip()
        lang = str(op.get("lang", "it"))
        self.status("busy", "Analisi del file audio…")
        try:
            wav, sr = sf.read(path, dtype="float32", always_2d=False)
        except Exception:
            import librosa
            wav, sr = librosa.load(path, sr=None, mono=True)
        wav = np.asarray(wav, dtype=np.float32)
        if wav.ndim > 1:
            wav = wav.mean(axis=1)
        entry = self.store.add(name=name, audio=wav, sr=int(sr), ref_text=ref_text, lang=lang, source="file")
        self._finish_voice(entry, req)

    def op_voice_design_preview(self, op: dict) -> None:
        self._require_qwen()
        req = str(op.get("req", ""))
        instruct = str(op.get("instruct", "")).strip()
        text = str(op.get("text", "")).strip()
        lang = str(op.get("lang", "it"))
        if not instruct or not text:
            raise ValueError("descrizione e frase di prova sono obbligatorie")
        eng = self._engine("qwen")
        audio, sr = eng.design(text, instruct, lang, self.status)
        from .audio import Leveler, StreamResampler, fade, to_int16, trim_silence
        audio = trim_silence(audio, sr, pad_ms=80.0)
        self.pending_design = {req: {"audio": audio, "sr": sr, "text": text, "instruct": instruct, "lang": lang}}
        job_id = int(op.get("id", 0)) & 0xFFFFFFFF
        rs = StreamResampler(sr, SAMPLE_RATE)
        y = np.concatenate([rs.process(audio), rs.flush()])
        y = Leveler().process(y)
        pcm = to_int16(fade(y, fade_in=144, fade_out=240))
        self.emit("job_start", id=job_id, text=text, segments=1, lang=lang)
        for i in range(0, pcm.size, 4800):
            self.conn.send_pcm(job_id, pcm[i:i + 4800])
        self.emit("job_end", id=job_id, cancelled=False, audio_ms=round(pcm.size / 48.0), gen_ms=0)
        self.emit("design_ready", req=req, seconds=round(audio.size / sr, 2))
        self.status("ready", "Anteprima pronta: salvala se ti piace.")

    def op_voice_design_save(self, op: dict) -> None:
        self._require_qwen()
        req = str(op.get("req", ""))
        pending = self.pending_design.get(req)
        if pending is None:
            raise ValueError("nessuna anteprima da salvare: generane una")
        name = str(op.get("name", "")).strip() or "Voce progettata"
        entry = self.store.add(name=name, audio=pending["audio"], sr=pending["sr"], ref_text=pending["text"],
                               lang=pending["lang"], source="design", instruct=pending["instruct"])
        self.pending_design.clear()
        self._finish_voice(entry, req)

    def _finish_voice(self, entry: dict, req) -> None:
        eng = self._engine("qwen")
        ok, _why = eng.available()
        if ok:
            eng.create_voice_prompt(entry["id"], self.status)
        self.emit("voice_created", req=req, voice={"id": f"clone:{entry['id']}", "name": entry["name"]})
        self._emit_voices()
        self.status("ready", f"Voce “{entry['name']}” pronta")

    def op_voice_delete(self, op: dict) -> None:
        vid = str(op.get("voice", ""))
        if vid.startswith("clone:"):
            self.store.delete(vid[6:])
            eng = self.engines.get("qwen")
            if eng is not None:
                eng._prompts = {k: v for k, v in eng._prompts.items() if k[0] != vid[6:]}
        self._emit_voices()

    def op_voice_rename(self, op: dict) -> None:
        vid = str(op.get("voice", ""))
        if vid.startswith("clone:"):
            self.store.rename(vid[6:], str(op.get("name", "")))
        self._emit_voices()


# ---------------------------------------------------------------------------
def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(prog="gbtts.server")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--token", required=True)
    ap.add_argument("--home", required=True)
    ap.add_argument("--parent-pid", type=int, default=0)
    ap.add_argument("--log-level", default="INFO")
    args = ap.parse_args(argv)

    logging.basicConfig(level=getattr(logging, args.log_level.upper(), logging.INFO), stream=sys.stderr,
                        format="%(asctime)s %(levelname)-7s %(name)s: %(message)s")
    home = Path(args.home).resolve()
    models = home / "models"
    models.mkdir(parents=True, exist_ok=True)
    os.environ["HF_HOME"] = str(models / "hf")
    os.environ["SUPERTONIC_CACHE_DIR"] = str(models / "supertonic3")
    os.environ.setdefault("HF_HUB_DISABLE_TELEMETRY", "1")
    os.environ.setdefault("TOKENIZERS_PARALLELISM", "false")

    from .netfix import use_system_certificates
    use_system_certificates()

    threading.Thread(target=_watch_parent, args=(args.parent_pid,), name="gbtts-parent", daemon=True).start()

    conn = None
    for _attempt in range(50):
        try:
            conn = FrameConnection.connect(args.port)
            break
        except OSError:
            time.sleep(0.1)
    if conn is None:
        log.error("cannot connect to plugin on port %d", args.port)
        return 2

    conn.send_json({"ev": "hello", "token": args.token, "version": __version__,
                    "protocol": PROTOCOL_VERSION, "pid": os.getpid()})
    backend = Backend(conn, home)
    backend.worker.start()
    backend.status("starting", "Backend avviato")
    log.info("gbtts %s connected (home=%s)", __version__, home)

    try:
        while True:
            ftype, payload = conn.recv_frame()
            if ftype == T_JSON:
                try:
                    op = json.loads(payload.decode("utf-8"))
                except ValueError:
                    continue
                if isinstance(op, dict):
                    backend.on_message(op)
            elif ftype == T_MIC:
                backend.on_mic(payload)
    except ConnectionClosed:
        log.info("plugin disconnected")
    except Exception as exc:  # noqa: BLE001
        log.error("reader crashed: %s\n%s", exc, traceback.format_exc())
    os._exit(0)


if __name__ == "__main__":
    raise SystemExit(main())
