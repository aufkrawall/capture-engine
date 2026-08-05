"""Regression tests for tools/refactor/source_splitter.py chunk scanning.

The scanner turns a logical source into balanced top-level chunks. A
multi-line raw string between the last token of a declaration and its
terminating semicolon used to truncate the chunk at the token line, dropping
the string body (and its terminator) from every generated unit. Pin the
behaviour: raw-string bodies stay inside their declaration chunk.
"""

from __future__ import annotations

import unittest
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


if __name__ == "__main__":
    unittest.main()
