import os
import tempfile
import unittest

from bridge.config import BridgeConfig


class PromptFileTests(unittest.TestCase):
    def test_system_prompt_from_file_overrides_env(self) -> None:
        with tempfile.NamedTemporaryFile(
            mode="w", suffix=".txt", delete=False, encoding="utf-8"
        ) as f:
            f.write("persona-from-file\n")
            path = f.name
        try:
            os.environ["BRIDGE_SYSTEM_PROMPT_FILE"] = path
            os.environ["BRIDGE_SYSTEM_PROMPT"] = "ignored-inline"
            cfg = BridgeConfig.from_env()
            self.assertEqual(cfg.system_prompt, "persona-from-file")
        finally:
            os.environ.pop("BRIDGE_SYSTEM_PROMPT_FILE", None)
            os.environ.pop("BRIDGE_SYSTEM_PROMPT", None)
            os.unlink(path)


if __name__ == "__main__":
    unittest.main()
