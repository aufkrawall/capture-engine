"""Fixture-driven tests for tools/check_cengine_exports.py."""

from __future__ import annotations

import contextlib
import io
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

from tools import check_cengine_exports as exports  # noqa: E402

HEADER = r"""/* SPDX-License-Identifier: MIT
 * cengine.h - CE_API is mentioned in this comment: CE_API void ce_not_a_function(void);
 */
#ifndef CENGINE_H
#define CENGINE_H

#if defined(CE_STATIC_LIBRARY)
#define CE_API
#elif defined(CE_BUILDING_LIBRARY)
#  define CE_API \
      __declspec(dllexport)
#else
#define CE_API __declspec(dllimport)
#endif

typedef struct ce_runtime ce_runtime_t;
// CE_API void ce_also_not_real(void);
CE_API uint32_t    ce_api_version(void);
CE_API const char* ce_version_string(void);              /* product build, static UTF-8 */
CE_API ce_status_t ce_runtime_create(const ce_runtime_desc_t* desc,
                                     ce_runtime_t** out_runtime);
/* Waits up to timeout_ms. */
CE_API ce_status_t ce_runtime_next_event(ce_runtime_t* rt, uint32_t timeout_ms, const ce_event_t** out);
CE_API void* ce_runtime_event_handle(ce_runtime_t* rt);
CE_API void        ce_settings_discard(ce_settings_edit_t* edit);

typedef struct ce_event {
    int32_t type;
} ce_event_t;
#endif
"""

HEADER_NAMES = [
    "ce_api_version",
    "ce_version_string",
    "ce_runtime_create",
    "ce_runtime_next_event",
    "ce_runtime_event_handle",
    "ce_settings_discard",
]

DEF_TEXT = """; module definition
LIBRARY cengine
EXPORTS
    ce_api_version          @1
    ce_version_string       @2 ; trailing comment
    ce_runtime_create
    ce_runtime_next_event   PRIVATE
    "ce_runtime_event_handle"
    ce_settings_discard=ce_settings_discard_impl @6
"""

READOBJ_DUMP = """File: cengine.dll
Format: COFF-x86-64
Arch: x86_64
AddressSize: 64bit
Export {
  Ordinal: 1
  Name: ce_api_version
  RVA: 0x1000
}
Export {
  Ordinal: 2
  Name: ce_version_string
  RVA: 0x1010
}
Export {
  Ordinal: 3
  Name: ce_runtime_create
  RVA: 0x1020
}
Export {
  Ordinal: 4
  Name: ce_runtime_next_event
  RVA: 0x1030
}
Export {
  Ordinal: 5
  Name: ce_runtime_event_handle
  RVA: 0x1040
}
Export {
  Ordinal: 6
  Name: ce_settings_discard
  RVA: 0x1050
}
"""

LLVM_OBJDUMP_DUMP = """cengine.dll:	file format coff-x86-64

DLL Name: KERNEL32.dll

Export Table:
 DLL name: cengine.dll
 Ordinal base: 1
   Ordinal      RVA  Name
         1   0x1000  ce_api_version
         2   0x1010  ce_version_string
         3   0x1020  ce_runtime_create
         4   0x1030
         5   0x1040  ce_runtime_event_handle

The Function Table (interpreted .pdata section contents)
"""

GNU_OBJDUMP_DUMP = """
cengine.dll:     file format pei-x86-64

There is an import table in .idata at 0x5000

The Export Tables (interpreted .edata section contents)

Export Flags 			0
Name Table VMA: 0x00002034
Ordinal Base 			1

[Ordinal/Name Pointer] Table
\t[   0] ce_api_version
\t[   1] ce_version_string
\t[   2] ce_runtime_create

PE File Base Relocations (interpreted .reloc section contents)
"""


class HeaderTest(unittest.TestCase):
    def test_declared_functions_in_order_with_line_numbers(self) -> None:
        parsed = exports.parse_header(HEADER)
        self.assertEqual(parsed.names, HEADER_NAMES)
        self.assertEqual(parsed.problems, [])
        lines = HEADER.splitlines()
        for name, line in parsed.lines.items():
            self.assertIn(name, lines[line - 1], f"{name} must be reported at its own line")

    def test_comments_and_the_linkage_macro_are_not_declarations(self) -> None:
        names = exports.parse_header(HEADER).names
        self.assertNotIn("ce_not_a_function", names)
        self.assertNotIn("ce_also_not_real", names)

    def test_multi_line_declaration_and_pointer_return_types(self) -> None:
        names = exports.parse_header(HEADER).names
        self.assertIn("ce_runtime_create", names)
        self.assertIn("ce_runtime_event_handle", names)
        self.assertIn("ce_version_string", names)

    def test_duplicate_declarations_and_bad_names_are_problems(self) -> None:
        text = "CE_API int ce_a(void);\nCE_API int ce_a(void);\nCE_API int CeBad(void);\nCE_API int ce_Upper(void);\n"
        parsed = exports.parse_header(text, "h")
        self.assertEqual(parsed.names, ["ce_a", "CeBad", "ce_Upper"])
        self.assertEqual(len(parsed.problems), 3)
        self.assertIn("declared twice", parsed.problems[0])
        self.assertTrue(any("CeBad" in problem for problem in parsed.problems))
        self.assertTrue(any("ce_Upper" in problem for problem in parsed.problems))

    def test_unterminated_and_non_function_declarations_are_reported(self) -> None:
        self.assertEqual(len(exports.parse_header("CE_API int ce_a(void)").problems), 1)
        self.assertEqual(len(exports.parse_header("CE_API int ce_variable;\n").problems), 1)

    def test_the_real_headers_parse_cleanly_and_declare_nothing_twice(self) -> None:
        shipped_path = Path(exports.ROOT) / "include" / "cengine" / "cengine.h"
        draft_path = Path(exports.ROOT) / "include" / "cengine" / "cengine_draft.h"
        if not shipped_path.is_file() or not draft_path.is_file():
            self.skipTest("no shipped and draft headers in this tree")
        shipped = exports.parse_header(shipped_path.read_text(encoding="utf-8"), "shipped")
        self.assertEqual(shipped.problems, [])
        self.assertGreaterEqual(len(shipped.names), 20)
        for name in ("ce_api_version", "ce_runtime_create", "ce_runtime_next_event", "ce_runtime_desc_init"):
            self.assertIn(name, shipped.names)
        draft = exports.parse_header(draft_path.read_text(encoding="utf-8"), "draft")
        self.assertEqual(draft.problems, [])
        self.assertIn("ce_settings_begin", draft.names)
        self.assertEqual(set(draft.names) & set(shipped.names), set(), "a declaration exists in both headers")


class DefTest(unittest.TestCase):
    def test_names_flags_comments_aliases_and_quotes(self) -> None:
        parsed = exports.parse_def(DEF_TEXT)
        self.assertEqual(parsed.names, HEADER_NAMES)
        self.assertEqual(parsed.problems, [])

    def test_only_exports_section_counts(self) -> None:
        text = "LIBRARY cengine\nDESCRIPTION \"x\"\nEXPORTS\n  ce_a\nSECTIONS\n  .data READ WRITE\n"
        self.assertEqual(exports.parse_def(text).names, ["ce_a"])

    def test_exports_keyword_with_the_first_name_on_the_same_line(self) -> None:
        self.assertEqual(exports.parse_def("EXPORTS ce_a\n  ce_b\n").names, ["ce_a", "ce_b"])

    def test_duplicate_names_and_ordinals_and_naming_rule(self) -> None:
        parsed = exports.parse_def("EXPORTS\n ce_a @1\n ce_b @1\n ce_a\n BadName\n")
        joined = "\n".join(parsed.problems)
        self.assertIn("ordinal 1 is used by ce_a and ce_b", joined)
        self.assertIn("declared twice", joined)
        self.assertIn("BadName", joined)


class ExportsDumpTest(unittest.TestCase):
    def test_llvm_readobj(self) -> None:
        self.assertEqual(exports.parse_exports_dump(READOBJ_DUMP), HEADER_NAMES)

    def test_llvm_objdump_skips_ordinal_only_rows_and_other_tables(self) -> None:
        self.assertEqual(
            exports.parse_exports_dump(LLVM_OBJDUMP_DUMP),
            ["ce_api_version", "ce_version_string", "ce_runtime_create", "ce_runtime_event_handle"],
        )

    def test_gnu_objdump(self) -> None:
        self.assertEqual(
            exports.parse_exports_dump(GNU_OBJDUMP_DUMP), ["ce_api_version", "ce_version_string", "ce_runtime_create"]
        )

    def test_empty_or_unrecognized_text_has_no_exports(self) -> None:
        self.assertEqual(exports.parse_exports_dump(""), [])
        self.assertEqual(exports.parse_exports_dump("nothing useful here\n"), [])


class CompareTest(unittest.TestCase):
    def test_missing_and_extra_relative_to_the_header(self) -> None:
        problems = exports.compare(["ce_a", "ce_b"], ["ce_b", "ce_c", "ce_d"], "the DLL")
        self.assertEqual(problems, ["missing in the DLL: ce_a", "extra in the DLL: ce_c", "extra in the DLL: ce_d"])

    def test_allowed_extras_are_tolerated_but_missing_never_is(self) -> None:
        self.assertEqual(exports.compare(["ce_a"], ["ce_a", "ce_dbg"], "dll", ["ce_dbg"]), [])
        self.assertEqual(exports.compare(["ce_a", "ce_dbg"], ["ce_a"], "dll", ["ce_dbg"]), ["missing in dll: ce_dbg"])

    def test_check_combines_all_three_sources(self) -> None:
        _, problems = exports.check(HEADER, "h", DEF_TEXT, "x.def", HEADER_NAMES, "x.dll")
        self.assertEqual(problems, [])
        _, problems = exports.check(HEADER, "h", DEF_TEXT.replace("    ce_runtime_create\n", ""), "x.def",
                                    HEADER_NAMES[:-1] + ["ce_surprise"], "x.dll")
        self.assertEqual(
            problems,
            ["missing in x.def: ce_runtime_create", "missing in x.dll: ce_settings_discard",
             "extra in x.dll: ce_surprise"],
        )


class CommandLineTest(unittest.TestCase):
    def setUp(self) -> None:
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        self.root = Path(directory.name)
        self.header = self.root / "cengine.h"
        self.header.write_text(HEADER, encoding="utf-8")
        self.definition = self.root / "cengine.def"
        self.definition.write_text(DEF_TEXT, encoding="utf-8")
        self.dump = self.root / "exports.txt"
        self.dump.write_text(READOBJ_DUMP, encoding="utf-8")

    def run_main(self, *arguments: str) -> tuple[int, str, str]:
        out, err = io.StringIO(), io.StringIO()
        with contextlib.redirect_stdout(out), contextlib.redirect_stderr(err):
            status = exports.main(["--header", str(self.header), *arguments])
        return status, out.getvalue(), err.getvalue()

    def test_consistent_artifacts_pass(self) -> None:
        status, out, err = self.run_main("--def", str(self.definition), "--exports-dump", str(self.dump))
        self.assertEqual((status, err), (0, ""))
        self.assertIn("6 function(s)", out)
        self.assertIn("consistent with def and dll", out)

    def test_header_only_check(self) -> None:
        status, out, _ = self.run_main()
        self.assertEqual(status, 0)
        self.assertIn("(header only)", out)

    def test_mismatch_fails_and_names_every_difference(self) -> None:
        self.definition.write_text(DEF_TEXT + "    ce_extra_symbol\n", encoding="utf-8")
        self.dump.write_text(READOBJ_DUMP.replace("ce_version_string", "ce_renamed"), encoding="utf-8")
        status, _, err = self.run_main("--def", str(self.definition), "--exports-dump", str(self.dump))
        self.assertEqual(status, 1)
        self.assertIn("extra in", err)
        self.assertIn("ce_extra_symbol", err)
        self.assertIn("missing in", err)
        self.assertIn("ce_version_string", err)
        self.assertIn("ce_renamed", err)

    def test_allow_extra_option(self) -> None:
        self.dump.write_text(READOBJ_DUMP + "Export {\n  Ordinal: 7\n  Name: ce_debug_only\n  RVA: 0x1\n}\n",
                             encoding="utf-8")
        status, _, _ = self.run_main("--exports-dump", str(self.dump))
        self.assertEqual(status, 1)
        status, _, _ = self.run_main("--exports-dump", str(self.dump), "--allow-extra", "ce_debug_only")
        self.assertEqual(status, 0)

    def test_list_prints_the_declared_functions(self) -> None:
        status, out, _ = self.run_main("--list")
        self.assertEqual(status, 0)
        self.assertEqual(out.split(), HEADER_NAMES)

    def test_unreadable_inputs_are_a_usage_error(self) -> None:
        status, _, err = self.run_main("--def", str(self.root / "missing.def"))
        self.assertEqual(status, 2)
        self.assertIn("missing.def", err)

    @unittest.skipUnless(os.name == "nt", "the fake tool is a batch file")
    def test_dll_is_read_through_the_export_tool(self) -> None:
        tool = self.root / "llvm-readobj.cmd"
        tool.write_text('@type "%~dp0exports.txt"\r\n', encoding="utf-8")
        dll = self.root / "cengine.dll"
        dll.write_bytes(b"MZ")
        status, out, err = self.run_main("--def", str(self.definition), "--dll", str(dll), "--tool", str(tool))
        self.assertEqual((status, err), (0, ""), out)

    def test_find_export_tool_prefers_the_projects_llvm_tools(self) -> None:
        bin_dir = self.root / "build" / "msys64" / "clang64" / "bin"
        bin_dir.mkdir(parents=True)
        (bin_dir / "objdump.exe").write_bytes(b"")
        self.assertEqual(exports.find_export_tool(str(self.root)), str(bin_dir / "objdump.exe"))
        (bin_dir / "llvm-readobj.exe").write_bytes(b"")
        self.assertEqual(exports.find_export_tool(str(self.root)), str(bin_dir / "llvm-readobj.exe"))

    def test_default_header_falls_back_to_the_draft(self) -> None:
        include = self.root / "include" / "cengine"
        include.mkdir(parents=True)
        self.assertTrue(exports.default_header(str(self.root)).endswith("cengine_draft.h"))
        (include / "cengine.h").write_text("", encoding="utf-8")
        self.assertTrue(exports.default_header(str(self.root)).endswith("cengine.h"))


if __name__ == "__main__":
    unittest.main()
