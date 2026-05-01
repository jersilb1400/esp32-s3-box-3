from __future__ import annotations

import json
from dataclasses import dataclass
from typing import Protocol
from urllib import request
from urllib.error import HTTPError, URLError

from .config import BridgeConfig


class ProviderError(RuntimeError):
    """Raised when an LLM provider request fails."""


def _http_error_detail(exc: HTTPError) -> str:
    try:
        body = exc.read().decode("utf-8", errors="replace")
    except Exception:  # noqa: BLE001
        body = ""
    if body:
        if len(body) > 500:
            body = body[:500] + "..."
        return f"HTTP {exc.code} {exc.reason}: {body}"
    return f"HTTP {exc.code} {exc.reason}"


class ChatProvider(Protocol):
    def generate(self, system_prompt: str, user_text: str) -> str:
        ...


@dataclass
class OllamaProvider:
    base_url: str
    model: str
    timeout_s: float = 30.0

    def generate(self, system_prompt: str, user_text: str) -> str:
        payload = {
            "model": self.model,
            "stream": False,
            "messages": [
                {"role": "system", "content": system_prompt},
                {"role": "user", "content": user_text},
            ],
        }
        body = json.dumps(payload).encode("utf-8")
        req = request.Request(
            url=f"{self.base_url.rstrip('/')}/api/chat",
            data=body,
            headers={"Content-Type": "application/json"},
            method="POST",
        )
        try:
            with request.urlopen(req, timeout=self.timeout_s) as resp:
                data = json.loads(resp.read().decode("utf-8"))
        except HTTPError as exc:
            detail = _http_error_detail(exc)
            hint = ""
            if exc.code == 404:
                hint = (
                    f" (Is `ollama serve` running? Does `ollama list` include model "
                    f"\"{self.model}\"? Run: ollama pull {self.model})"
                )
            raise ProviderError(f"Ollama: {detail}{hint}") from exc
        except (URLError, TimeoutError) as exc:
            raise ProviderError(
                f"Ollama: cannot reach {self.base_url} ({exc!s}). Is Ollama running?"
            ) from exc

        message = data.get("message", {})
        if isinstance(message, dict) and isinstance(message.get("content"), str):
            return message["content"]
        if isinstance(data.get("response"), str):
            return data["response"]
        raise ProviderError("Ollama response missing message content")


@dataclass
class AnthropicProvider:
    base_url: str
    api_key: str
    model: str
    version: str
    max_tokens: int
    timeout_s: float = 30.0

    def generate(self, system_prompt: str, user_text: str) -> str:
        if not self.api_key:
            raise ProviderError("ANTHROPIC_API_KEY is required for Anthropic provider")

        payload = {
            "model": self.model,
            "max_tokens": self.max_tokens,
            "system": system_prompt,
            "messages": [{"role": "user", "content": user_text}],
        }
        body = json.dumps(payload).encode("utf-8")
        req = request.Request(
            url=f"{self.base_url.rstrip('/')}/v1/messages",
            data=body,
            headers={
                "Content-Type": "application/json",
                "x-api-key": self.api_key,
                "anthropic-version": self.version,
            },
            method="POST",
        )
        try:
            with request.urlopen(req, timeout=self.timeout_s) as resp:
                data = json.loads(resp.read().decode("utf-8"))
        except HTTPError as exc:
            raise ProviderError(f"Anthropic: {_http_error_detail(exc)}") from exc
        except (URLError, TimeoutError) as exc:
            raise ProviderError(f"Anthropic: request failed: {exc!s}") from exc

        content = data.get("content", [])
        text_blocks = [
            block.get("text", "")
            for block in content
            if isinstance(block, dict) and block.get("type") == "text"
        ]
        joined = "".join(text_blocks).strip()
        if not joined:
            raise ProviderError("Anthropic response missing text content")
        return joined


@dataclass
class HybridProvider:
    primary: ChatProvider
    fallback: ChatProvider | None = None
    enable_fallback: bool = True

    def generate(self, system_prompt: str, user_text: str) -> str:
        try:
            return self.primary.generate(system_prompt, user_text)
        except ProviderError as primary_err:
            if not self.enable_fallback or self.fallback is None:
                raise
            try:
                return self.fallback.generate(system_prompt, user_text)
            except ProviderError as fallback_err:
                raise ProviderError(
                    f"Primary failed ({primary_err!s}). "
                    f"Fallback also failed ({fallback_err!s}). "
                    f"To use only Anthropic, set LLM_ENABLE_FALLBACK=false. "
                    f"To fix Ollama, run: ollama serve && ollama pull <OLLAMA_MODEL>."
                ) from fallback_err


def provider_from_config(config: BridgeConfig) -> HybridProvider:
    primary_name = config.llm_provider.strip().lower()
    fallback_name = config.llm_fallback_provider.strip().lower()

    def build(name: str) -> ChatProvider:
        if name == "ollama":
            return OllamaProvider(config.ollama_base_url, config.ollama_model)
        if name == "anthropic":
            return AnthropicProvider(
                base_url=config.anthropic_base_url,
                api_key=config.anthropic_api_key,
                model=config.anthropic_model,
                version=config.anthropic_version,
                max_tokens=config.anthropic_max_tokens,
            )
        raise ValueError(f"Unsupported provider: {name}")

    primary = build(primary_name)
    fallback = None
    if fallback_name and fallback_name != primary_name:
        fallback = build(fallback_name)

    return HybridProvider(
        primary=primary,
        fallback=fallback,
        enable_fallback=config.enable_fallback,
    )

