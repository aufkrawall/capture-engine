"""Tests for exact-input verification-stage success manifests."""

import os
import tempfile
import unittest
from pathlib import Path

from tools.verification_stage_cache import (
    collect_stage_inputs,
    discover_project_inputs,
    success_manifest_matches,
    write_success_manifest,
)


class VerificationStageCacheTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.stage = self.root / "stage"
        self.source = self.root / "common" / "source.cpp"
        self.header = self.root / "common" / "header.h"
        self.output = self.stage / "installed" / "captureengine.exe"
        self.depfile = self.stage / "obj" / "source.o.d"
        self.source.parent.mkdir(parents=True)
        self.output.parent.mkdir(parents=True)
        self.depfile.parent.mkdir(parents=True)
        (self.root / "build.py").write_text("# build\n", encoding="utf-8")
        self.source.write_text('#include "header.h"\n', encoding="utf-8")
        self.header.write_text("#define VALUE 1\n", encoding="utf-8")
        self.output.write_bytes(b"binary")
        self.depfile.write_text(f"{self.stage / 'obj/source.o'}: {self.source} {self.header}\n", encoding="utf-8")
        self.manifest = self.stage / "success.json"

    def write_manifest(self) -> None:
        discovered, inputs = collect_stage_inputs(
            project_root=str(self.root),
            stage_root=str(self.stage),
        )
        write_success_manifest(
            str(self.manifest),
            discovered_inputs=discovered,
            all_inputs=inputs,
            outputs=[str(self.output)],
        )

    def matches(self) -> bool:
        return success_manifest_matches(
            str(self.manifest),
            discovered_inputs=discover_project_inputs(str(self.root)),
        )

    def test_identical_inputs_and_outputs_reuse_success(self) -> None:
        self.write_manifest()
        self.assertTrue(self.matches())

    def test_content_change_invalidates_with_unchanged_timestamp(self) -> None:
        self.write_manifest()
        timestamp = self.header.stat().st_mtime_ns
        self.header.write_text("#define VALUE 2\n", encoding="utf-8")
        os.utime(self.header, ns=(timestamp, timestamp))
        self.assertFalse(self.matches())

    def test_new_project_input_invalidates_the_discovery_set(self) -> None:
        self.write_manifest()
        (self.root / "common" / "new.cpp").write_text("int value;\n", encoding="utf-8")
        self.assertFalse(self.matches())

    def test_missing_or_changed_output_invalidates(self) -> None:
        self.write_manifest()
        self.output.write_bytes(b"changed")
        self.assertFalse(self.matches())
        self.output.unlink()
        self.assertFalse(self.matches())

    def test_generated_build_identity_and_lint_baselines_are_excluded(self) -> None:
        generated = self.root / "common" / "build_version.h"
        baseline = self.root / "tools" / "clang_tidy_baseline.json"
        link_cache = self.root / "tests" / "unit_tests.exe.link-cache.json"
        generated.write_text("#define BUILD_NUMBER 1\n", encoding="utf-8")
        baseline.parent.mkdir()
        baseline.write_text("{}\n", encoding="utf-8")
        link_cache.parent.mkdir(exist_ok=True)
        link_cache.write_text("{}\n", encoding="utf-8")
        discovered = discover_project_inputs(str(self.root))
        self.assertNotIn(os.path.normcase(str(generated.resolve())), discovered)
        self.assertNotIn(os.path.normcase(str(baseline.resolve())), discovered)
        self.assertNotIn(os.path.normcase(str(link_cache.resolve())), discovered)

    def test_non_cpp_build_inputs_are_discovered(self) -> None:
        manifest = self.root / "captureengine" / "captureengine.manifest"
        shader = self.root / "tools" / "shaders" / "overlay.hlsl"
        definition = self.root / "hook" / "layer.def"
        for path in (manifest, shader, definition):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("input\n", encoding="utf-8")

        discovered = discover_project_inputs(str(self.root))

        for path in (manifest, shader, definition):
            self.assertIn(os.path.normcase(str(path.resolve())), discovered)


if __name__ == "__main__":
    unittest.main()
