from __future__ import annotations

import base64
import json
from dataclasses import dataclass
from typing import Protocol
from urllib import request
from urllib.error import HTTPError, URLError


class SttError(RuntimeError):
    """Raised when STT transcription fails."""


class SttProvider(Protocol):
    def transcribe(self, opus_frames: list[bytes]) -> str:
        ...


@dataclass
class NullSttProvider:
    def transcribe(self, opus_frames: list[bytes]) -> str:
        return ""


@dataclass
class HttpFrameSttProvider:
    """
    Generic HTTP STT provider that accepts base64-encoded Opus frames.

    Request:
      {
        "audio_format": "opus",
        "frames_base64": ["..."]
      }
    Response:
      {
        "text": "transcribed utterance"
      }
    """

    url: str
    api_key: str = ""
    timeout_s: float = 30.0

    def transcribe(self, opus_frames: list[bytes]) -> str:
        if not self.url:
            raise SttError("STT_HTTP_URL is required for STT_PROVIDER=http")
        payload = {
            "audio_format": "opus",
            "frames_base64": [base64.b64encode(frame).decode("ascii") for frame in opus_frames],
        }
        headers = {"Content-Type": "application/json"}
        if self.api_key:
            headers["Authorization"] = f"Bearer {self.api_key}"
        req = request.Request(
            url=self.url,
            data=json.dumps(payload).encode("utf-8"),
            headers=headers,
            method="POST",
        )
        try:
            with request.urlopen(req, timeout=self.timeout_s) as resp:
                data = json.loads(resp.read().decode("utf-8"))
        except (HTTPError, URLError, TimeoutError) as exc:
            raise SttError(f"STT HTTP request failed: {exc}") from exc
        text = data.get("text")
        if not isinstance(text, str):
            raise SttError("STT response missing 'text' field")
        return text.strip()

