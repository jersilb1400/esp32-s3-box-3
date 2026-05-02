# Speaker verification toolkit (jarvis-server side)

Companion module for **`fw-jarvis`** BOX-3 builds using:

- **`CONFIG_SEND_WAKE_WORD_DATA=y`**
- optional **`CONFIG_JARVIS_SPEAKER_VERIFY_HELLO=y`** (`features.speaker_verify=true` in websocket `hello`)

## What it provides

| File | Purpose |
|------|---------|
| `speaker_verify.py` | Stores per-device centroid embeddings (`~/.cache/jarvis-speaker/embeddings/*.json`). |
| `requirements-full.txt` | Optional heavy deps (Resemblyzer + torch wheels). |

`SpeakerVerificationService` exposes:

```python
from speaker_verify import SpeakerVerificationService

svc = SpeakerVerificationService()
svc.enroll_wake_audio(device_id, pcm_bytes)
accepted, score = svc.verify_wake_audio(device_id, pcm_bytes, threshold=0.78)
```

`pcm_bytes` must be little-endian PCM16 mono. Decode the burst of Opus frames that arrives immediately after `listen/state=detect` on your bridge.

## Recommended integration flow

1. **Provision / enroll** — after the owner finishes pairing and says “Jarvis” three consecutive times successfully, concatenate the PCM from those wake bursts, average embeddings, persist via `SpeakerProfileStore.save`.
2. **Runtime verify** — on every subsequent wake burst, cosine-check against centroid; reject (drop websocket / send `session` sleep) if similarity < configured threshold (~0.75–0.85 depending on frontend noise).
3. **Swap embedding backend** — start from `SimpleSpectralEmbedder` in `speaker_verify.py` and swap in SpeechBrain ECAPA, NeMo SpeakNet ONNX, Resemblyzer (see `requirements-full.txt`), or a managed API (Azure speaker verification, etc.).

Because `jarvis-server` is not bundled with this firmware repository, vendor this folder into whichever Python service terminates the BOX-3 websocket and extend your STT/LLM gating accordingly.
