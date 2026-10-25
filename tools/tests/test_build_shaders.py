# build.py executes its fragments via exec, so its module attributes exist only
# at runtime; pyright cannot see them through the facade.
# pyright: reportAttributeAccessIssue=false

import os
import tempfile
import unittest
from unittest.mock import patch

import build


class PrecompiledShaderManifestTest(unittest.TestCase):
    def test_every_manifest_source_is_one_raw_string_literal(self) -> None:
        for name, (path, symbol) in build.SHADER_SOURCES.items():
            with self.subTest(source=name):
                self.assertTrue(build.read_raw_shader_source(path, symbol).strip())

    def test_requests_are_unique_and_cover_every_owner(self) -> None:
        requests = build.collect_shader_requests()
        keys = [build.request_key(source, entry, target, flags) for _owner, source, entry, target, flags in requests]
        self.assertEqual(len(keys), len(set(keys)))
        owners = {owner for owner, *_rest in requests}
        self.assertEqual(owners, {owner for owner, *_rest in build.SHADER_REQUESTS})
        # PackedP010's VS_Main is RetainedYuv's (same source and target): one entry serves both.
        self.assertEqual(len(requests), sum(len(entries) for _o, _s, entries, _f in build.SHADER_REQUESTS) - 1)

    def test_request_key_is_fnv1a_over_source_entry_target_and_flags(self) -> None:
        # Pinned so the C++ RequestKey and this one cannot drift apart silently.
        self.assertEqual(build.request_key(b"", "", "", 0), 0x778B1A14B6876AA7)
        self.assertNotEqual(build.request_key(b"a", "VS_Main", "vs_4_0", 0),
                            build.request_key(b"a", "VS_Main", "vs_4_0", 1 << 15))

    def test_a_duplicate_or_missing_literal_fails_the_build(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            relative = "shader.h"
            with open(os.path.join(temporary, relative), "w", encoding="utf-8") as handle:
                handle.write('inline constexpr char kA[] = R"(one)";\ninline constexpr char kA[] = R"(two)";\n')
            with patch.object(build, "PROJECT_ROOT", temporary):
                with self.assertRaises(RuntimeError):
                    build.read_raw_shader_source(relative, "kA")
                with self.assertRaises(RuntimeError):
                    build.read_raw_shader_source(relative, "kMissing")

    def test_a_host_without_the_compiler_gets_an_empty_but_valid_table(self) -> None:
        header = build.render_precompiled_shader_header([], "none")
        self.assertIn("kEntryCount = 0;", header)
        self.assertIn("kEntries[] = {", header)


if __name__ == "__main__":
    unittest.main()
