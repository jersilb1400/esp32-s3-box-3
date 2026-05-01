from __future__ import annotations

import json
import threading
import unittest
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

from bridge.providers import (
    AnthropicProvider,
    HybridProvider,
    OllamaProvider,
    ProviderError,
)
from bridge.stt import HttpFrameSttProvider
from bridge.tts import HttpFrameTtsProvider


class _JsonHandler(BaseHTTPRequestHandler):
    routes: dict[str, tuple[int, dict]] = {}
    bodies: dict[str, dict] = {}

    def do_POST(self) -> None:  # noqa: N802
        length = int(self.headers.get("Content-Length", "0"))
        body = self.rfile.read(length).decode("utf-8")
        _JsonHandler.bodies[self.path] = json.loads(body) if body else {}
        status, payload = _JsonHandler.routes.get(self.path, (404, {"error": "missing"}))
        data = json.dumps(payload).encode("utf-8")
        self.send_response(status)
        self.send_header("Content-Type", "application/json")
        self.send_header("Content-Length", str(len(data)))
        self.end_headers()
        self.wfile.write(data)

    def log_message(self, format: str, *args) -> None:  # noqa: A003
        return


class ProviderTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls) -> None:
        cls.server = ThreadingHTTPServer(("127.0.0.1", 0), _JsonHandler)
        cls.thread = threading.Thread(target=cls.server.serve_forever, daemon=True)
        cls.thread.start()
        cls.base_url = f"http://127.0.0.1:{cls.server.server_port}"

    @classmethod
    def tearDownClass(cls) -> None:
        cls.server.shutdown()
        cls.server.server_close()
        cls.thread.join(timeout=2)

    def setUp(self) -> None:
        _JsonHandler.routes.clear()
        _JsonHandler.bodies.clear()

    def test_ollama_provider_parses_message_content(self) -> None:
        _JsonHandler.routes["/api/chat"] = (200, {"message": {"content": "hello from ollama"}})
        provider = OllamaProvider(self.base_url, "llama3.2:3b")

        text = provider.generate("system", "user")

        self.assertEqual(text, "hello from ollama")
        self.assertEqual(_JsonHandler.bodies["/api/chat"]["model"], "llama3.2:3b")

    def test_anthropic_provider_parses_content_blocks(self) -> None:
        _JsonHandler.routes["/v1/messages"] = (
            200,
            {"content": [{"type": "text", "text": "hello from anthropic"}]},
        )
        provider = AnthropicProvider(
            base_url=self.base_url,
            api_key="test-key",
            model="claude-sonnet-4-6",
            version="2023-06-01",
            max_tokens=64,
        )

        text = provider.generate("system", "user")

        self.assertEqual(text, "hello from anthropic")
        self.assertEqual(
            _JsonHandler.bodies["/v1/messages"]["model"], "claude-sonnet-4-6"
        )

    def test_hybrid_falls_back(self) -> None:
        class FailProvider:
            def generate(self, system_prompt: str, user_text: str) -> str:
                raise ProviderError("primary down")

        class OkProvider:
            def generate(self, system_prompt: str, user_text: str) -> str:
                return f"fallback:{user_text}"

        hybrid = HybridProvider(primary=FailProvider(), fallback=OkProvider(), enable_fallback=True)

        text = hybrid.generate("system", "ping")

        self.assertEqual(text, "fallback:ping")

    def test_hybrid_both_fail_includes_context(self) -> None:
        class Fail:
            def generate(self, system_prompt: str, user_text: str) -> str:
                raise ProviderError("primary down")

        hybrid = HybridProvider(primary=Fail(), fallback=Fail(), enable_fallback=True)
        with self.assertRaises(ProviderError) as ctx:
            hybrid.generate("system", "user")
        self.assertIn("Primary failed", str(ctx.exception))
        self.assertIn("Fallback also failed", str(ctx.exception))

    def test_http_stt_provider(self) -> None:
        _JsonHandler.routes["/stt"] = (200, {"text": "transcribed speech"})
        provider = HttpFrameSttProvider(f"{self.base_url}/stt")

        text = provider.transcribe([b"\x01\x02", b"\x03\x04"])

        self.assertEqual(text, "transcribed speech")
        self.assertEqual(
            _JsonHandler.bodies["/stt"]["audio_format"],
            "opus",
        )
        self.assertEqual(len(_JsonHandler.bodies["/stt"]["frames_base64"]), 2)

    def test_http_tts_provider(self) -> None:
        _JsonHandler.routes["/tts"] = (200, {"frames_base64": ["AQI=", "AwQ="]})
        provider = HttpFrameTtsProvider(f"{self.base_url}/tts")

        frames = provider.synthesize("hello")

        self.assertEqual(frames, [b"\x01\x02", b"\x03\x04"])
        self.assertEqual(_JsonHandler.bodies["/tts"]["audio_format"], "opus")


if __name__ == "__main__":
    unittest.main()

