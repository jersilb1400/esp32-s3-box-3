from __future__ import annotations

import asyncio
import contextlib
import json
import logging
import re
import time
import uuid
from dataclasses import asdict
from datetime import datetime
from typing import Any

from aiohttp import web, WSMsgType

from .config import BridgeConfig
from .openclaude import OpenClaudeSkillClient
from .providers import HybridProvider, ProviderError, provider_from_config
from .openai_audio import OpenAiFrameSttProvider, OpenAiFrameTtsProvider
from .stt import HttpFrameSttProvider, NullSttProvider, SttError, SttProvider
from .tts import HttpFrameTtsProvider, NullTtsProvider, TtsError, TtsProvider

logger = logging.getLogger(__name__)

BRIDGE_CONFIG_KEY: web.AppKey[dict[str, Any]] = web.AppKey("bridge_config", dict)


def _plain_text_for_tts(assistant_text: str) -> str:
    """Strip common markdown so TTS APIs get speakable text (display still uses full reply)."""
    t = re.sub(r"\*\*([^*]+)\*\*", r"\1", assistant_text)
    t = re.sub(r"[*_`#]+|^\s*[-*]\s+", " ", t, flags=re.MULTILINE)
    t = re.sub(r"\s+", " ", t).strip()
    return t if t else assistant_text


def _timezone_offset_minutes() -> int:
    # JavaScript-style offset minutes east/west from UTC.
    now = datetime.now().astimezone()
    offset = now.utcoffset()
    if offset is None:
        return 0
    return int(offset.total_seconds() // 60)


def _jsonrpc_ok(msg_id: Any, result: dict[str, Any]) -> dict[str, Any]:
    return {"jsonrpc": "2.0", "id": msg_id, "result": result}


def _jsonrpc_error(msg_id: Any, code: int, message: str) -> dict[str, Any]:
    return {"jsonrpc": "2.0", "id": msg_id, "error": {"code": code, "message": message}}


class BridgeApp:
    def __init__(
        self,
        config: BridgeConfig,
        provider: HybridProvider,
        openclaude: OpenClaudeSkillClient | None = None,
        stt_provider: SttProvider | None = None,
        tts_provider: TtsProvider | None = None,
    ) -> None:
        self.config = config
        self.provider = provider
        self.openclaude = openclaude
        self.stt = stt_provider or _stt_from_config(config)
        self.tts = tts_provider or _tts_from_config(config)

    async def health(self, _: web.Request) -> web.Response:
        h: dict[str, Any] = {
            "ok": True,
            "provider": self.config.llm_provider,
            "stt": self.config.stt_provider,
            "tts": self.config.tts_provider,
            "realtime_silence_eou": self.config.enable_realtime_silence_eou,
            "realtime_silence_ms": self.config.realtime_silence_end_ms,
        }
        if self.config.stt_provider.strip().lower() == "openai" or self.config.tts_provider.strip().lower() == "openai":
            h["openai_key_configured"] = bool(self.config.openai_api_key)
        return web.json_response(h)

    async def ota(self, _: web.Request) -> web.Response:
        payload = {
            "websocket": {
                "url": self.config.websocket_url,
                "token": self.config.websocket_token,
                "version": self.config.websocket_version,
            },
            "server_time": {
                "timestamp": int(time.time() * 1000),
                "timezone_offset": _timezone_offset_minutes(),
            },
        }
        return web.json_response(payload)

    async def chat(self, request: web.Request) -> web.Response:
        data = await request.json()
        text = str(data.get("text", "")).strip()
        if not text:
            return web.json_response({"error": "text is required"}, status=400)
        reply = self._generate_reply(text, session_id=str(data.get("session_id", "")))
        return web.json_response({"text": reply})

    def _generate_reply(self, text: str, session_id: str) -> str:
        if self.openclaude is not None:
            result = self.openclaude.call({"text": text, "session_id": session_id})
            value = result.get("text")
            if isinstance(value, str) and value.strip():
                return value.strip()
        return self.provider.generate(self.config.system_prompt, text)

    def _cancel_silence_task(self, s: dict[str, Any]) -> None:
        t = s.get("silence_task")
        if t is not None and not t.done():
            t.cancel()
        s["silence_task"] = None

    def _silence_eou_applies(self, listen_mode: str) -> bool:
        if not self.config.enable_realtime_silence_eou:
            return False
        m = listen_mode.strip().lower()
        if m == "manual":
            return False
        return True

    async def _maybe_arm_silence_eou(
        self, ws: web.WebSocketResponse, session_id: str, s: dict[str, Any]
    ) -> None:
        if not s.get("listening"):
            return
        if not self._silence_eou_applies(str(s.get("listen_mode", ""))):
            return
        if self.config.realtime_silence_end_ms <= 0:
            return
        self._cancel_silence_task(s)
        s["silence_task"] = asyncio.create_task(
            self._realtime_silence_waiter(ws, session_id, s)
        )

    async def _realtime_silence_waiter(
        self, ws: web.WebSocketResponse, session_id: str, s: dict[str, Any]
    ) -> None:
        delay = self.config.realtime_silence_end_ms / 1000.0
        try:
            await asyncio.sleep(delay)
        except asyncio.CancelledError:
            return
        if not s.get("listening"):
            return
        if not self._silence_eou_applies(str(s.get("listen_mode", ""))):
            return
        frames: list[bytes] = s["captured_opus_frames"]
        if len(frames) < self.config.realtime_silence_min_opus_frames:
            return
        n = len(frames)
        logger.info(
            "End of utterance (silence %.0f ms, %d opus frame(s), mode=%r)",
            delay * 1000,
            n,
            s.get("listen_mode", ""),
        )
        # This coroutine is s["silence_task"]; do not call cancel on ourselves.
        s["silence_task"] = None
        await self._process_utterance(
            ws=ws,
            session_id=session_id,
            captured_opus_frames=frames,
            fallback_text=str(s.get("pending_detect_text", "")),
        )
        s["listening"] = False
        s["pending_detect_text"] = ""
        frames.clear()

    async def websocket(self, request: web.Request) -> web.WebSocketResponse:
        logger.info("WebSocket /ws from %s", request.remote)
        ws = web.WebSocketResponse(heartbeat=20)
        await ws.prepare(request)
        session_id = str(uuid.uuid4())
        ws_state: dict[str, Any] = {
            "listening": False,
            "listen_mode": "",
            "captured_opus_frames": [],
            "pending_detect_text": "",
            "silence_task": None,
        }

        try:
            async for msg in ws:
                if msg.type == WSMsgType.TEXT:
                    await self._handle_text_message(
                        ws=ws, session_id=session_id, text=msg.data, s=ws_state
                    )
                elif msg.type == WSMsgType.BINARY:
                    if ws_state["listening"]:
                        ws_state["captured_opus_frames"].append(msg.data)
                        await self._maybe_arm_silence_eou(ws, session_id, ws_state)
                elif msg.type in {WSMsgType.CLOSE, WSMsgType.CLOSED, WSMsgType.ERROR}:
                    break
        finally:
            with contextlib.suppress(Exception):
                self._cancel_silence_task(ws_state)
        return ws

    async def _handle_text_message(
        self,
        ws: web.WebSocketResponse,
        session_id: str,
        text: str,
        s: dict[str, Any],
    ) -> None:
        try:
            data = json.loads(text)
        except json.JSONDecodeError:
            await ws.send_json({"type": "error", "message": "invalid JSON"})
            return

        msg_type = data.get("type")
        if msg_type == "hello":
            await ws.send_json(
                {
                    "type": "hello",
                    "transport": "websocket",
                    "session_id": session_id,
                    "audio_params": {
                        "format": "opus",
                        "sample_rate": self.config.server_sample_rate,
                        "channels": 1,
                        "frame_duration": self.config.server_frame_duration,
                    },
                }
            )
            return

        if msg_type == "listen":
            state = data.get("state")
            mode = str(data.get("mode", "")).strip()
            if state == "start":
                self._cancel_silence_task(s)
                s["captured_opus_frames"].clear()
                s["pending_detect_text"] = ""
                s["listen_mode"] = mode
                s["listening"] = True
            elif state == "stop":
                self._cancel_silence_task(s)
                if s["listening"]:
                    await self._process_utterance(
                        ws=ws,
                        session_id=session_id,
                        captured_opus_frames=s["captured_opus_frames"],
                        fallback_text=s["pending_detect_text"],
                    )
                s["listening"] = False
                s["pending_detect_text"] = ""
                s["captured_opus_frames"].clear()
            if state == "detect":
                prompt = str(data.get("text", "")).strip()
                if prompt:
                    s["pending_detect_text"] = prompt
                if not s["listening"] and prompt:
                    self._cancel_silence_task(s)
                    await self._process_utterance(
                        ws=ws,
                        session_id=session_id,
                        captured_opus_frames=s["captured_opus_frames"],
                        fallback_text=prompt,
                    )
                    s["pending_detect_text"] = ""
                    s["captured_opus_frames"].clear()
            return

        if msg_type == "mcp":
            payload = data.get("payload", {})
            response_payload = self._handle_mcp_payload(payload, session_id=session_id)
            await ws.send_json(
                {"session_id": session_id, "type": "mcp", "payload": response_payload}
            )
            return

    async def _process_utterance(
        self,
        ws: web.WebSocketResponse,
        session_id: str,
        captured_opus_frames: list[bytes],
        fallback_text: str,
    ) -> None:
        prompt = ""
        try:
            prompt = self.stt.transcribe(captured_opus_frames)
        except SttError as exc:
            logger.warning("STT error: %s", exc)
            prompt = ""
        if not prompt:
            prompt = fallback_text.strip()
        if not prompt:
            # Most common: STT not configured (none) or live HTTP STT not returning text,
            # while listen/start cleared wake-word text — nothing left to send to the LLM.
            if captured_opus_frames:
                msg = (
                    "I did not get any transcribed text from your speech. "
                    "Set STT_PROVIDER=openai and OPENAI_API_KEY, or STT_PROVIDER=http "
                    "with a working STT_HTTP_URL. See local-bridge README."
                )
                logger.warning(
                    "No transcript: %d opus frames, stt=%s, fallback empty",
                    len(captured_opus_frames),
                    self.config.stt_provider,
                )
            else:
                msg = "No speech audio was received. Try speaking again, or use push-to-talk."
                logger.warning("No transcript: zero opus frames")
            await self._send_tts_text_only(ws, session_id, msg)
            return

        await ws.send_json({"session_id": session_id, "type": "stt", "text": prompt})
        try:
            reply = self._generate_reply(prompt, session_id=session_id)
        except ProviderError as exc:
            reply = f"Provider error: {exc}"

        await ws.send_json({"session_id": session_id, "type": "tts", "state": "start"})
        await ws.send_json(
            {
                "session_id": session_id,
                "type": "tts",
                "state": "sentence_start",
                "text": reply,
            }
        )
        speak_text = _plain_text_for_tts(reply)
        try:
            frames = self.tts.synthesize(speak_text)
        except TtsError as exc:
            logger.warning("TTS error (no audio): %s", exc)
            frames = []
        if not frames and self.config.tts_provider.strip().lower() not in {"", "none"}:
            logger.warning(
                "TTS returned 0 Opus frames (device will be silent). "
                "Check TTS service logs, OPENAI_API_KEY, and curl http://127.0.0.1:9002/healthz"
            )
        elif not frames:
            logger.warning(
                "TTS_PROVIDER is none: no audio; set TTS_PROVIDER=http and run tts_server.py. "
                "Screen may still show the assistant text."
            )
        else:
            logger.info("TTS: sending %d opus frame(s) to device", len(frames))
        for frame in frames:
            await ws.send_bytes(frame)
        # Let the device main loop run HandleStateChanged and drain decode/playback before
        # tts "stop" flips to listening; avoids ResetDecoder(EnableVoiceProcessing) racing
        # the end of a long reply. Keep short to avoid slow interactions.
        await asyncio.sleep(0.12)
        await ws.send_json({"session_id": session_id, "type": "tts", "state": "stop"})

    async def _send_tts_text_only(
        self, ws: web.WebSocketResponse, session_id: str, text: str
    ) -> None:
        """Speakable guidance when we cannot run the full LLM path (e.g. missing STT)."""
        await ws.send_json({"session_id": session_id, "type": "tts", "state": "start"})
        await ws.send_json(
            {
                "session_id": session_id,
                "type": "tts",
                "state": "sentence_start",
                "text": text,
            }
        )
        try:
            frames = self.tts.synthesize(text)
        except TtsError as exc:
            logger.warning("TTS error on error-path message: %s", exc)
            frames = []
        for frame in frames:
            await ws.send_bytes(frame)
        await asyncio.sleep(0.12)
        await ws.send_json({"session_id": session_id, "type": "tts", "state": "stop"})

    def _handle_mcp_payload(self, payload: dict[str, Any], session_id: str) -> dict[str, Any]:
        method = payload.get("method")
        msg_id = payload.get("id")

        if method == "initialize":
            return _jsonrpc_ok(
                msg_id,
                {
                    "protocolVersion": "2024-11-05",
                    "capabilities": {"tools": {}},
                    "serverInfo": {"name": "local-bridge", "version": "0.1.0"},
                },
            )

        if method == "tools/list":
            tool = {
                "name": "local.bridge.chat",
                "description": "Send text to configured hybrid LLM provider",
                "inputSchema": {
                    "type": "object",
                    "properties": {
                        "text": {"type": "string", "description": "Prompt text"}
                    },
                    "required": ["text"],
                },
            }
            return _jsonrpc_ok(msg_id, {"tools": [tool], "nextCursor": ""})

        if method == "tools/call":
            params = payload.get("params", {})
            tool_name = params.get("name")
            if tool_name != "local.bridge.chat":
                return _jsonrpc_error(msg_id, -32601, f"Unknown tool: {tool_name}")
            args = params.get("arguments", {})
            text = str(args.get("text", "")).strip()
            if not text:
                return _jsonrpc_error(msg_id, -32602, "Missing argument: text")
            try:
                reply = self._generate_reply(text, session_id=session_id)
            except ProviderError as exc:
                return _jsonrpc_error(msg_id, -32000, f"Provider failure: {exc}")
            return _jsonrpc_ok(
                msg_id,
                {"content": [{"type": "text", "text": reply}], "isError": False},
            )

        return _jsonrpc_error(msg_id, -32601, f"Unsupported method: {method}")


def create_app(
    config: BridgeConfig | None = None,
    provider: HybridProvider | None = None,
    openclaude: OpenClaudeSkillClient | None = None,
    stt_provider: SttProvider | None = None,
    tts_provider: TtsProvider | None = None,
) -> web.Application:
    cfg = config or BridgeConfig.from_env()
    llm_provider = provider or provider_from_config(cfg)
    bridge = BridgeApp(
        cfg,
        llm_provider,
        openclaude,
        stt_provider=stt_provider,
        tts_provider=tts_provider,
    )

    app = web.Application()
    app[BRIDGE_CONFIG_KEY] = asdict(cfg)
    app.router.add_get("/healthz", bridge.health)
    app.router.add_post("/xiaozhi/ota/", bridge.ota)
    app.router.add_post("/v1/chat", bridge.chat)
    app.router.add_get("/ws", bridge.websocket)
    return app


def _stt_from_config(config: BridgeConfig) -> SttProvider:
    provider_name = config.stt_provider.strip().lower()
    if provider_name in {"", "none"}:
        return NullSttProvider()
    if provider_name == "openai":
        return OpenAiFrameSttProvider(api_key=config.openai_api_key, timeout_s=config.stt_timeout_s)
    if provider_name == "http":
        return HttpFrameSttProvider(
            url=config.stt_http_url,
            api_key=config.stt_http_api_key,
            timeout_s=config.stt_timeout_s,
        )
    raise ValueError(f"Unsupported STT provider: {config.stt_provider}")


def _tts_from_config(config: BridgeConfig) -> TtsProvider:
    provider_name = config.tts_provider.strip().lower()
    if provider_name in {"", "none"}:
        return NullTtsProvider()
    if provider_name == "openai":
        return OpenAiFrameTtsProvider(api_key=config.openai_api_key, timeout_s=config.tts_timeout_s)
    if provider_name == "http":
        return HttpFrameTtsProvider(
            url=config.tts_http_url,
            api_key=config.tts_http_api_key,
            timeout_s=config.tts_timeout_s,
        )
    raise ValueError(f"Unsupported TTS provider: {config.tts_provider}")


def main() -> None:
    logging.basicConfig(
        level=logging.INFO, format="%(asctime)s %(levelname)s %(name)s: %(message)s"
    )
    config = BridgeConfig.from_env()
    stt_l = config.stt_provider.strip().lower()
    tts_l = config.tts_provider.strip().lower()
    if stt_l in {"", "none"}:
        logger.warning(
            "STT_PROVIDER is none/empty: wake-word + listen will not produce text for "
            "Claude without speech-to-text. Set STT_PROVIDER=openai with OPENAI_API_KEY, "
            "or STT_PROVIDER=http and STT_HTTP_URL (see README)."
        )
    if stt_l == "openai" and not config.openai_api_key:
        logger.error("STT_PROVIDER=openai but OPENAI_API_KEY is empty.")
    if tts_l == "openai" and not config.openai_api_key:
        logger.error("TTS_PROVIDER=openai but OPENAI_API_KEY is empty (no audio).")
    app = create_app(config=config)
    web.run_app(app, host=config.host, port=config.port)


if __name__ == "__main__":
    main()

