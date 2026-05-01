#!/usr/bin/env bash
# Text-to-speech for the bridge (port 9002 by default). Needs OPENAI_API_KEY in .env.
set -euo pipefail
cd "$(dirname "$0")"
if [[ -f .venv/bin/activate ]]; then
  # shellcheck source=/dev/null
  source .venv/bin/activate
fi
export TTS_LISTEN_HOST="${TTS_LISTEN_HOST:-0.0.0.0}"
export TTS_LISTEN_PORT="${TTS_LISTEN_PORT:-9002}"
exec python tts_server.py
