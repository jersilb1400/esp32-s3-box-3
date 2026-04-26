"""
In-process STT + TTS via OpenAI (no separate stt_server / tts_server).

- ST: decode device Opus -> PCM, wrap WAV, POST /v1/audio/transcriptions
- TTS: POST /v1/audio/speech (pcm) -> Opus 60ms frames (same as tts_server.py)
"""
from __future__ import annotations

import io
import json
import os
import wave
from dataclasses import dataclass
from urllib import request
from urllib.error import HTTPError, URLError

from opuslib import APPLICATION_AUDIO, Encoder  # type: ignore[import-untyped]

from .stt import SttError
from .tts import TtsError

# TTS: OpenAI pcm is 24 kHz; Opus to device must match server hello (BRIDGE_SERVER_SAMPLE_RATE, default 24000).
# STT: uplink Opus from the device is 16 kHz (see audio_service.cc encoder_sample_rate_).
SAMPLE_RATE = int(os.environ.get("TTS_OPUS_SAMPLE_RATE", os.environ.get("STT_OPUS_SAMPLE_RATE", "24000")))
UPLINK_OPUS_SAMPLE_RATE = int(os.environ.get("STT_UPLINK_OPUS_SAMPLE_RATE", "24000"))
FRAME_MS = int(os.environ.get("STT_OPUS_FRAME_MS", "60"))
SAMPLES_PER_FRAME = SAMPLE_RATE * FRAME_MS // 1000
PCM_BYTES_PER_FRAME = SAMPLES_PER_FRAME * 2

OPENAI_STT_URL = os.environ.get(
    "OPENAI_STT_BASE_URL", "https://api.openai.com/v1/audio/transcriptions"
).rstrip("/")
OPENAI_TTS_URL = os.environ.get(
    "OPENAI_TTS_BASE_URL", "https://api.openai.com/v1/audio/speech"
).rstrip("/")
OPENAI_STT_MODEL = os.environ.get("OPENAI_STT_MODEL", "whisper-1")
OPENAI_TTS_MODEL = os.environ.get("OPENAI_TTS_MODEL", "tts-1")
OPENAI_TTS_VOICE = os.environ.get("OPENAI_TTS_VOICE", "alloy")


def _import_opus_decoder():
    from opuslib import Decoder  # type: ignore[import-untyped]

    return Decoder


def _frame_samples_for_rate(sample_rate: int) -> int:
    return sample_rate * FRAME_MS // 1000


def decode_opus_frames(frames: list[bytes], decoder_cls: type, sample_rate: int) -> bytes:
    dec = decoder_cls(sample_rate, 1)
    max_s = _frame_samples_for_rate(sample_rate)
    out = bytearray()
    for frame in frames:
        if not frame:
            continue
        try:
            pcm = dec.decode(frame, max_s)
        except Exception:
            continue
        if pcm:
            out.extend(pcm)
    return bytes(out)


def _pcm_to_wav_bytes(pcm: bytes, sample_rate: int) -> bytes:
    if not pcm:
        return b""
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(sample_rate)
        w.writeframes(pcm)
    return buf.getvalue()


def _http_error_body(exc: HTTPError) -> str:
    try:
        return exc.read().decode("utf-8", errors="replace")[:800]
    except Exception:
        return ""


def _build_multipart_wav(wav_data: bytes, boundary: str) -> bytes:
    crlf = b"\r\n"
    p0 = f'--{boundary}\r\nContent-Disposition: form-data; name="model"\r\n\r\n{OPENAI_STT_MODEL}\r\n'
    p1 = (
        f'--{boundary}\r\nContent-Disposition: form-data; name="file"; filename="a.wav"\r\n'
        f"Content-Type: audio/wav\r\n\r\n"
    )
    p2 = f"\r\n--{boundary}--\r\n"
    return p0.encode() + p1.encode() + wav_data + p2.encode()


@dataclass
class OpenAiFrameSttProvider:
    """STT: Opus frames (device) -> OpenAI transcriptions (WAV in multipart)."""

    api_key: str
    timeout_s: float = 30.0

    def transcribe(self, opus_frames: list[bytes]) -> str:
        if not self.api_key:
            raise SttError("Set OPENAI_API_KEY for STT_PROVIDER=openai")
        if not opus_frames:
            return ""
        decoder = _import_opus_decoder()
        pcm = decode_opus_frames(opus_frames, decoder, UPLINK_OPUS_SAMPLE_RATE)
        if not pcm:
            return ""
        wav = _pcm_to_wav_bytes(pcm, UPLINK_OPUS_SAMPLE_RATE)
        boundary = "----espbridgebound" + os.urandom(8).hex()
        body = _build_multipart_wav(wav, boundary)
        req = request.Request(
            url=OPENAI_STT_URL,
            data=body,
            headers={
                "Content-Type": f"multipart/form-data; boundary={boundary}",
                "Authorization": f"Bearer {self.api_key}",
            },
            method="POST",
        )
        try:
            with request.urlopen(req, timeout=self.timeout_s) as resp:
                data = json.loads(resp.read().decode("utf-8"))
        except HTTPError as exc:
            detail = _http_error_body(exc)
            raise SttError(f"OpenAI STT failed: HTTP {exc.code} {detail}") from exc
        except (URLError, TimeoutError) as exc:
            raise SttError(f"OpenAI STT failed: {exc}") from exc
        if isinstance(data, dict) and isinstance(data.get("text"), str):
            return data["text"].strip()
        return ""


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


@dataclass
class OpenAiFrameTtsProvider:
    """TTS: text -> OpenAI speech (pcm 24k) -> Opus frames for WebSocket bytes."""

    api_key: str
    timeout_s: float = 30.0

    def synthesize(self, text: str) -> list[bytes]:
        if not self.api_key:
            raise TtsError("Set OPENAI_API_KEY for TTS_PROVIDER=openai")
        t = (text or "").strip()
        if not t:
            return []

        payload = json.dumps(
            {
                "model": OPENAI_TTS_MODEL,
                "input": t[:4095],
                "voice": OPENAI_TTS_VOICE,
                "response_format": "pcm",
            }
        ).encode("utf-8")
        req = request.Request(
            url=OPENAI_TTS_URL,
            data=payload,
            headers={
                "Content-Type": "application/json",
                "Authorization": f"Bearer {self.api_key}",
            },
            method="POST",
        )
        try:
            with request.urlopen(req, timeout=self.timeout_s) as resp:
                pcm = resp.read()
        except HTTPError as exc:
            detail = _http_error_body(exc)
            raise TtsError(f"OpenAI TTS failed: HTTP {exc.code} {detail}") from exc
        except (URLError, TimeoutError) as exc:
            raise TtsError(f"OpenAI TTS failed: {exc}") from exc
        if not pcm:
            return []
        return pcm_s16le_to_opus_frames(pcm)
