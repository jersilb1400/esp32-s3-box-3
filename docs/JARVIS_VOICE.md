# Voice stack on Jarvis BOX-3 firmware

Firmware handles **wake word**, **AFE / NS / optional device AEC**, **Opus uplink/downlink**, and **WebSocket handshake**. Persona and TTS voice are chosen on the backend; see [`JARVIS_AGENT_SETUP.md`](JARVIS_AGENT_SETUP.md).

## OTA → WebSocket (jarvis-server / xiaozhi-style bridge)

1. **`CONFIG_OTA_URL`** resolves **firmware/asset checks** and returns JSON with websocket URL, token, and protocol **version** (stored under NVS keys `websocket` / `mqtt` by existing OTA code).
2. On **Open audio channel**, the device connects with headers `Protocol-Version`, `Device-Id`, `Client-Id`, and optional `Authorization: Bearer …`.
3. **Client hello** (`type: "hello"`) includes `version`, `transport: "websocket"`, **`audio_params`**: `{ "format": "opus", "sample_rate": 16000, "channels": 1, "frame_duration": 60 }` (60 ms matches `OPUS_FRAME_DURATION_MS` in `audio_service.h`), and **`features`**: MCP on; **`aec: true` only when `CONFIG_USE_SERVER_AEC=y`** (never combine with device AEC — build fails if both enabled).
4. Server answers with **`type: "hello"`** carrying **`session_id`** and negotiated **`audio_params`** (sample rate / frame duration). Downlink Opus frames use the framing implied by **`Protocol-Version`** (raw Opus vs binary protocol headers) in `websocket_protocol.cc`.
5. **JSON channel**: `tts` (`start` / `stop` / `sentence_start`), `stt`, `llm` (emotion), `mcp`, `system`, `alert`, optional `custom`. Device **must** receive `listen`/`start`-equivalent semantics via **`SendStartListening`** on each listening turn (see comments in `application.cc`).

## Wake word and uplink quirks

| Kconfig | Effect |
|---------|--------|
| `CONFIG_USE_CUSTOM_WAKE_WORD` | Multinet phrase (`CONFIG_CUSTOM_WAKE_WORD`) |
| `CONFIG_CUSTOM_WAKE_WORD_THRESHOLD` | Lower = **more sensitive** (more false triggers); higher = stricter |
| `CONFIG_WAKE_WORD_DETECTION_IN_LISTENING` | Barge-in / re-wake while listening |
| `CONFIG_SEND_WAKE_WORD_DATA` | Sends encoded wake audio + **`SendWakeWordDetected`** before normal mic stream |

Tune threshold first (e.g. 18–28) before changing phrase or rebuilding assets.

## AEC modes

| Build | Hello `features.aec` | Listening mode default | Notes |
|-------|---------------------|-------------------------|-------|
| `CONFIG_USE_DEVICE_AEC` only | omitted / false path | **`kListeningModeRealtime`** when AEC branch active | BOARD must expose **speaker reference** (e.g. `AUDIO_INPUT_REFERENCE` on BOX-3). Firmware calls **`EnableDeviceAec(true)`** at startup so runtime matches Kconfig before the first conversation. |
| `CONFIG_USE_SERVER_AEC` only | `aec: true` | Realtime semantics + timestamp queue from playback (`audio_service.cc`) | Unstable upstream; jarvis deployments typically use **device AEC**. |
| Neither | — | **`kListeningModeAutoStop`** | No echo cancellation negotiation in hello. |

**Do not enable** `CONFIG_USE_DEVICE_AEC` and `CONFIG_USE_SERVER_AEC` together.

## Quick verification checklist

1. Idle: wake phrase opens channel; logs show websocket connect + server hello.
2. With **`CONFIG_SEND_WAKE_WORD_DATA=y`**: server receives wake Opus bursts then normal stream after `listen` start.
3. During TTS: barge-in (if wake-in-listening enabled) fires **AbortSpeaking** and resumes listening path.
4. If mic is gated or STT silent: confirm **`SendStartListening`** runs each turn (`HandleStateChangedEvent` for listening).

## Sleep phrase, wake-only, and silence auto-hang-up

New Jarvis presets (see BOX-3 defaults in `sdkconfig.defaults.esp-box3-xiaozhi`):

- **`CONFIG_CUSTOM_SLEEP_WORD`** / **`CONFIG_CUSTOM_SLEEP_WORD_DISPLAY`** add a secondary Multinet command with **`action = sleep`** (`scripts/build_default_assets.py` merges it into `index.json`).
- **`CONFIG_JARVIS_WAKE_ONLY_CHAT`** blocks BOOT/toggle / MCP `StartListening` from opening a websocket while idle—you must invoke the Jarvis phrase first.
- **`CONFIG_JARVIS_SILENCE_END_SESSION_SEC`** arms a FreeRTOS timer every time microphone VAD returns to silence during **listening**; exceeding the configured window triggers **`SendStopListening()`** + **`CloseAudioChannel()`** (local “conversation finished”).

Remote control JSON is now supported:

```
{"type":"session","command":"sleep"}
```

Accepted `command` strings: **`voice_sleep`**, **`sleep`**, **`end`**, **`close`** (matching strings without extra metadata). `sleep` behaves like offline Multinet sleep and toggles **`voice.sleep_standby`** NVS. `close` hangs up instantly without flipping the persistent sleep flag.

Multinet sleeps only run while **`CustomWakeWord`** is fed—which is idle standby unless you customise the audio graph. Teach your STT layer to recognise “go to sleep” during active sessions and relay the JSON above.

## Speaker verification integration

Turn on **`CONFIG_JARVIS_SPEAKER_VERIFY_HELLO=y`** during bring-up so the websocket `hello` advertises **`"speaker_verify": true`**.

Copy `contrib/jarvis-server-speaker-verify/` alongside `~/jarvis-server`; it persists embeddings keyed by **`Device-Id`/`board.uuid`**, converts wake Opus (`CONFIG_SEND_WAKE_WORD_DATA=y`) bursts into cosine checks, and rejects unknown speakers before bridging to the LLM.
