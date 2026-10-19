#!/usr/bin/env python3
"""Check cengine's exported function set against its public header.

The public C ABI is whatever ``CE_API`` functions ``include/cengine/cengine.h`` declares (the draft
``cengine_draft.h`` until the shipped header exists). Two more artifacts have to say the same thing
(llm-wiki/library/07 "ABI", milestone M7):

* the module-definition file (``.def``) that fixes the export list, and
* the export table of the built DLL, read with ``llvm-readobj --coff-exports`` (or ``objdump -p``).

The header is the source of truth. The tool reports every name that is declared but not exported and
every name that is exported but not declared, plus malformed or duplicate entries, and exits non-zero.
A missing export compiles, links and then fails ``GetProcAddress`` at the client; an extra one silently
widens the ABI. Neither shows up anywhere else.

    python tools/check_cengine_exports.py --def path/to/cengine.def --dll build/bin/cengine.dll
    python tools/check_cengine_exports.py --list              # the functions the header declares

Exit status: 0 consistent, 1 mismatch, 2 usage or tool failure.
"""

from __future__ import annotations

import argparse
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from typing import Dict, Iterable, List, Optional, Sequence, Tuple

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
NAME_RULE = re.compile(r"^ce_[a-z0-9_]+$")


@dataclass
class ParsedNames:
    """Names in file order plus the problems found while reading them."""

    names: List[str] = field(default_factory=list)
    lines: Dict[str, int] = field(default_factory=dict)
    problems: List[str] = field(default_factory=list)

    def add(self, name: str, line: int, source: str) -> None:
        if name in self.lines:
            self.problems.append(f"{source}:{line}: {name} is declared twice (first at line {self.lines[name]})")
            return
        self.names.append(name)
        self.lines[name] = line
        if not NAME_RULE.match(name):
            self.problems.append(f"{source}:{line}: {name} does not follow the ce_<lowercase> export naming rule")


# ---- header ----------------------------------------------------------------------------------------------

_BLOCK_COMMENT = re.compile(r"/\*.*?\*/", re.DOTALL)
_LINE_COMMENT = re.compile(r"//[^\n]*")
_CE_API = re.compile(r"\bCE_API\b")
_TRAILING_IDENTIFIER = re.compile(r"([A-Za-z_]\w*)\s*$")


def _blank_comments(text: str) -> str:
    """Comments become spaces (newlines kept), so reported line numbers stay those of the file."""

    def blank(match: "re.Match[str]") -> str:
        return re.sub(r"[^\n]", " ", match.group(0))

    return _LINE_COMMENT.sub(blank, _BLOCK_COMMENT.sub(blank, text))


def _blank_preprocessor(text: str) -> str:
    """Directives (``#define CE_API ...`` included, with their continuation lines) become blank."""
    lines = text.split("\n")
    in_directive = False
    for index, line in enumerate(lines):
        if in_directive or line.lstrip().startswith("#"):
            in_directive = line.rstrip().endswith("\\")
            lines[index] = ""
    return "\n".join(lines)


def parse_header(text: str, source: str = "header") -> ParsedNames:
    """The function names of every ``CE_API`` declaration, in declaration order."""
    parsed = ParsedNames()
    code = _blank_preprocessor(_blank_comments(text))
    for match in _CE_API.finditer(code):
        end = code.find(";", match.end())
        line = code.count("\n", 0, match.start()) + 1
        if end < 0:
            parsed.problems.append(f"{source}:{line}: CE_API declaration without a terminating ';'")
            continue
        statement = code[match.end() : end]
        paren = statement.find("(")
        if paren < 0:
            parsed.problems.append(f"{source}:{line}: CE_API declaration is not a function")
            continue
        name = _TRAILING_IDENTIFIER.search(statement[:paren])
        if not name:
            parsed.problems.append(f"{source}:{line}: cannot find the function name in a CE_API declaration")
            continue
        parsed.add(name.group(1), line, source)
    return parsed


# ---- module definition file --------------------------------------------------------------------------------

_DEF_STATEMENTS = {
    "LIBRARY", "NAME", "DESCRIPTION", "STACKSIZE", "HEAPSIZE", "VERSION", "SECTIONS", "SEGMENTS", "STUB", "CODE",
    "DATA", "IMPORTS", "EXETYPE", "PROTMODE",
}  # fmt: skip
_DEF_ENTRY = re.compile(r'^\s*(?:"(?P<quoted>[^"]+)"|(?P<plain>[^\s="@;]+))\s*(?:=\s*\S+)?(?P<rest>.*)$')
_DEF_ORDINAL = re.compile(r"@\s*(\d+)")


def parse_def(text: str, source: str = ".def") -> ParsedNames:
    """Exported names of a module-definition file: the name before any ``=internal``, ordinals, flags
    and comments ignored. Only entries under ``EXPORTS`` count."""
    parsed = ParsedNames()
    ordinals: Dict[int, str] = {}
    in_exports = False
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.split(";", 1)[0].strip()
        if not line:
            continue
        keyword, _, remainder = line.partition(" ")
        keyword = keyword.upper()
        if keyword == "EXPORTS":
            in_exports = True
            line = remainder.strip()
            if not line:
                continue
        elif keyword in _DEF_STATEMENTS:
            in_exports = False
            continue
        if not in_exports:
            continue
        entry = _DEF_ENTRY.match(line)
        name = (entry.group("quoted") or entry.group("plain")) if entry else None
        if not name:
            parsed.problems.append(f"{source}:{number}: unreadable export entry '{line}'")
            continue
        parsed.add(name, number, source)
        ordinal = _DEF_ORDINAL.search(entry.group("rest") if entry else "")
        if ordinal:
            value = int(ordinal.group(1))
            if value in ordinals and ordinals[value] != name:
                parsed.problems.append(f"{source}:{number}: ordinal {value} is used by {ordinals[value]} and {name}")
            ordinals[value] = name
    return parsed


# ---- built DLL ---------------------------------------------------------------------------------------------

_READOBJ_NAME = re.compile(r"^\s*Name:\s*(\S+)\s*$")
_OBJDUMP_LLVM_ROW = re.compile(r"^\s*(\d+)\s+0x[0-9a-fA-F]+(?:\s+(\S+))?\s*$")
_OBJDUMP_GNU_ROW = re.compile(r"^\s*\[\s*\d+\]\s+(\S+)\s*$")


def parse_exports_dump(text: str) -> List[str]:
    """Exported names from ``llvm-readobj --coff-exports``, ``llvm-objdump -p`` or GNU ``objdump -p``
    output (the format is recognized from the text). Ordinal-only exports have no name and are skipped."""
    if re.search(r"^Export \{", text, re.MULTILINE):
        names = []
        for line in text.splitlines():
            match = _READOBJ_NAME.match(line)
            if match:
                names.append(match.group(1))
        return names
    names = []
    section = ""
    for line in text.splitlines():
        stripped = line.strip()
        if stripped.startswith("Export Table:"):
            section = "llvm"
            continue
        if "[Ordinal/Name Pointer] Table" in stripped:
            section = "gnu"
            continue
        if not stripped:
            if section == "gnu":
                section = ""
            continue
        if section == "llvm":
            match = _OBJDUMP_LLVM_ROW.match(line)
            if match:
                if match.group(2):
                    names.append(match.group(2))
            elif not (stripped.startswith(("DLL name:", "Ordinal base:", "Ordinal")) or stripped[0].isdigit()):
                section = ""  # the next table started
        elif section == "gnu":
            match = _OBJDUMP_GNU_ROW.match(line)
            if match:
                names.append(match.group(1))
            else:
                section = ""
    return names


def find_export_tool(root: str = ROOT) -> Optional[str]:
    """The pinned LLVM tools first (the project's own msys2 clang64), then PATH."""
    bin_dir = os.path.join(root, "build", "msys64", "clang64", "bin")
    for name in ("llvm-readobj.exe", "llvm-readobj", "llvm-objdump.exe", "objdump.exe"):
        candidate = os.path.join(bin_dir, name)
        if os.path.isfile(candidate):
            return candidate
    for name in ("llvm-readobj", "llvm-objdump", "objdump"):
        found = shutil.which(name)
        if found:
            return found
    return None


def read_dll_exports(tool: str, dll: str) -> List[str]:
    base = os.path.basename(tool).lower()
    arguments = ["--coff-exports"] if base.startswith("llvm-readobj") else ["-p"]
    completed = subprocess.run([tool, *arguments, dll], check=True, capture_output=True, text=True,
                               encoding="utf-8", errors="replace")
    return parse_exports_dump(completed.stdout)


# ---- comparison --------------------------------------------------------------------------------------------


def compare(header: Sequence[str], other: Iterable[str], label: str, allow_extra: Iterable[str] = ()) -> List[str]:
    """Header-relative differences: declared-but-absent and present-but-undeclared (minus allowances)."""
    present = set(other)
    declared = set(header)
    allowed = set(allow_extra)
    problems = [f"missing in {label}: {name}" for name in header if name not in present]
    problems += [f"extra in {label}: {name}" for name in sorted(present - declared - allowed)]
    return problems


def default_header(root: str = ROOT) -> str:
    shipped = os.path.join(root, "include", "cengine", "cengine.h")
    return shipped if os.path.isfile(shipped) else os.path.join(root, "include", "cengine", "cengine_draft.h")


def check(header_text: str, header_name: str, def_text: Optional[str] = None, def_name: str = ".def",
          dll_names: Optional[Sequence[str]] = None, dll_label: str = "the DLL",
          allow_extra: Iterable[str] = ()) -> Tuple[ParsedNames, List[str]]:
    header = parse_header(header_text, header_name)
    problems = list(header.problems)
    if def_text is not None:
        definition = parse_def(def_text, def_name)
        problems += definition.problems
        problems += compare(header.names, definition.names, def_name, allow_extra)
    if dll_names is not None:
        problems += compare(header.names, dll_names, dll_label, allow_extra)
    return header, problems


def main(argv: Optional[Sequence[str]] = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--header", help="public header (default: include/cengine/cengine.h, else cengine_draft.h)")
    parser.add_argument("--def", dest="def_file", help="module-definition file to compare with the header")
    parser.add_argument("--dll", help="built DLL whose export table is compared with the header")
    parser.add_argument("--exports-dump", help="saved llvm-readobj / objdump output instead of reading --dll")
    parser.add_argument("--tool", help="llvm-readobj or objdump (default: the project's clang64, then PATH)")
    parser.add_argument("--allow-extra", action="append", default=[], metavar="NAME",
                        help="export that may exist without a declaration (repeatable)")
    parser.add_argument("--list", action="store_true", help="print the functions the header declares and exit")
    args = parser.parse_args(argv)

    header_path = args.header or default_header()
    try:
        with open(header_path, encoding="utf-8") as stream:
            header_text = stream.read()
        def_text = None
        if args.def_file:
            with open(args.def_file, encoding="utf-8") as stream:
                def_text = stream.read()
        dll_names: Optional[List[str]] = None
        if args.exports_dump:
            with open(args.exports_dump, encoding="utf-8", errors="replace") as stream:
                dll_names = parse_exports_dump(stream.read())
        elif args.dll:
            tool = args.tool or find_export_tool()
            if not tool:
                sys.stderr.write("check_cengine_exports: no llvm-readobj/objdump found; pass --tool\n")
                return 2
            dll_names = read_dll_exports(tool, args.dll)
    except (OSError, subprocess.CalledProcessError) as error:
        sys.stderr.write(f"check_cengine_exports: {error}\n")
        return 2

    header_name = os.path.relpath(header_path, ROOT).replace("\\", "/") if header_path.startswith(ROOT) else header_path
    header, problems = check(header_text, header_name, def_text, args.def_file or ".def", dll_names,
                             args.dll or args.exports_dump or "the DLL", args.allow_extra)
    if args.list:
        for name in header.names:
            print(name)
        return 1 if header.problems else 0
    if problems:
        sys.stderr.write(f"cengine export check failed ({len(problems)} problem(s)):\n")
        for problem in problems:
            sys.stderr.write(f"  {problem}\n")
        return 1
    compared = [label for label, present in (("def", def_text is not None), ("dll", dll_names is not None)) if present]
    print(f"cengine exports OK: {len(header.names)} function(s) in {header_name}"
          + (f", consistent with {' and '.join(compared)}" if compared else " (header only)"))
    return 0


if __name__ == "__main__":
    sys.exit(main())
