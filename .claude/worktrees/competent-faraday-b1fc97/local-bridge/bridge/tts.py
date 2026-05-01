from __future__ import annotations

import base64
import json
from dataclasses import dataclass
from typing import Protocol
from urllib import request
from urllib.error import HTTPError, URLError


class TtsError(RuntimeError):
    """Raised when TTS generation fails."""


class TtsProvider(Protocol):
    def synthesize(self, text: str) -> list[bytes]:
        ...


@dataclass
class NullTtsProvider:
    def synthesize(self, text: str) -> list[bytes]:
        return []


@dataclass
class HttpFrameTtsProvider:
    """
    Generic HTTP TTS provider that returns Opus frames.

    Request:
      {
        "text": "...",
        "audio_format": "opus"
      }
    Response:
      {
        "frames_base64": ["..."]
      }
    """

    url: str
    api_key: str = ""
    timeout_s: float = 30.0

    def synthesize(self, text: str) -> list[bytes]:
        if not self.url:
            raise TtsError("TTS_HTTP_URL is required for TTS_PROVIDER=http")
        payload = {"text": text, "audio_format": "opus"}
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
            raise TtsError(f"TTS HTTP request failed: {exc}") from exc
        frames = data.get("frames_base64")
        if not isinstance(frames, list):
            raise TtsError("TTS response missing 'frames_base64' list")
        out: list[bytes] = []
        for frame in frames:
            if not isinstance(frame, str):
                raise TtsError("Invalid frame value in 'frames_base64'")
            out.append(base64.b64decode(frame.encode("ascii")))
        return out

