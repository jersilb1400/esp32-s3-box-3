from __future__ import annotations

import os
from dataclasses import dataclass
from pathlib import Path


def _env_bool(name: str, default: bool) -> bool:
    raw = os.getenv(name)
    if raw is None:
        return default
    return raw.strip().lower() in {"1", "true", "yes", "on"}


def _system_prompt_from_env(default: str) -> str:
    path_raw = os.getenv("BRIDGE_SYSTEM_PROMPT_FILE", "").strip().strip('"').strip("'")
    if path_raw:
        p = Path(path_raw).expanduser()
        if p.is_file():
            return p.read_text(encoding="utf-8").strip()
    raw = os.getenv("BRIDGE_SYSTEM_PROMPT", default)
    return raw.strip().strip('"').strip("'") if raw else default


@dataclass(frozen=True)
class BridgeConfig:
    host: str = "0.0.0.0"
    port: int = 8000
    websocket_url: str = "ws://127.0.0.1:8000/ws"
    websocket_token: str = "local-dev-token"
    websocket_version: int = 1
    server_sample_rate: int = 24000
    server_frame_duration: int = 60

    llm_provider: str = "anthropic"
    llm_fallback_provider: str = "ollama"
    # Default off: Ollama is often not running; 404 on fallback confuses new setups.
    enable_fallback: bool = False

    ollama_base_url: str = "http://127.0.0.1:11434"
    ollama_model: str = "llama3.2:3b"

    anthropic_base_url: str = "https://api.anthropic.com"
    anthropic_api_key: str = ""
    # Use a current API id from https://docs.anthropic.com/en/docs/about-claude/models (3.5 snapshots are retired).
    anthropic_model: str = "claude-sonnet-4-6"
    anthropic_version: str = "2023-06-01"
    anthropic_max_tokens: int = 512

    system_prompt: str = (
        "You are a concise assistant for an ESP32 voice device."
    )
    openclaude_skill_url: str = ""
    openclaude_api_key: str = ""
    # Used when STT_PROVIDER=openai / TTS_PROVIDER=openai (in-process, no stt_server/tts_server)
    openai_api_key: str = ""

    stt_provider: str = "none"
    stt_http_url: str = ""
    stt_http_api_key: str = ""
    stt_timeout_s: float = 30.0

    tts_provider: str = "none"
    tts_http_url: str = ""
    tts_http_api_key: str = ""
    tts_timeout_s: float = 30.0

    # When non-empty, utterances that do NOT contain this word (case-insensitive) are discarded.
    # Prevents responding to nearby conversations not addressed to the assistant.
    # Example: set to "jarvis" so only speech containing "jarvis" is processed.
    address_filter_word: str = ""

    # ESP-Box with device AEC uses listen mode "realtime"; the device often never sends
    # listen/state=stop. After this many seconds with no new Opus frame, run STT (like stop).
    realtime_silence_end_ms: int = 1000
    realtime_silence_min_opus_frames: int = 1
    enable_realtime_silence_eou: bool = True

    @classmethod
    def from_env(cls) -> "BridgeConfig":
        return cls(
            host=os.getenv("BRIDGE_HOST", "0.0.0.0"),
            port=int(os.getenv("BRIDGE_PORT", os.getenv("PORT", "8000"))),
            websocket_url=os.getenv("BRIDGE_WEBSOCKET_URL", "ws://127.0.0.1:8000/ws"),
            websocket_token=os.getenv("BRIDGE_WEBSOCKET_TOKEN", "local-dev-token"),
            websocket_version=int(os.getenv("BRIDGE_WEBSOCKET_VERSION", "1")),
            server_sample_rate=int(os.getenv("BRIDGE_SERVER_SAMPLE_RATE", "24000")),
            server_frame_duration=int(os.getenv("BRIDGE_SERVER_FRAME_DURATION", "60")),
            llm_provider=os.getenv("LLM_PROVIDER", "anthropic"),
            llm_fallback_provider=os.getenv("LLM_FALLBACK_PROVIDER", "ollama"),
            enable_fallback=_env_bool("LLM_ENABLE_FALLBACK", False),
            ollama_base_url=os.getenv("OLLAMA_BASE_URL", "http://127.0.0.1:11434"),
            ollama_model=os.getenv("OLLAMA_MODEL", "llama3.2:3b"),
            anthropic_base_url=os.getenv("ANTHROPIC_BASE_URL", "https://api.anthropic.com"),
            # Strip quotes/whitespace — common 401 cause when pasting into .env
            anthropic_api_key=os.getenv("ANTHROPIC_API_KEY", "").strip().strip('"').strip("'"),
            anthropic_model=os.getenv("ANTHROPIC_MODEL", "claude-sonnet-4-6"),
            anthropic_version=os.getenv("ANTHROPIC_VERSION", "2023-06-01"),
            anthropic_max_tokens=int(os.getenv("ANTHROPIC_MAX_TOKENS", "512")),
            system_prompt=_system_prompt_from_env(
                "You are a concise assistant for an ESP32 voice device."
            ),
            openclaude_skill_url=os.getenv("OPENCLAUDE_SKILL_URL", ""),
            openclaude_api_key=os.getenv("OPENCLAUDE_API_KEY", ""),
            openai_api_key=os.getenv("OPENAI_API_KEY", "").strip().strip('"').strip("'"),
            stt_provider=os.getenv("STT_PROVIDER", "none"),
            stt_http_url=os.getenv("STT_HTTP_URL", ""),
            stt_http_api_key=os.getenv("STT_HTTP_API_KEY", ""),
            stt_timeout_s=float(os.getenv("STT_TIMEOUT_S", "30")),
            tts_provider=os.getenv("TTS_PROVIDER", "none"),
            tts_http_url=os.getenv("TTS_HTTP_URL", ""),
            tts_http_api_key=os.getenv("TTS_HTTP_API_KEY", ""),
            tts_timeout_s=float(os.getenv("TTS_TIMEOUT_S", "30")),
            address_filter_word=os.getenv("BRIDGE_ADDRESS_FILTER", "").strip().strip('"').strip("'").lower(),
            realtime_silence_end_ms=int(os.getenv("BRIDGE_REALTIME_SILENCE_MS", "1000")),
            realtime_silence_min_opus_frames=int(
                os.getenv("BRIDGE_REALTIME_MIN_OPUS_FRAMES", "1")
            ),
            enable_realtime_silence_eou=_env_bool("BRIDGE_REALTIME_SILENCE_EOU", True),
        )

