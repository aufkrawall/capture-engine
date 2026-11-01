"""Regression tests for the content-addressed clang-tidy cache."""

import json
import os
import tempfile
import threading
import unittest
from concurrent.futures import ThreadPoolExecutor
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
        self.config = self.root / "tools" / "config" / ".clang-tidy"
        self.tool.write_bytes(b"clang-tidy")
        self.config.parent.mkdir(parents=True, exist_ok=True)
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

    def test_concurrent_atomic_writers_publish_only_the_first_completed_generation(self) -> None:
        destination = self.root / "concurrent.json"
        destination.write_text('{"writer": -1}\n', encoding="utf-8")
        worker_count = 8
        write_barrier = threading.Barrier(worker_count)
        original_write_text = Path.write_text
        original_replace = os.replace

        def synchronized_write_text(path: Path, *args, **kwargs):
            result = original_write_text(path, *args, **kwargs)
            write_barrier.wait(timeout=5)
            return result

        with patch.object(Path, "write_text", synchronized_write_text), patch.object(
            clang_tidy_cache.os, "replace", wraps=original_replace
        ) as replace:
            with ThreadPoolExecutor(max_workers=worker_count) as executor:
                futures = [
                    executor.submit(clang_tidy_cache._atomic_write_json, destination, {"writer": writer})
                    for writer in range(worker_count)
                ]
                for future in futures:
                    future.result()

        payload = json.loads(destination.read_text(encoding="utf-8"))
        self.assertIn(payload["writer"], range(worker_count))
        self.assertEqual(replace.call_count, 1)
        self.assertEqual(list(self.root.glob("concurrent.json.tmp.*")), [])

    def test_sequential_atomic_writer_replaces_the_previous_generation(self) -> None:
        destination = self.root / "sequential.json"
        clang_tidy_cache._atomic_write_json(destination, {"generation": 1})
        clang_tidy_cache._atomic_write_json(destination, {"generation": 2})

        self.assertEqual(json.loads(destination.read_text(encoding="utf-8")), {"generation": 2})


class LocationIndependentCacheTest(unittest.TestCase):
    """A result computed in one checkout serves the identical translation unit in another."""

    def setUp(self) -> None:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        self.base = Path(temporary.name)
        self.cache = self.base / "shared-cache" / "clang-tidy"
        self.tool = self.base / "clang-tidy.exe"
        self.tool.write_bytes(b"clang-tidy")

    def checkout(self, name: str):
        root = self.base / name
        (root / "tools" / "config").mkdir(parents=True)
        (root / "tools" / "config" / ".clang-tidy").write_text("Checks: bugprone-*\n", encoding="utf-8")
        (root / "hook").mkdir()
        source = root / "hook" / "source.cpp"
        header = root / "hook" / "shared.h"
        source.write_text('#include "shared.h"\nint value = SHARED;\n', encoding="utf-8")
        header.write_text("#define SHARED 1\n", encoding="utf-8")
        depfile = root / "source.o.d"
        depfile.write_text(f"{root / 'source.o'}: {source} {header}\n", encoding="utf-8")
        entry = {
            "directory": str(root),
            "file": str(source),
            "arguments": ["clang++.exe", f"-I{root / 'hook'}", "-MMD", "-MF", str(depfile), "-c", str(source)],
        }
        return root, entry

    def run_in(self, root: Path, entry):
        return clang_tidy_cache.run_cached_clang_tidy(
            clang_tidy=str(self.tool),
            compile_database=[entry],
            compile_database_dir=str(root),
            project_root=str(root),
            cache_dir=str(self.cache),
            jobs=1,
            env={"PATH": ""},
        )

    def test_release_workspace_reuses_the_developer_tree_result_with_its_own_paths(self) -> None:
        developer, developer_entry = self.checkout("captureproject")
        runner, runner_entry = self.checkout("runner-work-capture-engine")
        warning = f"{developer / 'hook' / 'source.cpp'}:2:1: warning: issue [bugprone-test]"
        completed = ClangTidyCacheTest.completed(warning)
        with patch.object(clang_tidy_cache.subprocess, "run", return_value=completed) as execute:
            self.run_in(developer, developer_entry)
            reused = self.run_in(runner, runner_entry)
        self.assertEqual(execute.call_count, 1)
        self.assertEqual((reused.hits, reused.misses), (1, 0))
        # The diagnostic points into the checkout that asked, so subsystem attribution holds.
        self.assertEqual(reused.stdout, f"{runner / 'hook' / 'source.cpp'}:2:1: warning: issue [bugprone-test]")
        stored = next(self.cache.glob("*.json")).read_text(encoding="utf-8")
        self.assertNotIn(json.dumps(str(developer))[1:-1], stored)

    def test_different_content_in_the_other_checkout_still_misses(self) -> None:
        developer, developer_entry = self.checkout("captureproject")
        runner, runner_entry = self.checkout("runner")
        (runner / "hook" / "shared.h").write_text("#define SHARED 2\n", encoding="utf-8")
        with patch.object(clang_tidy_cache.subprocess, "run", return_value=ClangTidyCacheTest.completed()) as execute:
            self.run_in(developer, developer_entry)
            changed = self.run_in(runner, runner_entry)
        self.assertEqual(execute.call_count, 2)
        self.assertEqual((changed.hits, changed.misses), (0, 1))

    @unittest.skipUnless(os.name == "nt", "the runner workspace junctions build/msys64 on Windows")
    def test_junctioned_toolchain_spelled_with_forward_slashes_still_hits(self) -> None:
        # Release run 41335417448: the developer tree's compiler arguments became
        # `{ROOT}\build\msys64/clang64/...` (junction-target alias) and the runner's
        # `{ROOT}/build/msys64/clang64/...` (root alias), so all 1,484 entries missed.
        import _winapi

        developer, developer_entry = self.checkout("captureproject")
        toolchain = developer / "build" / "msys64" / "clang64" / "include"
        toolchain.mkdir(parents=True)
        (toolchain / "system.h").write_text("#define SYSTEM 1\n", encoding="utf-8")
        runner, runner_entry = self.checkout("runner-work")
        (runner / "build").mkdir()
        _winapi.CreateJunction(str(developer / "build" / "msys64"), str(runner / "build" / "msys64"))
        for root, entry in ((developer, developer_entry), (runner, runner_entry)):
            include = (root / "build" / "msys64" / "clang64" / "include").as_posix()
            entry["arguments"].insert(1, f"-isystem{include}")
            depfile = Path(entry["arguments"][entry["arguments"].index("-MF") + 1])
            depfile.write_text(depfile.read_text(encoding="utf-8").rstrip("\n")
                               + f" {include}/system.h\n", encoding="utf-8")
        with patch.object(clang_tidy_cache.subprocess, "run", return_value=ClangTidyCacheTest.completed()) as execute:
            self.run_in(developer, developer_entry)
            reused = self.run_in(runner, runner_entry)
        self.assertEqual(execute.call_count, 1)
        self.assertEqual((reused.hits, reused.misses), (1, 0))

    def test_a_longer_sibling_directory_is_not_mistaken_for_the_root(self) -> None:
        aliases = clang_tidy_cache.PathAliases(self.base / "root")
        sibling = str(self.base / "root2" / "file.cpp")
        inside = str(self.base / "root" / "file.cpp")
        self.assertEqual(aliases.normalize(sibling), sibling)
        self.assertEqual(aliases.normalize(inside), os.path.join("{ROOT}", "file.cpp"))
        self.assertEqual(aliases.normalize(inside.replace(os.sep, "/")), "{ROOT}/file.cpp")


if __name__ == "__main__":
    unittest.main()
