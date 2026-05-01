from __future__ import annotations

import asyncio
import unittest

from aiohttp.test_utils import TestClient, TestServer

from bridge.config import BridgeConfig
from bridge.providers import HybridProvider
from bridge.server import create_app


class _StaticProvider:
    def generate(self, system_prompt: str, user_text: str) -> str:
        return f"assistant:{user_text}"


class _StaticStt:
    def transcribe(self, opus_frames: list[bytes]) -> str:
        if opus_frames:
            return "from-audio"
        return ""


class _StaticTts:
    def synthesize(self, text: str) -> list[bytes]:
        return [b"\x11\x22", b"\x33\x44"]


class ServerTests(unittest.IsolatedAsyncioTestCase):
    async def asyncSetUp(self) -> None:
        config = BridgeConfig(
            websocket_url="ws://127.0.0.1:8000/ws",
            websocket_token="local-dev-token",
            llm_provider="ollama",
            enable_fallback=False,
        )
        provider = HybridProvider(primary=_StaticProvider(), fallback=None, enable_fallback=False)
        app = create_app(
            config=config,
            provider=provider,
            stt_provider=_StaticStt(),
            tts_provider=_StaticTts(),
        )
        self.server = TestServer(app)
        self.client = TestClient(self.server)
        await self.client.start_server()

    async def asyncTearDown(self) -> None:
        await self.client.close()
        await self.server.close()

    async def test_ota_response_has_websocket_settings(self) -> None:
        resp = await self.client.post("/xiaozhi/ota/", json={"device": "esp32"})
        body = await resp.json()

        self.assertEqual(resp.status, 200)
        self.assertEqual(body["websocket"]["url"], "ws://127.0.0.1:8000/ws")
        self.assertEqual(body["websocket"]["token"], "local-dev-token")
        self.assertIn("server_time", body)

    async def test_websocket_hello_and_detect_flow(self) -> None:
        ws = await self.client.ws_connect("/ws")
        await ws.send_json({"type": "hello", "version": 1, "transport": "websocket"})
        hello = await ws.receive_json()
        self.assertEqual(hello["type"], "hello")
        self.assertEqual(hello["transport"], "websocket")

        await ws.send_json({"type": "listen", "state": "detect", "text": "turn on light"})
        stt = await ws.receive_json()
        tts_start = await ws.receive_json()
        tts_sentence = await ws.receive_json()
        _audio1 = await ws.receive()
        _audio2 = await ws.receive()
        tts_stop = await ws.receive_json()

        self.assertEqual(stt["type"], "stt")
        self.assertEqual(stt["text"], "turn on light")
        self.assertEqual(tts_start["type"], "tts")
        self.assertEqual(tts_start["state"], "start")
        self.assertEqual(tts_sentence["state"], "sentence_start")
        self.assertEqual(tts_sentence["text"], "assistant:turn on light")
        self.assertEqual(tts_stop["state"], "stop")
        await ws.close()

    async def test_binary_audio_roundtrip_after_listen_stop(self) -> None:
        ws = await self.client.ws_connect("/ws")
        await ws.send_json({"type": "hello", "version": 1, "transport": "websocket"})
        await ws.receive_json()

        await ws.send_json({"type": "listen", "state": "start", "mode": "manual"})
        await ws.send_bytes(b"\xaa\xbb")
        await ws.send_json({"type": "listen", "state": "stop", "mode": "manual"})

        stt = await ws.receive_json()
        tts_start = await ws.receive_json()
        tts_sentence = await ws.receive_json()
        audio1 = await ws.receive()
        audio2 = await ws.receive()
        tts_stop = await ws.receive_json()

        self.assertEqual(stt["type"], "stt")
        self.assertEqual(stt["text"], "from-audio")
        self.assertEqual(tts_start["state"], "start")
        self.assertEqual(tts_sentence["text"], "assistant:from-audio")
        self.assertEqual(audio1.type.name, "BINARY")
        self.assertEqual(audio1.data, b"\x11\x22")
        self.assertEqual(audio2.type.name, "BINARY")
        self.assertEqual(audio2.data, b"\x33\x44")
        self.assertEqual(tts_stop["state"], "stop")
        await ws.close()

    async def test_realtime_silence_triggers_utterance_without_device_stop(self) -> None:
        """ESP-Box + device AEC often never sends listen/stop; bridge ends utterance on silence."""
        config = BridgeConfig(
            websocket_url="ws://127.0.0.1:8000/ws",
            websocket_token="local-dev-token",
            llm_provider="ollama",
            enable_fallback=False,
            realtime_silence_end_ms=50,
            realtime_silence_min_opus_frames=1,
            enable_realtime_silence_eou=True,
        )
        provider = HybridProvider(primary=_StaticProvider(), fallback=None, enable_fallback=False)
        app = create_app(
            config=config,
            provider=provider,
            stt_provider=_StaticStt(),
            tts_provider=_StaticTts(),
        )
        server = TestServer(app)
        client = TestClient(server)
        await client.start_server()
        try:
            ws = await client.ws_connect("/ws")
            await ws.send_json({"type": "hello", "version": 1, "transport": "websocket"})
            await ws.receive_json()

            await ws.send_json({"type": "listen", "state": "start", "mode": "realtime"})
            await ws.send_bytes(b"\xaa\xbb")
            # No listen/stop — silence watcher should run STT after ~50ms.
            await asyncio.sleep(0.12)

            stt = await ws.receive_json()
            tts_start = await ws.receive_json()
            tts_sentence = await ws.receive_json()
            _ = await ws.receive()
            _ = await ws.receive()
            tts_stop = await ws.receive_json()

            self.assertEqual(stt["type"], "stt")
            self.assertEqual(stt["text"], "from-audio")
            self.assertEqual(tts_start["state"], "start")
            self.assertEqual(tts_sentence["text"], "assistant:from-audio")
            self.assertEqual(tts_stop["state"], "stop")
            await ws.close()
        finally:
            await client.close()
            await server.close()

    async def test_manual_mode_waits_for_explicit_stop_not_silence(self) -> None:
        config = BridgeConfig(
            websocket_url="ws://127.0.0.1:8000/ws",
            websocket_token="local-dev-token",
            llm_provider="ollama",
            enable_fallback=False,
            realtime_silence_end_ms=30,
            enable_realtime_silence_eou=True,
        )
        provider = HybridProvider(primary=_StaticProvider(), fallback=None, enable_fallback=False)
        app = create_app(
            config=config,
            provider=provider,
            stt_provider=_StaticStt(),
            tts_provider=_StaticTts(),
        )
        server = TestServer(app)
        client = TestClient(server)
        await client.start_server()
        try:
            ws = await client.ws_connect("/ws")
            await ws.send_json({"type": "hello", "version": 1, "transport": "websocket"})
            await ws.receive_json()
            await ws.send_json({"type": "listen", "state": "start", "mode": "manual"})
            await ws.send_bytes(b"\xaa\xbb")
            with self.assertRaises(asyncio.TimeoutError):
                await asyncio.wait_for(ws.receive(), 0.15)
            await ws.send_json({"type": "listen", "state": "stop", "mode": "manual"})
            stt = await ws.receive_json()
            self.assertEqual(stt["type"], "stt")
            for _ in range(5):
                await ws.receive()
            await ws.close()
        finally:
            await client.close()
            await server.close()

    async def test_mcp_tools_call(self) -> None:
        ws = await self.client.ws_connect("/ws")
        await ws.send_json({"type": "mcp", "payload": {"jsonrpc": "2.0", "method": "tools/list", "id": 1}})
        tools_msg = await ws.receive_json()
        tools = tools_msg["payload"]["result"]["tools"]
        self.assertEqual(tools[0]["name"], "local.bridge.chat")

        await ws.send_json(
            {
                "type": "mcp",
                "payload": {
                    "jsonrpc": "2.0",
                    "method": "tools/call",
                    "params": {"name": "local.bridge.chat", "arguments": {"text": "status"}},
                    "id": 2,
                },
            }
        )
        call_msg = await ws.receive_json()
        text = call_msg["payload"]["result"]["content"][0]["text"]
        self.assertEqual(text, "assistant:status")
        await ws.close()


if __name__ == "__main__":
    unittest.main()

