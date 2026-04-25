#!/usr/bin/env python3
"""
Local STT HTTP server matching the local-bridge STT contract:

  POST { "audio_format": "opus", "frames_base64": ["..."] }
  -> { "text": "..." }

Requires: libopus (e.g. macOS: brew install opus), `pip install` deps from
requirements-stt.txt (minimal) and optionally requirements-stt-local.txt.

Transcription engines (see STT_ENGINE):
  - local: faster-whisper (Python 3.10–3.12 typical; needs onnx wheels for your version)
  - openai: OpenAI /audio/transcriptions (no heavy ML deps; set OPENAI_API_KEY)

Opus: 24 kHz mono, 60 ms frames (see main/audio on device).
"""
from __future__ import annotations

import base64
import io
import json
import logging
import os
import sys
import wave
from typing import Any, Literal

import aiohttp
import numpy as np
from aiohttp import web
from dotenv import load_dotenv

load_dotenv()

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] stt: %(message)s",
)
log = logging.getLogger("stt_server")

# Match ESP-Box audio: config.h + OPUS_FRAME_DURATION_MS
SAMPLE_RATE = int(os.environ.get("STT_OPUS_SAMPLE_RATE", "24000"))
FRAME_MS = int(os.environ.get("STT_OPUS_FRAME_MS", "60"))
MAX_FRAME_SAMPLES = SAMPLE_RATE * FRAME_MS // 1000
_MIN_PCM_BYTES = int(SAMPLE_RATE * 0.05) * 2

WHISPER_MODEL = os.environ.get("WHISPER_MODEL", "base")
WHISPER_DEVICE = os.environ.get("WHISPER_DEVICE", "auto")
WHISPER_COMPUTE_TYPE = os.environ.get("WHISPER_COMPUTE_TYPE", "int8")
WHISPER_LANGUAGE = os.environ.get("WHISPER_LANGUAGE", "").strip() or None

STT_LISTEN_HOST = os.environ.get("STT_LISTEN_HOST", "0.0.0.0")
STT_LISTEN_PORT = int(os.environ.get("STT_LISTEN_PORT", "9001"))
STT_HTTP_API_KEY = os.environ.get("STT_HTTP_API_KEY", "").strip().strip('"').strip("'")

OPENAI_API_KEY = os.environ.get("OPENAI_API_KEY", "").strip().strip('"').strip("'")
OPENAI_STT_BASE = os.environ.get(
    "OPENAI_STT_BASE_URL", "https://api.openai.com/v1/audio/transcriptions"
).rstrip("/")

Engine = Literal["local", "openai"]


def _import_opus():
    try:
        from opuslib import Decoder  # type: ignore[import-untyped]
    except ImportError as e:
        log.error(
            "opuslib is required. Install: pip install opuslib\n"
            "  macOS: brew install pkg-config opus\n"
            "  Linux: apt install libopus0 libopus-dev (or your distro's opus dev package)"
        )
        raise e
    return Decoder


def _faster_import():
    from faster_whisper import WhisperModel

    return WhisperModel


def _choose_engine() -> Engine:
    raw = os.environ.get("STT_ENGINE", "auto").strip().lower()
    if raw in ("openai", "whisper_api", "api"):
        return "openai"
    if raw in ("local", "faster", "faster_whisper"):
        return "local"
    if raw != "auto":
        log.error("STT_ENGINE must be auto, local, or openai; got %r", raw)
        sys.exit(1)

    try:
        import faster_whisper  # noqa: F401
    except ImportError:
        if OPENAI_API_KEY:
            log.info("faster_whisper not installed; using OpenAI transcriptions (STT_ENGINE=openai).")
            return "openai"
        log.error(
            "No STT backend available. Either:\n"
            "  • pip install -r requirements-stt-local.txt (local Whisper; use Python 3.10–3.12 for wheels), or\n"
            "  • Set OPENAI_API_KEY and we will use the cloud transcriptions API (STT_ENGINE=openai)."
        )
        sys.exit(1)
    if OPENAI_API_KEY and os.environ.get("STT_PREFER_OPENAI", "").lower() in ("1", "true", "yes"):
        log.info("STT_PREFER_OPENAI=1: using OpenAI transcriptions despite local model availability.")
        return "openai"
    return "local"


def pcm_s16le_to_f32_16k(pcm: bytes) -> np.ndarray:
    if not pcm:
        return np.array([], dtype=np.float32)
    x = np.frombuffer(pcm, dtype=np.int16).astype(np.float32) / 32768.0
    if SAMPLE_RATE == 16000:
        return x.astype(np.float32)
    n_out = int(round(len(x) * 16000.0 / float(SAMPLE_RATE)))
    if n_out < 1:
        return np.array([], dtype=np.float32)
    t_old = np.arange(len(x), dtype=np.float64) / float(SAMPLE_RATE)
    t_new = np.arange(n_out, dtype=np.float64) / 16000.0
    y = np.interp(t_new, t_old, x)
    return y.astype(np.float32)


def pcm_s16_24k_to_wav_bytes_16k(pcm: bytes) -> bytes:
    """WAV 16 kHz mono s16le for OpenAI and other API consumers."""
    audio = pcm_s16le_to_f32_16k(pcm)
    if audio.size < 1:
        return b""
    pcm16 = (np.clip(audio, -1.0, 1.0) * 32767.0).astype(np.int16)
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(16000)
        w.writeframes(pcm16.tobytes())
    return buf.getvalue()


def decode_opus_frames(frames: list[bytes], decoder_cls: type) -> bytes:
    dec = decoder_cls(SAMPLE_RATE, 1)
    out = bytearray()
    for i, frame in enumerate(frames):
        if not frame:
            continue
        try:
            pcm = dec.decode(frame, MAX_FRAME_SAMPLES)
        except Exception as e:
            log.warning("Opus frame %d decode failed: %s", i, e)
            continue
        if pcm:
            out.extend(pcm)
    return bytes(out)


def build_app(
    whisper_model: object | None,
    decoder_cls: type,
    engine: Engine,
) -> web.Application:
    app = web.Application()
    app["whisper"] = whisper_model
    app["decoder_cls"] = decoder_cls
    app["engine"] = engine

    async def on_startup(a: web.Application) -> None:
        if a["engine"] == "openai":
            a["http"] = aiohttp.ClientSession(
                timeout=aiohttp.ClientTimeout(total=float(os.environ.get("OPENAI_STT_TIMEOUT_S", "60")))
            )

    async def on_cleanup(a: web.Application) -> None:
        sess = a.pop("http", None)
        if sess is not None:
            await sess.close()

    app.on_startup.append(on_startup)
    app.on_cleanup.append(on_cleanup)

    async def health(_: web.Request) -> web.Response:
        h: dict[str, Any] = {
            "ok": True,
            "service": "stt",
            "engine": app["engine"],
            "opus_sample_rate": SAMPLE_RATE,
        }
        if app["engine"] == "local" and app["whisper"] is not None:
            h["whisper_model"] = WHISPER_MODEL
        if app["engine"] == "openai":
            h["openai_stt"] = bool(OPENAI_API_KEY)
        return web.json_response(h)

    async def transcribe_openai(
        request: web.Request, pcm: bytes, n_frames: int
    ) -> web.Response:
        if not OPENAI_API_KEY:
            log.error("OPENAI_API_KEY is not set")
            return web.json_response({"text": ""}, status=500)
        wav = pcm_s16_24k_to_wav_bytes_16k(pcm)
        if not wav:
            return web.json_response({"text": ""})
        session: aiohttp.ClientSession = request.app["http"]
        form = aiohttp.FormData()
        form.add_field("file", wav, filename="audio.wav", content_type="audio/wav")
        form.add_field("model", os.environ.get("OPENAI_STT_MODEL", "whisper-1"))
        if WHISPER_LANGUAGE:
            form.add_field("language", WHISPER_LANGUAGE)
        try:
            async with session.post(
                OPENAI_STT_BASE,
                data=form,
                headers={"Authorization": f"Bearer {OPENAI_API_KEY}"},
            ) as resp:
                body = await resp.text()
                if resp.status != 200:
                    log.warning("OpenAI STT %s: %s", resp.status, body[:500])
                    return web.json_response({"text": ""})
        except aiohttp.ClientError as e:
            log.warning("OpenAI STT request failed: %s", e)
            return web.json_response({"text": ""})
        data = json.loads(body)
        text = (data.get("text") or "").strip() if isinstance(data, dict) else ""
        log.info("openai: %d frames -> %r", n_frames, text[:120] if text else text)
        return web.json_response({"text": text})

    async def transcribe_local(
        request: web.Request, pcm: bytes, n_frames: int
    ) -> web.Response:
        model = request.app["whisper"]
        if model is None:
            return web.json_response({"text": ""}, status=500)
        audio = pcm_s16le_to_f32_16k(pcm)
        if audio.size < 100:
            return web.json_response({"text": ""})
        segs, _info = model.transcribe(
            audio,
            language=WHISPER_LANGUAGE,
            beam_size=5,
            task="transcribe",
        )
        text = "".join(s.text for s in segs).strip()
        log.info("local: %d frames -> %r", n_frames, text[:120] if text else text)
        return web.json_response({"text": text})

    async def transcribe(request: web.Request) -> web.Response:
        if STT_HTTP_API_KEY:
            auth = request.headers.get("Authorization", "")
            if auth != f"Bearer {STT_HTTP_API_KEY}":
                return web.json_response({"text": ""}, status=401)

        try:
            body: dict[str, Any] = await request.json()
        except Exception as e:
            log.warning("Invalid JSON: %s", e)
            return web.json_response({"text": ""}, status=400)

        raw_b64 = body.get("frames_base64", [])
        if not isinstance(raw_b64, list) or not raw_b64:
            return web.json_response({"text": ""})

        frames: list[bytes] = []
        for i, s in enumerate(raw_b64):
            if not isinstance(s, str):
                continue
            try:
                frames.append(base64.b64decode(s, validate=True))
            except Exception as e:
                log.warning("bad base64 at %d: %s", i, e)

        pcm = decode_opus_frames(frames, request.app["decoder_cls"])
        if not pcm or len(pcm) < _MIN_PCM_BYTES:
            log.info("No usable PCM after Opus decode (%d frames in)", len(frames))
            return web.json_response({"text": ""})

        eng: Engine = request.app["engine"]
        if eng == "openai":
            return await transcribe_openai(request, pcm, len(frames))
        return await transcribe_local(request, pcm, len(frames))

    app.router.add_get("/healthz", health)
    app.router.add_post("/transcribe", transcribe)
    return app


def main() -> None:
    engine = _choose_engine()
    if engine == "openai" and not OPENAI_API_KEY:
        log.error("STT_ENGINE=openai (or auto fallback) requires OPENAI_API_KEY in the environment.")
        sys.exit(1)

    decoder_cls = _import_opus()
    model = None
    if engine == "local":
        WhisperModel = _faster_import()
        log.info(
            "Loading Whisper model=%r device=%r compute_type=%r (first run may download weights)",
            WHISPER_MODEL,
            WHISPER_DEVICE,
            WHISPER_COMPUTE_TYPE,
        )
        try:
            model = WhisperModel(
                WHISPER_MODEL,
                device=WHISPER_DEVICE,
                compute_type=WHISPER_COMPUTE_TYPE,
            )
        except Exception as e:
            log.error("Failed to load faster-whisper: %s", e)
            if OPENAI_API_KEY:
                log.info("Falling back to OpenAI transcriptions. Set STT_ENGINE=openai to skip local load.")
                engine = "openai"
                model = None
            else:
                sys.exit(1)
    else:
        log.info("STT using OpenAI transcriptions at %s", OPENAI_STT_BASE)

    app = build_app(model, decoder_cls, engine)
    log.info(
        "STT server engine=%r listening on http://%s:%s POST /transcribe (Opus %d Hz, %d ms frames)",
        engine,
        STT_LISTEN_HOST,
        STT_LISTEN_PORT,
        SAMPLE_RATE,
        FRAME_MS,
    )
    web.run_app(app, host=STT_LISTEN_HOST, port=STT_LISTEN_PORT, print=None)


if __name__ == "__main__":
    main()
