"""Regression tests for tools/refactor/source_splitter.py chunk scanning.

The scanner turns a logical source into balanced top-level chunks. A
multi-line raw string between the last token of a declaration and its
terminating semicolon used to truncate the chunk at the token line, dropping
the string body (and its terminator) from every generated unit. Pin the
behaviour: raw-string bodies stay inside their declaration chunk.
"""

from __future__ import annotations

import unittest
import tempfile
from pathlib import Path

from tools.refactor import source_splitter


class SourceSplitterScanTest(unittest.TestCase):
    def test_raw_string_var_keeps_body_and_terminator(self) -> None:
        text = (
            "namespace {\n"
            "constexpr const char* SHADER = R\"GLSL(\n"
            "#version 330 core\n"
            "void main() {\n"
            "    gl_Position = vec4(0.0);\n"
            "}\n"
            ")GLSL\";\n"
            "}\n"
        )
        chunks = source_splitter.scan(text)
        shader = next(c for c in chunks if c.name == "SHADER")
        self.assertEqual(shader.kind, "var")
        self.assertIn("#version 330 core", shader.text)
        self.assertIn(")GLSL\";", shader.text)
        self.assertEqual(shader.start, 1)
        self.assertEqual(shader.end, 7)

    def test_plain_single_line_var_unchanged(self) -> None:
        text = "int answer = 42;\n"
        chunks = source_splitter.scan(text)
        self.assertEqual(len(chunks), 1)
        self.assertEqual(chunks[0].name, "answer")
        self.assertEqual(chunks[0].text, text.rstrip("\n"))

    def test_scan_covers_every_non_blank_line(self) -> None:
        text = (
            "int a = 1;\n"
            "int b = R\"x(\n"
            "body\n"
            ")x\";\n"
            "int c = 3;\n"
        )
        chunks = source_splitter.scan(text)
        covered = {line for chunk in chunks for line in range(chunk.start, chunk.end)}
        self.assertEqual(covered, {0, 1, 2, 3, 4})


class SourceSplitterSplitTest(unittest.TestCase):
    def _split(self, text: str, grouping: dict) -> tuple[Path, dict]:
        temporary = tempfile.TemporaryDirectory()
        self.addCleanup(temporary.cleanup)
        root = Path(temporary.name)
        facade = root / "m.cpp"
        facade.write_text(text, encoding="utf-8", newline="\n")
        source_splitter.split_source(facade, grouping)
        return root, grouping

    def test_unscope_anon_moves_definition_to_unit_with_prototype(self) -> None:
        root, grouping = self._split(
            "int A() { return 1; }\nnamespace {\nint B() { return A(); }\n}\n",
            {
                "module": "m",
                "header": "m_internal.h",
                "units": {"m.cpp": {"rest": True}, "m_b.cpp": {"chunks": [1]}},
                "unscope_anon": [1],
                "delete": [],
            },
        )
        header = (root / "m_internal.h").read_text(encoding="utf-8")
        unit_b = (root / "m_b.cpp").read_text(encoding="utf-8")
        unit_main = (root / "m.cpp").read_text(encoding="utf-8")
        self.assertIn("int B();", header)
        self.assertNotIn("static int B();", header)
        self.assertIn("int B() {", unit_b)
        self.assertNotIn("namespace {", unit_b)
        self.assertIn("int A() {", unit_main)

    def test_unstatic_var_gets_extern_after_inline_use(self) -> None:
        root, grouping = self._split(
            "static int g_value = 7;\ninline int Read() { return g_value; }\nint Use() { return Read(); }\n",
            {
                "module": "m",
                "header": "m_internal.h",
                "units": {"m.cpp": {"rest": True}, "m_use.cpp": {"chunks": [2]}},
                "unstatic": [0],
                "delete": [],
            },
        )
        header = (root / "m_internal.h").read_text(encoding="utf-8")
        unit_main = (root / "m.cpp").read_text(encoding="utf-8")
        unit_use = (root / "m_use.cpp").read_text(encoding="utf-8")
        self.assertLess(header.find("extern int m_g_value;"), header.find("inline int Read() {"))
        self.assertIn("int m_g_value = 7;", unit_main)
        self.assertIn("int Use() {", unit_use)


if __name__ == "__main__":
    unittest.main()
