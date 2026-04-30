# Phase 1 — Speaker Identification & Voice Gating Implementation Plan

> **For Claude:** REQUIRED SUB-SKILL: Use the appropriate execution skill (`executing-plans` or `subagent-driven-development`) to implement this plan.

**Goal:** Make Jarvis respond only to the enrolled owner's voice — bystanders and overheard conversations are silently ignored.
**Architecture:** Two-layer defense on the server. **Layer A** (biometric wake-word check): on every wake-triggered audio frame, the server extracts a SpeechBrain ECAPA-TDNN voice embedding and compares to the stored owner voiceprint via cosine similarity. Below threshold → discard. **Layer B** (utterance diarization): pyannote-audio segments the captured speech and only transmits owner-attributed audio to STT. New voice-driven enrollment flow ("Jarvis, enroll me") captures 5 prompted phrases and persists an averaged voiceprint encrypted on the Fly.io volume.
**Tech Stack:** Python 3.11, SpeechBrain, pyannote-audio 3.x, numpy, sqlite3, cryptography (AES-GCM), xiaozhi-esp32-server (Python).
**Assumptions:**
- Assumes the Fly.io machine has ≥1GB RAM and ≥1 CPU available for the additional model load — will NOT work if memory is exhausted (current `fly.toml` says 1GB; may need bump to 2GB; verify in Task 1).
- Assumes wake word audio is captured by the device and forwarded with the rest of the utterance — will NOT work if firmware drops the wake-word buffer before sending (verify Task 2).
- Assumes `xiaozhi-esp32-server` exposes an extension point in the receive-audio pipeline where we can insert the gate before STT — will NOT work without a code-path inspection in Task 3.
- Assumes user enrolls in a quiet environment for first setup — will NOT work if enrollment audio has SNR < 10dB (Task 9 enforces this).

---

### Task 1: Verify and bump Fly.io memory if needed

**Files:**
- Modify: `~/jarvis-server/fly.toml`

**Step 1: Inspect current**
```bash
grep -A 4 'vm\|memory' ~/jarvis-server/fly.toml
fly scale show --app jarvis-server
```

**Step 2: Bump if < 2GB**
If memory is 1024MB, change to 2048MB in `fly.toml`:
```toml
[[vm]]
  memory = '2gb'
  cpus = 1
```
Then:
```bash
cd ~/jarvis-server && fly deploy
```

**Step 3: Verify**
```bash
fly status --app jarvis-server | grep -i memory
```
Expected: 2GB shown.

**Step 4: Commit**
```bash
git add fly.toml && git commit -m "ops: bump jarvis-server to 2GB RAM for speaker ID models"
```

---

### Task 2: Verify wake-word audio buffer reaches the server

**Files (read only):** `~/esp32-s3-box-3/main/audio_service.cc`, `~/esp32-s3-box-3/main/protocols/websocket_protocol.cc`

**Step 1: Inspect firmware audio flow**
```bash
grep -n "wake\|prebuf\|listen.*start" ~/esp32-s3-box-3/main/audio_service.cc | head
grep -n "wake\|prebuf" ~/esp32-s3-box-3/main/protocols/websocket_protocol.cc | head
```

**Step 2: Check server-side reception**
```bash
fly ssh console --app jarvis-server -C "grep -rn 'wake\|listen.*start' /opt/xiaozhi-esp32-server/core/ | head"
```

**Step 3: Document findings**
Append to `~/esp32-s3-box-3/docs/plans/2026-04-29-phase1-notes.md`:
- Where wake-word buffer enters the WebSocket stream
- Frame format (Opus? PCM? Sample rate?)
- Whether the buffer is contiguous with the post-wake utterance

**Step 4: Decision point**
- If wake-word audio is captured and sent: proceed to Task 3.
- If wake-word audio is dropped before TX: add a firmware patch task before continuing — modify `audio_service.cc` to retain the 1-second prebuffer and send on `listen/start`.

---

### Task 3: Inspect xiaozhi-esp32-server audio pipeline for the gate insertion point

**Files (read only):** `~/jarvis-server/` (image content via `fly ssh`)

**Step 1: Find the function that hands audio to ASR**
```bash
fly ssh console --app jarvis-server -C "grep -rn 'asr\|transcribe' /opt/xiaozhi-esp32-server/core/handle/ | head -20"
```

**Step 2: Locate the connection-handling state machine**
```bash
fly ssh console --app jarvis-server -C "grep -rn 'state.*listen\|listen.*start\|handle_listen' /opt/xiaozhi-esp32-server/core/ | head"
```

**Step 3: Document gate insertion points**
Append findings to phase1-notes.md. Need 2 hooks:
1. After wake-word audio arrives, before ASR invocation → Layer A check
2. After full utterance is buffered, before sending to ASR → Layer B diarization

**Step 4: Verify**
```bash
test -f ~/esp32-s3-box-3/docs/plans/2026-04-29-phase1-notes.md
```

---

### Task 4: Add speaker_id Python module skeleton (TDD: write failing tests first)

**Files:**
- Create: `~/jarvis-server/speaker_id/__init__.py`
- Create: `~/jarvis-server/speaker_id/embedder.py`
- Create: `~/jarvis-server/tests/test_embedder.py`

**Step 1: Add failing test**
`tests/test_embedder.py`:
```python
import numpy as np
import pytest

def test_embedder_returns_192d_vector():
    from speaker_id.embedder import VoiceEmbedder
    e = VoiceEmbedder()
    fake_audio = np.zeros(16000 * 2, dtype=np.float32)  # 2s of silence at 16kHz
    embedding = e.embed(fake_audio, sample_rate=16000)
    assert embedding.shape == (192,)
    assert embedding.dtype == np.float32
```
Run:
```bash
cd ~/jarvis-server && pytest tests/test_embedder.py
```
Expected: ImportError (module doesn't exist yet).

**Step 2: Implement minimal**
`speaker_id/embedder.py`:
```python
import numpy as np
from speechbrain.inference.speaker import EncoderClassifier
import torch

class VoiceEmbedder:
    """ECAPA-TDNN voice embedder. 192-dim float32 output."""
    _model = None

    def __init__(self):
        if VoiceEmbedder._model is None:
            VoiceEmbedder._model = EncoderClassifier.from_hparams(
                source="speechbrain/spkrec-ecapa-voxceleb",
                savedir="/opt/xiaozhi-esp32-server/data/models/ecapa",
                run_opts={"device": "cpu"},
            )

    def embed(self, audio: np.ndarray, sample_rate: int = 16000) -> np.ndarray:
        if sample_rate != 16000:
            raise ValueError(f"expected 16kHz, got {sample_rate}")
        with torch.no_grad():
            x = torch.from_numpy(audio).unsqueeze(0)
            emb = VoiceEmbedder._model.encode_batch(x).squeeze().cpu().numpy()
        return emb.astype(np.float32)
```

`speaker_id/__init__.py`:
```python
from .embedder import VoiceEmbedder
__all__ = ["VoiceEmbedder"]
```

**Step 3: Add deps to requirements**
Append to `~/jarvis-server/requirements.txt`:
```
speechbrain>=1.0
torch>=2.0,<3.0
torchaudio>=2.0
```

**Step 4: Verify**
```bash
pip install -r requirements.txt
pytest tests/test_embedder.py -v
```
Expected: pass.

**Step 5: Commit**
```bash
git add speaker_id/ tests/test_embedder.py requirements.txt
git commit -m "feat(speaker-id): add ECAPA-TDNN voice embedder"
```

---

### Task 5: Encrypted voiceprint storage

**Files:**
- Create: `~/jarvis-server/speaker_id/storage.py`
- Create: `~/jarvis-server/tests/test_storage.py`

**Does NOT cover:** This task gates voiceprint storage to "encrypted at rest only." It does NOT cover key rotation or HSM-backed key management — both deferred. If the master key leaks, all voiceprints leak with it.

**Step 1: Failing test**
`tests/test_storage.py`:
```python
import os
import numpy as np
import pytest
import tempfile

@pytest.fixture
def tmp_db():
    with tempfile.NamedTemporaryFile(suffix=".db", delete=False) as f:
        yield f.name
    os.unlink(f.name)

def test_store_and_retrieve_roundtrip(tmp_db, monkeypatch):
    monkeypatch.setenv("VOICEPRINT_KEY", "0" * 64)  # 32-byte hex
    from speaker_id.storage import VoiceprintStore
    store = VoiceprintStore(tmp_db)
    emb = np.random.rand(192).astype(np.float32)
    store.save("owner", emb)
    loaded = store.load("owner")
    np.testing.assert_array_almost_equal(emb, loaded, decimal=6)

def test_missing_key_raises(tmp_db, monkeypatch):
    monkeypatch.delenv("VOICEPRINT_KEY", raising=False)
    from speaker_id.storage import VoiceprintStore
    with pytest.raises(RuntimeError, match="VOICEPRINT_KEY"):
        VoiceprintStore(tmp_db)
```

**Step 2: Implement**
`speaker_id/storage.py`:
```python
import os
import sqlite3
import time
import numpy as np
from cryptography.hazmat.primitives.ciphers.aead import AESGCM


class VoiceprintStore:
    def __init__(self, db_path: str):
        key_hex = os.environ.get("VOICEPRINT_KEY", "").strip()
        if len(key_hex) != 64:
            raise RuntimeError("VOICEPRINT_KEY must be 64 hex chars (32 bytes)")
        self._aes = AESGCM(bytes.fromhex(key_hex))
        self._conn = sqlite3.connect(db_path)
        self._conn.execute("""
            CREATE TABLE IF NOT EXISTS voiceprints (
                user_id TEXT PRIMARY KEY,
                enrolled_at INTEGER NOT NULL,
                blob BLOB NOT NULL,
                sample_count INTEGER NOT NULL,
                threshold REAL DEFAULT 0.65
            )
        """)

    def save(self, user_id: str, embedding: np.ndarray, sample_count: int = 1):
        nonce = os.urandom(12)
        ct = self._aes.encrypt(nonce, embedding.tobytes(), user_id.encode())
        self._conn.execute(
            "REPLACE INTO voiceprints VALUES (?, ?, ?, ?, ?)",
            (user_id, int(time.time()), nonce + ct, sample_count, 0.65),
        )
        self._conn.commit()

    def load(self, user_id: str) -> np.ndarray | None:
        row = self._conn.execute(
            "SELECT blob FROM voiceprints WHERE user_id=?", (user_id,)
        ).fetchone()
        if not row:
            return None
        blob = row[0]
        nonce, ct = blob[:12], blob[12:]
        pt = self._aes.decrypt(nonce, ct, user_id.encode())
        return np.frombuffer(pt, dtype=np.float32)
```

**Step 3: Verify**
```bash
pytest tests/test_storage.py -v
```
Expected: pass.

**Step 4: Commit**
```bash
git add speaker_id/storage.py tests/test_storage.py
git commit -m "feat(speaker-id): encrypted voiceprint storage with AES-GCM"
```

---

### Task 6: Set Fly.io secret for VOICEPRINT_KEY

**Step 1: Generate and store key**
```bash
KEY=$(openssl rand -hex 32)
fly secrets set VOICEPRINT_KEY="$KEY" --app jarvis-server
```

**Step 2: Verify**
```bash
fly secrets list --app jarvis-server | grep VOICEPRINT_KEY
```
Expected: `VOICEPRINT_KEY  Deployed`.

**Step 3: WARNING** — this key cannot be rotated without re-enrolling. Document in `docs/RUNBOOK.md` (created in Task 27).

---

### Task 7: Wake-word similarity gate (Layer A)

**Files:**
- Create: `~/jarvis-server/speaker_id/gate.py`
- Create: `~/jarvis-server/tests/test_gate.py`

**Does NOT cover:** This task gates on wake-word audio only. It does NOT detect mid-utterance speaker switches (that's Task 11 — diarization). If the bystander starts talking *after* the owner's wake word, Layer A passes; Layer B catches it.

**Step 1: Failing test**
`tests/test_gate.py`:
```python
import numpy as np
import pytest

def test_gate_accepts_matching_voice():
    from speaker_id.gate import VoiceGate
    owner = np.array([0.5] * 192, dtype=np.float32)
    candidate = owner.copy() + np.random.normal(0, 0.01, 192).astype(np.float32)
    g = VoiceGate(threshold=0.95)
    assert g.matches(owner, candidate) is True

def test_gate_rejects_different_voice():
    from speaker_id.gate import VoiceGate
    owner = np.array([0.5] * 192, dtype=np.float32)
    bystander = -owner
    g = VoiceGate(threshold=0.65)
    assert g.matches(owner, bystander) is False
```

**Step 2: Implement**
`speaker_id/gate.py`:
```python
import numpy as np
from dataclasses import dataclass


@dataclass
class VoiceGate:
    threshold: float = 0.65

    def similarity(self, a: np.ndarray, b: np.ndarray) -> float:
        return float(np.dot(a, b) / (np.linalg.norm(a) * np.linalg.norm(b)))

    def matches(self, owner: np.ndarray, candidate: np.ndarray) -> bool:
        return self.similarity(owner, candidate) >= self.threshold
```

**Step 3: Verify**
```bash
pytest tests/test_gate.py -v
```
Expected: pass.

**Step 4: Commit**
```bash
git add speaker_id/gate.py tests/test_gate.py
git commit -m "feat(speaker-id): cosine-similarity wake-word gate"
```

---

### Task 8: Audit log for wake attempts

**Files:**
- Modify: `~/jarvis-server/speaker_id/storage.py`
- Modify: `~/jarvis-server/tests/test_storage.py`

**Step 1: Failing test** — extend storage to record wake attempts:
```python
def test_log_wake_attempt(tmp_db, monkeypatch):
    monkeypatch.setenv("VOICEPRINT_KEY", "0" * 64)
    from speaker_id.storage import VoiceprintStore
    store = VoiceprintStore(tmp_db)
    store.log_attempt(similarity=0.71, accepted=True)
    rows = store.recent_attempts(limit=1)
    assert len(rows) == 1
    assert rows[0]["accepted"] is True
```

**Step 2: Implement** — add to `VoiceprintStore`:
```python
    def log_attempt(self, similarity: float, accepted: bool):
        self._conn.execute("""
            CREATE TABLE IF NOT EXISTS wake_attempts (
                id INTEGER PRIMARY KEY AUTOINCREMENT,
                ts INTEGER NOT NULL,
                similarity REAL NOT NULL,
                accepted INTEGER NOT NULL
            )
        """)
        self._conn.execute(
            "INSERT INTO wake_attempts (ts, similarity, accepted) VALUES (?, ?, ?)",
            (int(time.time()), float(similarity), int(accepted)),
        )
        self._conn.commit()

    def recent_attempts(self, limit: int = 100):
        cur = self._conn.execute(
            "SELECT ts, similarity, accepted FROM wake_attempts "
            "ORDER BY id DESC LIMIT ?", (limit,)
        )
        return [{"ts": r[0], "similarity": r[1], "accepted": bool(r[2])} for r in cur]
```

**Step 3: Verify**
```bash
pytest tests/test_storage.py -v
```

**Step 4: Commit**
```bash
git add speaker_id/storage.py tests/test_storage.py
git commit -m "feat(speaker-id): audit log for wake-word attempts (no audio retained)"
```

---

### Task 9: Enrollment service — average 5 phrases, reject low-SNR

**Files:**
- Create: `~/jarvis-server/speaker_id/enrollment.py`
- Create: `~/jarvis-server/tests/test_enrollment.py`

**Step 1: Failing test**
```python
import numpy as np
import pytest

def test_enrollment_averages_embeddings(tmp_path, monkeypatch):
    monkeypatch.setenv("VOICEPRINT_KEY", "0" * 64)
    from speaker_id.enrollment import Enroller
    from speaker_id.storage import VoiceprintStore
    store = VoiceprintStore(str(tmp_path / "vp.db"))
    e = Enroller(store)
    embs = [np.random.rand(192).astype(np.float32) for _ in range(5)]
    e.enroll(user_id="owner", embeddings=embs)
    loaded = store.load("owner")
    expected = np.mean(np.stack(embs), axis=0).astype(np.float32)
    np.testing.assert_array_almost_equal(loaded, expected, decimal=5)

def test_rejects_outlier_embedding(tmp_path, monkeypatch):
    monkeypatch.setenv("VOICEPRINT_KEY", "0" * 64)
    from speaker_id.enrollment import Enroller
    from speaker_id.storage import VoiceprintStore
    store = VoiceprintStore(str(tmp_path / "vp.db"))
    e = Enroller(store)
    base = np.random.rand(192).astype(np.float32)
    embs = [base + np.random.normal(0, 0.01, 192).astype(np.float32) for _ in range(4)]
    embs.append(-base)  # outlier
    with pytest.raises(ValueError, match="outlier"):
        e.enroll(user_id="owner", embeddings=embs)
```

**Step 2: Implement**
`speaker_id/enrollment.py`:
```python
import numpy as np


class Enroller:
    def __init__(self, store):
        self.store = store

    def enroll(self, user_id: str, embeddings: list[np.ndarray]):
        if len(embeddings) < 3:
            raise ValueError("at least 3 phrases required")
        stacked = np.stack(embeddings)
        centroid = stacked.mean(axis=0)
        # Outlier rejection: any phrase >0.4 cosine distance from centroid
        for i, e in enumerate(stacked):
            sim = np.dot(e, centroid) / (np.linalg.norm(e) * np.linalg.norm(centroid))
            if sim < 0.6:
                raise ValueError(f"outlier phrase #{i+1}, similarity={sim:.2f}")
        self.store.save(user_id, centroid.astype(np.float32), sample_count=len(embeddings))
```

**Step 3: Verify**
```bash
pytest tests/test_enrollment.py -v
```

**Step 4: Commit**
```bash
git add speaker_id/enrollment.py tests/test_enrollment.py
git commit -m "feat(speaker-id): enrollment with averaging and outlier rejection"
```

---

### Task 10: SNR check before accepting enrollment audio

**Files:**
- Create: `~/jarvis-server/speaker_id/audio_quality.py`
- Create: `~/jarvis-server/tests/test_audio_quality.py`

**Step 1: Failing test** for `estimate_snr_db(audio: np.ndarray) -> float` returning ≥10 for clean signal, <10 for very noisy.

**Step 2: Implement** using simple energy-of-speech vs energy-of-silence ratio (use webrtcvad to find speech/non-speech frames; if vad unavailable, fall back to top-percentile energy / bottom-percentile energy).

**Step 3: Wire into Enroller** — reject any phrase audio with SNR < 10dB and ask user to retry.

**Step 4: Verify, commit.**

---

### Task 11: Diarization (Layer B)

**Files:**
- Create: `~/jarvis-server/speaker_id/diarizer.py`
- Create: `~/jarvis-server/tests/test_diarizer.py`

**Does NOT cover:** Diarization here only attributes segments to the *enrolled owner* vs "everyone else." Multi-user diarization (separating spouse from kid from owner) is explicitly out of scope (Q7 = single-user).

**Step 1: Failing test** — feed a synthetic 4-second clip with first 2s = owner-like noise pattern, last 2s = different noise pattern; expect diarizer to return `[(0.0, 2.0)]` as owner segments.

**Step 2: Implement** using pyannote-audio segmentation pipeline + per-segment embedding + cosine compare to owner voiceprint:
```python
from pyannote.audio import Pipeline
from speaker_id.embedder import VoiceEmbedder

class OwnerDiarizer:
    def __init__(self, owner_voiceprint, threshold=0.65):
        self.owner = owner_voiceprint
        self.threshold = threshold
        self.pipe = Pipeline.from_pretrained("pyannote/segmentation-3.0")
        self.embedder = VoiceEmbedder()

    def owner_segments(self, audio, sr=16000):
        segments = self.pipe({"waveform": audio, "sample_rate": sr})
        owner_segs = []
        for seg in segments.get_timeline().support():
            slice_ = audio[int(seg.start * sr):int(seg.end * sr)]
            emb = self.embedder.embed(slice_, sr)
            sim = np.dot(self.owner, emb) / (np.linalg.norm(self.owner) * np.linalg.norm(emb))
            if sim >= self.threshold:
                owner_segs.append((seg.start, seg.end))
        return owner_segs
```

**Step 3: Verify, commit.**

---

### Task 12: Wire Layer A into xiaozhi-esp32-server's listen-start handler

**Files:**
- Modify: `/opt/xiaozhi-esp32-server/core/handle/receiveAudioHandle.py` (or whichever file Task 3 identified)
- Modify (locally): `~/jarvis-server/Dockerfile` to copy our `speaker_id/` module into the image

**Step 1: Add `speaker_id` to Docker image**
In `~/jarvis-server/Dockerfile`, add (after the WORKDIR):
```dockerfile
COPY speaker_id/ /opt/xiaozhi-esp32-server/speaker_id/
COPY requirements-speaker-id.txt /tmp/
RUN pip install -r /tmp/requirements-speaker-id.txt
```

Create `requirements-speaker-id.txt`:
```
speechbrain>=1.0
torch>=2.0,<3.0
torchaudio>=2.0
pyannote.audio>=3.1
cryptography>=41
numpy>=1.24
```

**Step 2: Patch xiaozhi-esp32-server**
Find the function from Task 3 that's invoked when listen-start completes (full utterance buffered). Insert a hook BEFORE ASR call:
```python
# At top of file
from speaker_id.embedder import VoiceEmbedder
from speaker_id.gate import VoiceGate
from speaker_id.storage import VoiceprintStore

_embedder = VoiceEmbedder()
_gate = VoiceGate(threshold=0.65)
_store = VoiceprintStore("/opt/xiaozhi-esp32-server/data/voiceprints.db")

def _is_owner(audio_pcm_16k: np.ndarray) -> bool:
    owner = _store.load("owner")
    if owner is None:
        return True  # not enrolled → allow (fallback to address filter)
    cand = _embedder.embed(audio_pcm_16k)
    sim = _gate.similarity(owner, cand)
    accepted = sim >= _gate.threshold
    _store.log_attempt(similarity=sim, accepted=accepted)
    return accepted
```

In the `listen-start` → ASR-call path, add:
```python
if not _is_owner(captured_audio):
    log.info(f"voice gate rejected utterance, similarity={sim:.2f}")
    await send_empty_tts_cycle(conn)  # device returns to idle
    return
```

**Step 3: Deploy + verify with bystander test**
```bash
cd ~/jarvis-server && fly deploy
# ask a friend (or use a recording of a different voice) to say "Jarvis"
# verify device displays nothing and returns to idle
fly logs --app jarvis-server --no-tail | grep "voice gate" | tail
```
Expected: log shows `voice gate rejected` with similarity below threshold.

**Step 4: Commit**

---

### Task 13: Wire Layer B (diarization) into utterance pipeline

**Files:**
- Modify: same handler as Task 12

**Step 1:** After Layer A passes, run diarization on the full utterance, slice the audio to owner-only segments, send only those to ASR.
**Step 2:** Verify with mid-conversation handoff test (owner says "Jarvis, what's the weather—" then bystander interrupts; only owner's text reaches LLM).
**Step 3:** Commit.

---

### Task 14: MCP enrollment tool

**Files:**
- Create: `~/jarvis-server/speaker_id/mcp_enroll.py`
- Modify: xiaozhi-esp32-server MCP tool registry

**Step 1:** Register tool `self.assistant.enroll_owner` with description: "Begin voiceprint enrollment for the owner. Asks the user to speak 5 prompted phrases."
**Step 2:** Tool flow:
1. Server speaks (TTS): "Beginning enrollment, sir. Please repeat: phrase one of five — *the quick brown fox jumps over the lazy dog*."
2. Captures next utterance.
3. Embeds, validates SNR, repeats 4 more times.
4. Saves averaged voiceprint.
5. Confirms: "Enrollment complete. From now on, I'll respond only to you."
**Step 3:** Test flow end-to-end on device.
**Step 4:** Commit.

---

### Task 15: Passphrase fallback (sick/laryngitis recovery)

**Files:**
- Create: `~/jarvis-server/speaker_id/fallback.py`

**Step 1:** When wake gate rejects, check if utterance starts with `"Jarvis listen to me sir"` (LLM-classified, not exact match). If yes, prompt to enroll temporarily for 2 hours: "Your voice sounds different. Confirm by saying: <random word from screen>."
**Step 2:** On confirm, allow this session for 2 hours with relaxed threshold (0.45).
**Step 3:** Verify, commit.

---

### Task 16: Voice command — adjust threshold

**Files:**
- New MCP tool: `self.assistant.adjust_voice_gate`

**Step 1:** Tool spec: param `direction` ∈ {"raise", "lower"}; adjusts stored threshold ±0.03, clamps to [0.4, 0.85].
**Step 2:** Test, commit.

---

### Task 17: First-run "no voiceprint" fallback

**Files:**
- Modify: server startup logic

**Step 1:** On boot, if voiceprints DB has no `owner` row, log warning and use the existing `BRIDGE_ADDRESS_FILTER`-style behavior (only respond if utterance contains "jarvis").
**Step 2:** Display reminder on device boot UI: "Jarvis: Not enrolled. Say 'Jarvis, enroll me' to begin."
**Step 3:** Test by deleting voiceprints DB, rebooting, verifying fallback works.

---

### Task 18: Threshold auto-tune script (manual run, 2 weeks post-deploy)

**Files:**
- Create: `~/jarvis-server/scripts/tune_threshold.py`

**Step 1:** Script reads `wake_attempts` log, computes false-accept rate (manual labels needed) and false-reject rate, recommends threshold delta.
**Step 2:** Document in RUNBOOK.

---

### Task 19: Integration test — bystander vs owner

**Files:**
- Create: `~/jarvis-server/tests/integration/test_bystander.py`

**Step 1:** Use 2 different voice samples (e.g., LibriSpeech samples), enroll one as owner, verify the other is rejected at gate.
**Step 2:** Add to CI as a slow test (skip on PRs, run on main).

---

### Task 20: Documentation

**Files:**
- Create: `~/esp32-s3-box-3/docs/SPEAKER_ENROLLMENT.md`

User-facing: "How to enroll your voice" — includes troubleshooting (low SNR, retry steps, removing voiceprint, multi-user future).

---

## Definition of Done

- [ ] Owner enrolled; bystander false-accept rate < 5% on internal tests
- [ ] Layer A and Layer B both active in production
- [ ] Voice command "Jarvis, enroll me" works end-to-end on device
- [ ] Encrypted voiceprint storage; key in Fly.io secrets
- [ ] Audit log of wake attempts viewable
- [ ] First-run fallback (no enrollment yet) does not crash
- [ ] User-facing enrollment doc exists
- [ ] All unit tests green; integration tests green on main
