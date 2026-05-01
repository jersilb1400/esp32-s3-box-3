#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
cd "$ROOT_DIR"

if [[ ! -d ".venv" ]]; then
  echo "Creating virtualenv..."
  python3 -m venv .venv
fi

source .venv/bin/activate
python -m pip install --upgrade pip >/dev/null
python -m pip install -r requirements.txt >/dev/null

if [[ ! -f ".env" ]]; then
  echo "Missing .env file."
  echo "Create it with: cp .env.example .env"
  exit 1
fi

set -a
source .env
set +a

required_vars=(BRIDGE_WEBSOCKET_URL STT_PROVIDER TTS_PROVIDER)
for var_name in "${required_vars[@]}"; do
  if [[ -z "${!var_name:-}" ]]; then
    echo "Required variable is empty: ${var_name}"
    exit 1
  fi
done

# Ollama is required when it is the primary or active fallback provider.
_ollama_needs_url=false
if [[ "${LLM_PROVIDER:-}" == "ollama" ]]; then
  _ollama_needs_url=true
fi
if [[ "${LLM_ENABLE_FALLBACK:-false}" == "true" && "${LLM_FALLBACK_PROVIDER:-}" == "ollama" ]]; then
  _ollama_needs_url=true
fi
if [[ "${_ollama_needs_url}" == "true" ]]; then
  if [[ -z "${OLLAMA_BASE_URL:-}" ]]; then
    echo "OLLAMA_BASE_URL is required when Ollama is primary or LLM fallback."
    exit 1
  fi
fi

# Anthropic (Claude API) is required when it is the primary or active fallback.
_anthropic_needs_key=false
if [[ "${LLM_PROVIDER:-}" == "anthropic" ]]; then
  _anthropic_needs_key=true
fi
if [[ "${LLM_ENABLE_FALLBACK:-false}" == "true" && "${LLM_FALLBACK_PROVIDER:-}" == "anthropic" ]]; then
  _anthropic_needs_key=true
fi
if [[ "${_anthropic_needs_key}" == "true" ]]; then
  if [[ -z "${ANTHROPIC_API_KEY:-}" || "${ANTHROPIC_API_KEY}" == "replace-me" ]]; then
    echo "ANTHROPIC_API_KEY is required when Anthropic (Claude API) is primary or LLM fallback."
    exit 1
  fi
fi

echo "Running smoke tests..."
python -m unittest discover -s tests -v

echo ""
echo "Starting local bridge..."
echo "Health URL: http://127.0.0.1:${BRIDGE_PORT:-8000}/healthz"
echo "OTA URL for device: http://<YOUR_LAN_IP>:${BRIDGE_PORT:-8000}/xiaozhi/ota/"
echo ""
python -m bridge.server
