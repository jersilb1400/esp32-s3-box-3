# Deploying the Jarvis Bridge to Fly.io

The bridge is a Python aiohttp WebSocket server.  Fly.io runs it in a persistent
VM so your ESP-BOX-3 can reach Jarvis from anywhere — not just your home network.

---

## Prerequisites

Install the Fly CLI once:

```bash
brew install flyctl
fly auth login          # opens browser, sign up if you haven't
```

---

## Step 1 — Create the app

```bash
cd local-bridge

# "jarvis-bridge" must be globally unique on Fly.io — pick your own name.
fly launch \
  --name jarvis-bridge \
  --region ord \          # Chicago; change to: lax, iad, dfw, mia, etc.
  --no-deploy             # we'll deploy manually after setting secrets
```

Accept all defaults when prompted.  This creates the app and writes
`fly.toml` if it doesn't exist yet (it already does in this repo).

---

## Step 2 — Set secrets

These are injected as environment variables at runtime — they never appear
in the Docker image or in `fly.toml`.

```bash
fly secrets set \
  ANTHROPIC_API_KEY="sk-ant-..." \
  OPENAI_API_KEY="sk-..."
```

Optional extras (only needed if you use non-default providers):
```bash
fly secrets set OLLAMA_BASE_URL="..."
```

Verify what's stored (values are hidden):
```bash
fly secrets list
```

---

## Step 3 — Deploy

```bash
fly deploy
```

Fly builds the Docker image, pushes it, and starts the VM.  First deploy
takes ~2 minutes; subsequent deploys ~30 seconds.

Watch logs live during startup:
```bash
fly logs
```

---

## Step 4 — Get your WebSocket URL

```bash
fly status
```

Your app URL will be `https://jarvis-bridge.fly.dev`.

The WebSocket endpoint the ESP-BOX-3 connects to is:
```
wss://jarvis-bridge.fly.dev/ws
```

Health check (should return `{"status":"ok"}`):
```bash
curl https://jarvis-bridge.fly.dev/healthz
```

---

## Step 5 — Configure the firmware

Update `sdkconfig.defaults` (or run `idf.py menuconfig` → Xiaozhi Assistant
→ OTA URL / websocket settings) to point the device at your Fly.io server.

The relevant env var that the OTA provisioning flow reads is set in your
device's Wi-Fi provision step.  Alternatively, hard-code it in your board's
`config.json` or via the web provisioning UI.

The device sends:
```
Authorization: Bearer local-dev-token
```

Change `BRIDGE_WEBSOCKET_TOKEN` in `fly.toml` `[env]` (or `fly secrets set
BRIDGE_WEBSOCKET_TOKEN=your-token`) and update the firmware's
`CONFIG_WEBSOCKET_TOKEN` accordingly.

---

## Step 6 — Scale to zero when idle (optional, saves money)

```bash
fly scale count 0   # stops VM; Fly cold-starts it on first connection
fly scale count 1   # always on ($2–5/month for 512 MB shared)
```

Cold-start latency is ~3–5 seconds for the first request after scale-to-zero.
For a voice device you probably want `count 1` (always on).

---

## Updating

```bash
fly deploy           # rebuilds image and hot-swaps the VM
```

Zero-downtime rolling deploy by default.

---

## Troubleshooting

| Symptom | Fix |
|---------|-----|
| `fly logs` shows `ANTHROPIC_API_KEY` missing | Run `fly secrets set ANTHROPIC_API_KEY=...` |
| Health check fails | Check `fly logs` — opuslib install error means `libopus0` is missing from Dockerfile |
| WebSocket connects but no audio response | Confirm `STT_PROVIDER=openai` and `OPENAI_API_KEY` is set |
| Device can't reach the server | Ensure device firmware uses `wss://` not `ws://`; Fly.io requires TLS on port 443 |
