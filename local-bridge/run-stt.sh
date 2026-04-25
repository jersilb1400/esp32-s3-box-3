#!/usr/bin/env bash
# Start the local STT service (port 9001 by default). Use the same venv as the bridge.
set -euo pipefail
cd "$(dirname "$0")"
if [[ -f .venv/bin/activate ]]; then
  # shellcheck source=/dev/null
  source .venv/bin/activate
fi
export STT_LISTEN_HOST="${STT_LISTEN_HOST:-0.0.0.0}"
export STT_LISTEN_PORT="${STT_LISTEN_PORT:-9001}"
exec python stt_server.py
