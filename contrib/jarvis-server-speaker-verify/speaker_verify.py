from __future__ import annotations

import json
import math
import pathlib
from array import array
from typing import Optional, Sequence, Tuple


Vector = list[float]


def cosine_similarity(a: Sequence[float], b: Sequence[float]) -> float:
    if len(a) != len(b) or len(a) == 0:
        raise ValueError("embedding length mismatch")
    dot = sum(x * y for x, y in zip(a, b))
    na = math.sqrt(sum(x * x for x in a))
    nb = math.sqrt(sum(y * y for y in b))
    if na == 0 or nb == 0:
        return 0.0
    return dot / (na * nb)


class SimpleSpectralEmbedder:
    """Very lightweight deterministic embedding for demos.

    Replace this with SpeechBrain ECAPA-TDNN, NeMo SpeakNet, Azure speaker ID, etc.
    """

    def __init__(self, frame_pts: int = 512, hop_pts: int = 256):
        self.frame_pts = frame_pts
        self.hop_pts = hop_pts

    def embed_pcm16_mono(self, pcm_bytes: bytes, sample_rate: int = 16000) -> Vector:
        samples = array("h")
        samples.frombytes(pcm_bytes)
        floats = [s / 32768.0 for s in samples]
        if len(floats) < self.frame_pts:
            raise ValueError("need at least {:.0f} ms of audio".format(1000 * self.frame_pts / sample_rate))
        feats: list[float] = []
        for start in range(0, len(floats) - self.frame_pts, self.hop_pts):
            frame = floats[start : start + self.frame_pts]
            window = [
                samp * math.sin(math.pi * i / (len(frame) - 1)) if len(frame) > 1 else samp
                for i, samp in enumerate(frame)
            ]
            fft_bins = []
            chunk = window
            n = len(chunk)
            for k in range(32):
                real = sum(
                    chunk[idx] * math.cos(2 * math.pi * k * idx / n) for idx in range(n)
                )
                imag = sum(
                    -chunk[idx] * math.sin(2 * math.pi * k * idx / n) for idx in range(n)
                )
                fft_bins.append(math.sqrt(real * real + imag * imag))
            feats.extend(fft_bins)
        norm = math.sqrt(sum(x * x for x in feats))
        if norm == 0:
            raise ValueError("silent audio")
        return [x / norm for x in feats]


class SpeakerProfileStore:
    def __init__(self, root: pathlib.Path | None = None):
        self.root = root or pathlib.Path.home() / ".cache" / "jarvis-speaker" / "embeddings"
        self.root.mkdir(parents=True, exist_ok=True)

    def _path_for(self, device_id: str) -> pathlib.Path:
        safe = device_id.replace("/", "_")
        return self.root / f"{safe}.json"

    def load(self, device_id: str) -> Optional[dict]:
        path = self._path_for(device_id)
        if not path.exists():
            return None
        return json.loads(path.read_text(encoding="utf-8"))

    def save_centroid(self, device_id: str, embedding: Sequence[float]) -> None:
        payload = {
            "embedding": list(map(float, embedding)),
            "meta": {"norm": math.sqrt(sum(x * x for x in embedding)), "dims": len(embedding)},
        }
        self._path_for(device_id).write_text(json.dumps(payload), encoding="utf-8")


class SpeakerVerificationService:
    def __init__(self, backend: SimpleSpectralEmbedder | None = None, store: SpeakerProfileStore | None = None):
        self.backend = backend or SimpleSpectralEmbedder()
        self.store = store or SpeakerProfileStore()

    def enroll_wake_audio(self, device_id: str, pcm_bytes: bytes, sample_rate: int = 16000) -> Vector:
        vector = self.backend.embed_pcm16_mono(pcm_bytes, sample_rate=sample_rate)
        self.store.save_centroid(device_id, vector)
        return list(vector)

    def verify_wake_audio(
        self, device_id: str, pcm_bytes: bytes, threshold: float, sample_rate: int = 16000
    ) -> Tuple[bool, float]:
        profile = self.store.load(device_id)
        if profile is None:
            raise LookupError(f"no centroid for device {device_id}")

        centroid = profile["embedding"]
        candidate = self.backend.embed_pcm16_mono(pcm_bytes, sample_rate=sample_rate)
        score = cosine_similarity(candidate, centroid)
        return score >= threshold, score
