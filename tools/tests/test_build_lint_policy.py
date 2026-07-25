"""Lint and compilation-database policy tests for build.py.

Split out of test_build_flags.py, which reached the AGENTS.md size ceiling.
Covers the clang-tidy header filters and the compilation database that the
lint ratchets are measured over.
"""

import json
import re
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

import build


class ClangTidyConfigPolicyTest(unittest.TestCase):
    def test_clang_tidy_excludes_external_and_generated_headers(self) -> None:
        config = (Path(build.__file__).parent / ".clang-tidy").read_text(encoding="utf-8")
        self.assertIn("HeaderFilterRegex:", config)
        self.assertIn("ExcludeHeaderFilterRegex:", config)
        for tree in ("external", "installed", "ffmpeg_build"):
            self.assertIn(tree, config)

    def test_clang_tidy_header_filters_match_project_headers_on_windows(self) -> None:
        """Project headers must be analyzed wherever the repository is checked out.

        A bare 'build' segment in the exclude pattern also matched this
        checkout's own path (...\\Programme\\build\\captureproject\\...), which
        silently disabled clang-tidy for every project header - 456 of 1616
        findings were invisible. Both patterns must also accept backslashes,
        because clang-tidy reports included files with native separators.
        """
        config = (Path(build.__file__).parent / ".clang-tidy").read_text(encoding="utf-8")
        include = re.search(r"^HeaderFilterRegex:\s*'(.+)'\s*$", config, re.MULTILINE)
        exclude = re.search(r"^ExcludeHeaderFilterRegex:\s*'(.+)'\s*$", config, re.MULTILINE)
        self.assertIsNotNone(include)
        self.assertIsNotNone(exclude)
        include_re = re.compile(include.group(1))
        exclude_re = re.compile(exclude.group(1))

        analyzed = [
            r"C:\Users\dev\Programme\build\captureproject\common\capture_policy\constants.h",
            r"C:/Users/dev/Programme/build/captureproject/hook/common/fps_limiter.h",
            r"C:\proj\captureproject\testapp\dx12_fg_switch_runtime.inl",
        ]
        for path in analyzed:
            self.assertTrue(include_re.search(path), path)
            self.assertIsNone(exclude_re.search(path), path)

        skipped = [
            r"C:\proj\captureproject\build\msys64\clang64\include\c++\v1\vector",
            r"C:\proj\captureproject\external\imgui\imgui.h",
            r"C:\proj\captureproject\ffmpeg_build\include\libavcodec\avcodec.h",
            r"C:\proj\captureproject\build\obj\x64\generated.h",
        ]
        for path in skipped:
            self.assertIsNotNone(exclude_re.search(path), path)


class CompileCommandsDeterminismTest(unittest.TestCase):
    """The compilation database must not depend on parallel compile ordering.

    dx9_test.cpp is compiled as plain D3D9, as D3D9Ex, and again for x86, all
    under the same "file" key. While the survivor was decided by whichever
    parallel task appended last, the recorded flags flipped between builds and
    took the clang-tidy findings for that source with them, so the lint ratchet
    failed at random.
    """

    def entries(self, order):
        return [
            {
                "directory": "C:/proj",
                "file": "C:/proj/testapp/dx9_test.cpp",
                "arguments": ["clang++", "-O2"] + extra + ["C:/proj/testapp/dx9_test.cpp"],
            }
            for extra in order
        ]

    def write(self, commands):
        with tempfile.TemporaryDirectory() as temporary:
            with patch.object(build, "PROJECT_ROOT", temporary), patch.object(
                build, "COMPILE_COMMANDS", commands
            ), patch.object(build, "log", lambda *args, **kwargs: None):
                build.write_compile_commands_json()
                return json.loads((Path(temporary) / "compile_commands.json").read_text(encoding="utf-8"))

    def test_duplicate_sources_resolve_identically_regardless_of_append_order(self) -> None:
        plain = []
        d3d9ex = ["-DCE_TESTAPP_D3D9EX=1"]

        forward = self.write(self.entries([plain, d3d9ex]))
        reverse = self.write(self.entries([d3d9ex, plain]))

        self.assertEqual(len(forward), 1)
        self.assertEqual(forward, reverse)

    def test_x86_variant_never_wins_over_the_x64_one(self) -> None:
        x64 = ["--target=x86_64-w64-windows-gnu"]
        x86 = ["--target=i686-w64-windows-gnu"]

        for order in ([x64, x86], [x86, x64]):
            written = self.write(self.entries(order))
            self.assertEqual(len(written), 1)
            self.assertFalse(build.is_x86_compile_command(written[0]["arguments"]))


if __name__ == "__main__":
    unittest.main()


if __name__ == "__main__":
    unittest.main()
