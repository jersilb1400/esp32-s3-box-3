# Local Hybrid Bridge (ESP32 + Ollama + Claude/OpenClaude)

This bridge is a **scaffold** for the ESP32 firmware in this repository:

- Serves OTA config at `POST /xiaozhi/ota/`
- Serves WebSocket session endpoint at `GET /ws`
- Default LLM: **Claude (Anthropic API)** primary, optional **Ollama** fallback (swap with `ollama-primary.env.example` for the reverse)
- Exposes a small MCP tool surface (`local.bridge.chat`) for deterministic testing

It is intentionally minimal and does **not** yet decode/encode Opus audio streams.
It now supports **binary Opus frame passthrough hooks** for STT/TTS via pluggable HTTP providers.

## Why this shape matches the firmware

The firmware (`main/ota.cc` and `main/protocols/websocket_protocol.cc`) expects:

1. OTA URL returning a JSON object with a `websocket` section (`url`, `token`, `version`)
2. WebSocket handshake (`type: "hello"`, `transport: "websocket"`)
3. JSON message exchange (`stt`, `tts`, `mcp`) on that socket

This scaffold implements those control-plane requirements so you can plug in your local stack.

### You cannot “remove the bridge” and talk to Anthropic from the device alone

The box speaks **WebSocket + Opus** (xiaozhi protocol). **Claude** is **HTTPS** JSON. Something on your network (this app) must translate. You *can* run **one** Python process: no separate `stt_server` / `tts_server` (see **Simple profile** below).

## Simple profile (one process: Claude + OpenAI voice)

One terminal, no STT/TTS sidecars, no `STT_HTTP_URL` / `TTS_HTTP_URL` loopback:

- **Claude** (Anthropic): `ANTHROPIC_API_KEY`, `ANTHROPIC_MODEL=claude-sonnet-4-6` (or current id).
- **Speech** (OpenAI, in-process): `OPENAI_API_KEY`, `STT_PROVIDER=openai`, `TTS_PROVIDER=openai`.
- **LAN**: set `BRIDGE_WEBSOCKET_URL` to `ws://<this-pc-lan-ip>:8000/ws` and OTA to `http://<this-pc-lan-ip>:8000/xiaozhi/ota/`.
- `pip install -r requirements.txt` (includes `opuslib`; macOS: `brew install pkg-config opus` if needed).

```bash
cd local-bridge
source .venv/bin/activate
python -m bridge.server
```

You still use **`./run-bridge.sh`** if you prefer; it is the same server.

## Setup

```bash
cd local-bridge
python3 -m venv .venv
source .venv/bin/activate
pip install -r requirements.txt
# STT: minimal deps (OpenAI or Opus-only); local Whisper is optional — see below
pip install -r requirements-stt.txt
# Optional: local faster-whisper (Python 3.10–3.12 often required for wheels)
# pip install -r requirements-stt-local.txt
```

On macOS, install **libopus** for `pip install opuslib` to work: `brew install pkg-config opus`.

### Bundled STT server (`stt_server.py`)

This repo includes `stt_server.py`, an HTTP service that implements the same JSON contract as the bridge’s `STT_HTTP_URL`:

- `POST /transcribe` with `{"audio_format":"opus","frames_base64":[...]}` → `{"text":"..."}`  
- `GET /healthz` for a quick check

It decodes **24 kHz, 60 ms** Opus frames (what the ESP-Box stack sends), then transcribes using either:

1. **Local** — **faster-whisper** (no cloud; install `requirements-stt-local.txt`). Prebuilt `onnxruntime` / `av` wheels are often only available for **Python 3.10–3.12**. On **3.14+**, use option 2 or a 3.12 venv for local Whisper.
2. **OpenAI** — `POST` to OpenAI’s **audio transcriptions** API (`STT_ENGINE=openai` and `OPENAI_API_KEY`). No heavy ML stack; API usage is billed to your OpenAI account.

| Variable | Default | Meaning |
|----------|---------|---------|
| `STT_ENGINE` | `auto` | `auto` → use local faster-whisper if importable, else OpenAI if `OPENAI_API_KEY` set. `local` / `openai` to force. |
| `STT_PREFER_OPENAI` | *(empty)* | If `1`/`true` and `OPENAI_API_KEY` is set, use OpenAI even when local Whisper is available. |
| `OPENAI_API_KEY` | *(empty)* | Required for `STT_ENGINE=openai` (or `auto` when local ML deps are missing). |
| `OPENAI_STT_BASE_URL` | `https://api.openai.com/v1/audio/transcriptions` | Override for API-compatible proxies. |
| `OPENAI_STT_MODEL` | `whisper-1` | OpenAI STT model name. |
| `STT_LISTEN_HOST` | `0.0.0.0` | Bind address |
| `STT_LISTEN_PORT` | `9001` | Port (match `STT_HTTP_URL` in `.env`) |
| `STT_OPUS_SAMPLE_RATE` | `24000` | Must match device / bridge |
| `STT_OPUS_FRAME_MS` | `60` | Frame size in ms |
| `WHISPER_MODEL` | `base` | Local only: `tiny` / `base` / `small` / … (first run downloads weights) |
| `WHISPER_DEVICE` | `auto` | Local only: `cpu` / `cuda` / `auto` |
| `WHISPER_COMPUTE_TYPE` | `int8` | Local only, e.g. `float16` on GPU |
| `WHISPER_LANGUAGE` | *(empty)* | e.g. `en` to force English; empty = auto (both engines) |
| `STT_HTTP_API_KEY` | *(empty)* | If set, requires `Authorization: Bearer …` (same as bridge) |

**Run** (second terminal, `.env` in `local-bridge` so `OPENAI_API_KEY` / `STT_HTTP_API_KEY` match):

```bash
cd local-bridge
source .venv/bin/activate
./run-stt.sh
```

Set `STT_HTTP_URL=http://127.0.0.1:9001/transcribe` and `STT_PROVIDER=http` in `.env` and restart the bridge. The ESP32 talks only to the **bridge**; the bridge calls STT on your PC at `127.0.0.1`.

### Bundled TTS server (`tts_server.py`)

The device **only plays audio** from **WebSocket binary Opus frames**. `sentence_start` updates the **screen** text; it does **not** speak locally. If `TTS_PROVIDER=none` or TTS returns no frames, you will see the reply on the display but **hear nothing**.

This repo includes `tts_server.py`:

- `POST /synthesize` with `{"text":"...","audio_format":"opus"}` → `{"frames_base64":[...]}`  
- Uses **OpenAI** [`/v1/audio/speech`](https://platform.openai.com/docs/api-reference/audio/createSpeech) (`response_format: pcm`, 24 kHz mono), then encodes **60 ms Opus** frames for the bridge to send to the device.

| Variable | Default | Meaning |
|----------|---------|---------|
| `OPENAI_API_KEY` | *(required)* | Same as STT if you use OpenAI; bills TTS usage. |
| `OPENAI_TTS_BASE_URL` | `https://api.openai.com/v1/audio/speech` | Override if needed. |
| `OPENAI_TTS_MODEL` | `tts-1` | `tts-1` or `tts-1-hd`. |
| `OPENAI_TTS_VOICE` | `alloy` | `alloy`, `echo`, `fable`, `onyx`, `nova`, `shimmer`. |
| `TTS_LISTEN_HOST` / `TTS_LISTEN_PORT` | `0.0.0.0` / `9002` | Match `TTS_HTTP_URL` in `.env`. |
| `TTS_HTTP_API_KEY` | *(empty)* | Optional `Authorization: Bearer` (same as bridge). |

```bash
pip install -r requirements-tts.txt
./run-tts.sh
```

In `.env`: `TTS_PROVIDER=http`, `TTS_HTTP_URL=http://127.0.0.1:9002/synthesize`, and **restart the bridge**. Run **bridge + STT + TTS** together for full voice.

`source .env` is shell syntax: any value with **spaces** must be in **double quotes**, e.g. `BRIDGE_SYSTEM_PROMPT="You are ..."`. Otherwise the shell tries to run words like `are` as commands.

## Environment

Optional **`BRIDGE_SYSTEM_PROMPT_FILE`**: absolute path to a UTF-8 text file used as the LLM system prompt (overrides inline `BRIDGE_SYSTEM_PROMPT` when the file exists). Useful for long JARVIS-style personas without huge `.env` lines.

Primary options:

- `LLM_PROVIDER=ollama|anthropic` (default: **`anthropic`** — Claude API)
- `LLM_FALLBACK_PROVIDER=anthropic|ollama` (default: **`ollama`**, used when `LLM_ENABLE_FALLBACK=true`)
- `LLM_ENABLE_FALLBACK=true|false` (default: **`false`** — use Anthropic only unless you intentionally run Ollama as backup)
- `ANTHROPIC_API_KEY` (required when Anthropic is primary or enabled as fallback)
- `OLLAMA_BASE_URL` (required when Ollama is primary or enabled as fallback)
- `OLLAMA_MODEL` (default: `llama3.2:3b`)
- `ANTHROPIC_MODEL` (default: `claude-sonnet-4-6` — pick a [current Claude API id](https://docs.anthropic.com/en/docs/about-claude/models); older 3.5 snapshot ids are **retired** and return **404**)
- `OPENCLAUDE_SKILL_URL` (optional; if set, called before LLM fallback)
- `STT_PROVIDER=none|http|openai` — **`openai`** = in-process OpenAI transcriptions (needs `OPENAI_API_KEY`); no `stt_server`
- `STT_HTTP_URL` (required for `STT_PROVIDER=http` only)
- `TTS_PROVIDER=none|http|openai` — **`openai`** = in-process OpenAI speech + Opus (needs `OPENAI_API_KEY`); no `tts_server`

**End-of-utterance (ESP-Box + `realtime` mode):** The device often does not send `listen`/`state`:`stop` when using device AEC. The bridge treats **silence** after the last Opus frame as end-of utterance and runs STT (same as an explicit `stop`). Press-to-talk devices that use `mode: manual` are unchanged. Tunables:

| Variable | Default | Meaning |
|----------|---------|---------|
| `BRIDGE_REALTIME_SILENCE_EOU` | `true` | Set `false` to disable silence-based processing (only explicit `stop` or `detect` flows). |
| `BRIDGE_REALTIME_SILENCE_MS` | `1000` | Max gap (ms) with no new Opus before inferring you finished speaking. |
| `BRIDGE_REALTIME_MIN_OPUS_FRAMES` | `1` | Require at least this many frames before a silence can end the turn. |
- `TTS_HTTP_URL` (required for `TTS_PROVIDER=http` only)
- `OPENAI_API_KEY` — used when `STT_PROVIDER=openai` and/or `TTS_PROVIDER=openai` (separate from Anthropic; enables voice without extra services)

Bridge endpoints:

- `BRIDGE_HOST` (default: `0.0.0.0`)
- `BRIDGE_PORT` (default: `8000`)
- `BRIDGE_WEBSOCKET_URL` (what OTA sends to device, e.g. `ws://192.168.1.50:8000/ws`)
- `BRIDGE_WEBSOCKET_TOKEN` (default: `local-dev-token`)

## Run

```bash
cd local-bridge
source .venv/bin/activate
python -m bridge.server
```

## STT/TTS HTTP contract (for `*_PROVIDER=http`)

STT request:

```json
{
  "audio_format": "opus",
  "frames_base64": ["..."]
}
```

STT response:

```json
{
  "text": "transcribed utterance"
}
```

TTS request:

```json
{
  "text": "assistant response text",
  "audio_format": "opus"
}
```

TTS response:

```json
{
  "frames_base64": ["..."]
}
```

## Voice: wake word works but “Jarvis” does not answer your command

The bridge must turn **microphone audio** into **text** before it can call Claude.  
**Hearing the reply** also needs **TTS** that returns Opus **frames** (`TTS_PROVIDER=http` and a working `TTS_HTTP_URL`, e.g. bundled `tts_server.py`). Text-only `sentence_start` is not enough for the speaker.

- If **`STT_PROVIDER`** is **`none`** (or missing), the bridge **cannot** transcribe what you said after the wake word. It will not call the LLM with your command; you may see a **spoken/ subtitle error** telling you to configure STT.
- Set **`STT_PROVIDER=http`** and a working **`STT_HTTP_URL`** that implements the JSON contract in this README (Opus frames in → `text` out).
- **`GET /healthz`** now returns `"stt"` and `"tts"` so you can confirm what the running process is using.

Until STT is configured, **manual / text tests** can still use `POST /v1/chat` with JSON `{"text":"..."}` — that path does not need speech recognition.

## Troubleshooting: Anthropic `401` / unauthorized

The bridge sends `x-api-key` and `anthropic-version` to `https://api.anthropic.com/v1/messages` (see `bridge/providers.py`). A **401** means the API key was not accepted.

Check, in order:

1. **Key is an API key**, not a Claude.ai login. Create or copy it from [Anthropic Console](https://console.anthropic.com/) (API keys). It should look like `sk-ant-api03-...`.
2. **No extra characters** in `.env`: no space before/after `=`, no accidental line break in the middle of the key. After editing, restart `./run-bridge.sh`.
3. **Not revoked**: create a new key in the console if unsure.
4. **`ANTHROPIC_MODEL`**: use a model ID your org can access (see [Models](https://docs.anthropic.com/en/docs/about-claude/models)); wrong model usually returns **404**, not 401, but worth checking if the error body says otherwise.

Quick local test (replace with your key once):

```bash
curl -sS https://api.anthropic.com/v1/messages \
  -H "content-type: application/json" \
  -H "anthropic-version: 2023-06-01" \
  -H "x-api-key: $ANTHROPIC_API_KEY" \
  -d '{"model":"claude-sonnet-4-6","max_tokens":16,"messages":[{"role":"user","content":"ping"}]}'
```

You should get JSON with a `content` array, not `401`.

## Claude-only (no Ollama)

Set in `.env`:

```bash
LLM_PROVIDER=anthropic
LLM_ENABLE_FALLBACK=false
```

`OLLAMA_BASE_URL` is not required. You still need a valid `ANTHROPIC_API_KEY`.

## Switch back to Ollama-primary

```bash
cp ollama-primary.env.example .env
# edit paths and keys, then:
./run-bridge.sh
```

## Point the device to local OTA

Set `CONFIG_OTA_URL` in your firmware build (or `wifi.ota_url` in NVS) to:

`http://<your-lan-ip>:8000/xiaozhi/ota/`

Do **not** use `localhost`; the ESP32 must reach your machine over LAN.

## Smoke tests

```bash
cd local-bridge
source .venv/bin/activate
python -m unittest discover -s tests -v
```

These tests verify:

- OTA response structure
- WebSocket handshake and listen-detect text flow
- MCP `tools/list` and `tools/call` behavior
- Ollama + Anthropic response parsing and hybrid fallback behavior
- HTTP STT/TTS frame provider parsing behavior
- Binary websocket audio round-trip on listen start/stop

