# pyright: reportAttributeAccessIssue=false
import unittest
from pathlib import Path

import build
from tools.build.build_lhm_plugin import LHM_PINNED_FILE_SHA256


class ElevationBuildTest(unittest.TestCase):
    def test_privileged_runtime_hashes_match_build_pins(self):
        runtime_path = Path(build.PROJECT_ROOT) / "captureengine" / "elevation" / "elevation_runtime.cpp"
        source = runtime_path.read_text(encoding="utf-8")
        for name, digest in LHM_PINNED_FILE_SHA256.items():
            self.assertIn(name, source)
            self.assertIn(digest, source)

    def test_service_is_required_by_sanitizer_stage_cache(self):
        outputs = {Path(name).name for name in build.sanitizer_stage_outputs()}
        self.assertIn("captureengine_elevation_service.exe", outputs)

    def test_elevation_parser_is_registered_with_committed_seeds(self):
        corpus = build.FUZZ_TARGET_CORPUS["fuzz_elevation_protocol.cpp"]
        seed_directory = Path(build.PROJECT_ROOT) / "tests" / "fuzz" / "corpus" / corpus
        self.assertGreaterEqual(len(list(seed_directory.iterdir())), 6)
