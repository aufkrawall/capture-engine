"""Keep every capture-audit regression group reachable from its CLI self-test."""

from __future__ import annotations

import importlib.util
import sys
import unittest
from pathlib import Path
from unittest.mock import patch


class CaptureAVSelfTestDispatchTest(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        source = Path(__file__).resolve().parents[1] / "analysis" / "analyze_capture_av.py"
        spec = importlib.util.spec_from_file_location("capture_av_dispatch_probe", source)
        assert spec is not None and spec.loader is not None
        cls.analyzer = importlib.util.module_from_spec(spec)
        sys.modules[spec.name] = cls.analyzer
        spec.loader.exec_module(cls.analyzer)

    def test_cli_self_test_reaches_each_regression_group(self):
        for group in ("sessions", "encoder", "syncdelay", "audio", "correlation"):
            with self.subTest(group=group):
                marker = f"reached_{group}"
                with patch.object(self.analyzer, f"_self_test_{group}", side_effect=RuntimeError(marker)):
                    with self.assertRaisesRegex(RuntimeError, marker):
                        self.analyzer.self_test()


if __name__ == "__main__":
    unittest.main()
