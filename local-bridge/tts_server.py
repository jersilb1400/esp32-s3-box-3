#!/usr/bin/env python3
"""
Local TTS HTTP server matching the local-bridge TTS contract:

  POST { "text": "...", "audio_format": "opus" }
  -> { "frames_base64": ["...", ...] }

Pipeline: OpenAI /v1/audio/speech (PCM 24kHz s16le mono) -> Opus 60ms frames
(same as ESP-Box: OPUS_FRAME_DURATION_MS=60, AUDIO_*_SAMPLE_RATE=24000).

Requires: libopus, OPENAI_API_KEY, pip: aiohttp opuslib (see requirements-tts.txt).
"""
from __future__ import annotations

import base64
import json
import logging
import os
import sys
from typing import Any

import aiohttp
from aiohttp import web
from dotenv import load_dotenv
from opuslib import APPLICATION_AUDIO, Encoder  # type: ignore[import-untyped]

load_dotenv()

logging.basicConfig(
    level=logging.INFO,
    format="%(asctime)s [%(levelname)s] tts: %(message)s",
)
log = logging.getLogger("tts_server")

SAMPLE_RATE = 24000
FRAME_MS = 60
SAMPLES_PER_FRAME = SAMPLE_RATE * FRAME_MS // 1000  # 1440 for 24k/60ms
PCM_BYTES_PER_FRAME = SAMPLES_PER_FRAME * 2

OPENAI_API_KEY = os.environ.get("OPENAI_API_KEY", "").strip().strip('"').strip("'")
OPENAI_TTS_BASE = os.environ.get("OPENAI_TTS_BASE_URL", "https://api.openai.com/v1/audio/speech").rstrip(
    "/"
)
OPENAI_TTS_MODEL = os.environ.get("OPENAI_TTS_MODEL", "tts-1")
OPENAI_TTS_VOICE = os.environ.get("OPENAI_TTS_VOICE", "alloy")

TTS_LISTEN_HOST = os.environ.get("TTS_LISTEN_HOST", "0.0.0.0")
TTS_LISTEN_PORT = int(os.environ.get("TTS_LISTEN_PORT", "9002"))
# Optional: require same Bearer as the bridge (TTS_HTTP_API_KEY in .env)
TTS_HTTP_API_KEY = os.environ.get("TTS_HTTP_API_KEY", "").strip().strip('"').strip("'")


def pcm_s16le_to_opus_frames(pcm: bytes) -> list[bytes]:
    if len(pcm) % 2:
        pcm = pcm[:-1]
    if not pcm:
        return []
    enc = Encoder(SAMPLE_RATE, 1, APPLICATION_AUDIO)
    frames: list[bytes] = []
    for off in range(0, len(pcm), PCM_BYTES_PER_FRAME):
        chunk = pcm[off : off + PCM_BYTES_PER_FRAME]
        if len(chunk) < PCM_BYTES_PER_FRAME:
            chunk = chunk + b"\x00" * (PCM_BYTES_PER_FRAME - len(chunk))
        packet = enc.encode(chunk, frame_size=SAMPLES_PER_FRAME)
        if packet:
            frames.append(packet)
    return frames


async def openai_tts_pcm(session: aiohttp.ClientSession, text: str) -> bytes:
    payload = {
        "model": OPENAI_TTS_MODEL,
        "input": text[:4095],
        "voice": OPENAI_TTS_VOICE,
        "response_format": "pcm",
    }
    async with session.post(
        OPENAI_TTS_BASE,
        json=payload,
        headers={"Authorization": f"Bearer {OPENAI_API_KEY}"},
    ) as resp:
        if resp.status != 200:
            body = await resp.text()
            log.warning("OpenAI TTS %s: %s", resp.status, body[:400])
            raise RuntimeError(f"openai tts: {body[:300]}")
        return await resp.read()


def build_app() -> web.Application:
    app = web.Application()

    async def on_startup(a: web.Application) -> None:
        a["http"] = aiohttp.ClientSession(
            timeout=aiohttp.ClientTimeout(total=float(os.environ.get("OPENAI_TTS_TIMEOUT_S", "60")))
        )

    async def on_cleanup(a: web.Application) -> None:
        sess = a.pop("http", None)
        if sess is not None:
            await sess.close()

    app.on_startup.append(on_startup)
    app.on_cleanup.append(on_cleanup)

    async def health(_: web.Request) -> web.Response:
        return web.json_response(
            {
                "ok": True,
                "service": "tts",
                "sample_rate_hz": SAMPLE_RATE,
                "frame_ms": FRAME_MS,
                "openai_configured": bool(OPENAI_API_KEY),
            }
        )

    async def synthesize(request: web.Request) -> web.Response:
        if TTS_HTTP_API_KEY:
            auth = request.headers.get("Authorization", "")
            if auth != f"Bearer {TTS_HTTP_API_KEY}":
                return web.json_response({"error": "unauthorized", "frames_base64": []}, status=401)

        try:
            data: dict[str, Any] = await request.json()
        except Exception as e:
            log.warning("invalid json: %s", e)
            return web.json_response({"frames_base64": []}, status=400)

        if data.get("audio_format", "opus") != "opus":
            return web.json_response(
                {"error": "only audio_format=opus is supported", "frames_base64": []},
                status=400,
            )

        text = str(data.get("text", "")).strip()
        if not text:
            return web.json_response({"frames_base64": []})

        if not OPENAI_API_KEY:
            return web.json_response(
                {"error": "OPENAI_API_KEY is not set; TTS needs OpenAI speech. See tts_server.py.", "frames_base64": []},
                status=503,
            )

        session: aiohttp.ClientSession = request.app["http"]
        try:
            pcm = await openai_tts_pcm(session, text)
        except RuntimeError as e:
            return web.json_response(
                {"error": "openai_tts_failed", "detail": str(e), "frames_base64": []},
                status=502,
            )
        try:
            frames = pcm_s16le_to_opus_frames(pcm)
        except Exception:
            log.exception("Opus encode failed")
            return web.json_response({"error": "opus_encode_failed", "frames_base64": []}, status=500)

        b64 = [base64.b64encode(f).decode("ascii") for f in frames]
        log.info("TTS: %d chars -> %d opus frames", len(text), len(frames))
        return web.json_response({"frames_base64": b64})

    app.router.add_get("/healthz", health)
    app.router.add_post("/synthesize", synthesize)
    return app


def main() -> None:
    if not OPENAI_API_KEY:
        log.warning("OPENAI_API_KEY is not set; /synthesize will return errors until you set it.")
    app = build_app()
    log.info(
        "TTS server http://%s:%s POST /synthesize (OpenAI->PCM 24k -> Opus %dms) voice=%s",
        TTS_LISTEN_HOST,
        TTS_LISTEN_PORT,
        FRAME_MS,
        OPENAI_TTS_VOICE,
    )
    web.run_app(app, host=TTS_LISTEN_HOST, port=TTS_LISTEN_PORT, print=None)


if __name__ == "__main__":
    main()
