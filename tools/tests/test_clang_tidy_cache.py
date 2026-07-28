"""Regression tests for the content-addressed clang-tidy cache."""

import json
import os
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

from tools import clang_tidy_cache


class ClangTidyCacheTest(unittest.TestCase):
    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.root = Path(temporary.name)
        self.cache = self.root / "cache"
        self.tool = self.root / "clang-tidy.exe"
        self.source = self.root / "source.cpp"
        self.header = self.root / "shared.h"
        self.depfile = self.root / "source.o.d"
        self.config = self.root / ".clang-tidy"
        self.tool.write_bytes(b"clang-tidy")
        self.source.write_text('#include "shared.h"\nint value = SHARED;\n', encoding="utf-8")
        self.header.write_text("#define SHARED 1\n", encoding="utf-8")
        self.config.write_text("Checks: bugprone-*\n", encoding="utf-8")
        self.depfile.write_text(f"{self.root / 'source.o'}: {self.source} {self.header}\n", encoding="utf-8")
        self.entry = {
            "directory": str(self.root),
            "file": str(self.source),
            "arguments": [
                "clang++.exe",
                "-MMD",
                "-MF",
                str(self.depfile),
                "-c",
                str(self.source),
                "-o",
                str(self.root / "source.o"),
            ],
        }

    @staticmethod
    def completed(stdout: str = "", returncode: int = 0):
        return clang_tidy_cache.subprocess.CompletedProcess(
            args=["clang-tidy"],
            returncode=returncode,
            stdout=stdout,
            stderr="",
        )

    def run_cache(self):
        return clang_tidy_cache.run_cached_clang_tidy(
            clang_tidy=str(self.tool),
            compile_database=[self.entry],
            compile_database_dir=str(self.root),
            project_root=str(self.root),
            cache_dir=str(self.cache),
            jobs=2,
            env={"PATH": ""},
        )

    def test_second_identical_run_is_a_cache_hit(self) -> None:
        warning = f"{self.source}:2:1: warning: issue [bugprone-test]\n"
        with patch.object(clang_tidy_cache.subprocess, "run", return_value=self.completed(warning)) as execute:
            first = self.run_cache()
            second = self.run_cache()

        self.assertEqual(execute.call_count, 1)
        self.assertEqual((first.hits, first.misses), (0, 1))
        self.assertEqual((second.hits, second.misses), (1, 0))
        self.assertEqual(second.stdout, warning.rstrip())

    def test_header_content_change_invalidates_even_with_same_timestamp(self) -> None:
        with patch.object(clang_tidy_cache.subprocess, "run", return_value=self.completed()) as execute:
            self.run_cache()
            original_timestamp = self.header.stat().st_mtime_ns
            self.header.write_text("#define SHARED 2\n", encoding="utf-8")
            os.utime(self.header, ns=(original_timestamp, original_timestamp))
            changed = self.run_cache()

        self.assertEqual(execute.call_count, 2)
        self.assertEqual((changed.hits, changed.misses), (0, 1))

    def test_tool_config_and_compile_command_are_fingerprint_inputs(self) -> None:
        with patch.object(clang_tidy_cache.subprocess, "run", return_value=self.completed()) as execute:
            self.run_cache()
            self.tool.write_bytes(b"new-clang-tidy")
            self.run_cache()
            self.config.write_text("Checks: performance-*\n", encoding="utf-8")
            self.run_cache()
            self.entry["arguments"].insert(1, "-DCHANGED=1")
            self.run_cache()

        self.assertEqual(execute.call_count, 4)

    def test_missing_depfile_runs_but_does_not_cache(self) -> None:
        self.depfile.unlink()
        with patch.object(clang_tidy_cache.subprocess, "run", return_value=self.completed()) as execute:
            first = self.run_cache()
            second = self.run_cache()

        self.assertEqual(execute.call_count, 2)
        self.assertEqual(first.uncacheable, 1)
        self.assertEqual(second.uncacheable, 1)

    def test_failed_tool_run_is_not_cached(self) -> None:
        with patch.object(
            clang_tidy_cache.subprocess,
            "run",
            side_effect=[self.completed(returncode=1), self.completed()],
        ) as execute:
            failed = self.run_cache()
            recovered = self.run_cache()

        self.assertEqual(execute.call_count, 2)
        self.assertEqual(failed.returncode, 1)
        self.assertEqual(recovered.returncode, 0)

    def test_snapshot_requires_the_same_build_script(self) -> None:
        snapshot = self.root / "snapshot"
        clang_tidy_cache.write_compile_database_snapshot(
            compile_database=[self.entry],
            snapshot_dir=str(snapshot),
            build_script_sha256="build-a",
        )

        loaded = clang_tidy_cache.load_compile_database_snapshot(
            snapshot_dir=str(snapshot),
            build_script_sha256="build-a",
        )
        self.assertEqual(loaded, [self.entry])
        self.assertIsNone(
            clang_tidy_cache.load_compile_database_snapshot(
                snapshot_dir=str(snapshot),
                build_script_sha256="build-b",
            )
        )
        self.assertIsInstance(
            json.loads((snapshot / "compile_commands.json").read_text(encoding="utf-8")),
            list,
        )


if __name__ == "__main__":
    unittest.main()
