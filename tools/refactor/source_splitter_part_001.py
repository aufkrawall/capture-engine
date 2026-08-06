#!/usr/bin/env python3
"""Reassemble .inl source fragments and split logical sources into .cpp units.

Subcommands:
  reassemble <facade.cpp> [--out <path>]
      Inline every '#include "*.inl"' (recursively) into the facade text.
  map <facade.cpp>
      Print a structural map of the reassembled source: top-level chunks
      (functions, classes, variables, preprocessor regions, ...) with line
      ranges, names and namespace paths.
  split <facade.cpp> <grouping.json> [--dry-run]
      Apply a grouping of chunk indexes to target .cpp units, generate the
      module internal header, and delete the fragment .inl files.

The tool is intentionally conservative: it only moves whole balanced
constructs between files and never rewrites a chunk's text. The compiler is
the authority; run a build after every split.

Grouping schema (see split_source()):
{
  "module": "config",
  "header": "config_internal.h",
  "units": {
    "config.cpp":          {"chunks": [0, 1, 2], "rest": true},
    "config_profiles.cpp": {"chunks": [3, 4]}
  },
  "classes_in_units": [5],
  "keep_in_units": [6],
  "unscope_anon": [1],
  "unstatic": [7],
  "destatic": [8],
  "delete": ["config_part_001.inl", "config_part_002.inl"]
}
"""

from __future__ import annotations

import json
import re
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Dict, List, Optional, Sequence, Tuple


INL_INCLUDE = re.compile(r'^\s*#\s*include\s+"([^"]+\.inl)"')
COND_DIRECTIVE = re.compile(r"^\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b")
USING_OR_TYPEDEF = re.compile(r"^\s*(using|typedef)\b")
EXTERN_DECL = re.compile(r"^\s*extern\b")
FWD_DECL = re.compile(r"^\s*(struct|class|union|enum)\s+[A-Za-z_][\w:]*\s*;")
NS_ALIAS = re.compile(r"^\s*namespace\s+[A-Za-z_]\w*\s*=")
COMMENT_LINE = re.compile(r"^\s*(//|/\*|\*)")


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig").replace("\r\n", "\n").replace("\r", "\n")


def _code_text(text: str) -> str:
    """Strip leading comment lines so text-based classification sees the code."""
    lines = text.split("\n")
    idx = 0
    while idx < len(lines) and COMMENT_LINE.match(lines[idx]):
        idx += 1
    return "\n".join(lines[idx:])


def _split_comments(text: str) -> Tuple[str, str]:
    """Split leading comment lines from code; returns (comments, code)."""
    lines = text.split("\n")
    idx = 0
    while idx < len(lines) and COMMENT_LINE.match(lines[idx]):
        idx += 1
    comments = "\n".join(lines[:idx])
    return (comments + "\n" if comments else ""), "\n".join(lines[idx:])


def _is_pure_directive_region(text: str) -> bool:
    """True when a #if/#endif region contains only preprocessor lines."""
    for line in text.split("\n"):
        stripped = line.strip()
        if not stripped or stripped.startswith(("#", "//", "/*", "*")):
            continue
        return False
    return True


def _extern_decl(text: str, name: str) -> Optional[str]:
    """Build an extern declaration for a mutable global definition."""
    comments, body = _split_comments(text)
    decl = re.sub(r"^\s*(static|inline)\s+", "", body, count=1)
    decl = decl.strip()
    if re.search(r"[A-Za-z_]\w*::" + re.escape(name) + r"\b", decl):
        return None  # qualified name: out-of-line member definition, not a global
    name_pos = decl.find(name)
    cut = len(decl)
    for marker in ("=", "{", "("):
        pos = decl.find(marker, name_pos if marker == "(" else 0)
        if pos != -1 and pos < cut:
            cut = pos
    decl = decl[:cut]
    decl = decl.rstrip("; \t\r\n") + ";"
    if not re.search(r"\b" + re.escape(name) + r"\b", decl):
        return None
    return comments + "extern " + decl


def _rename_ident(text: str, old: str, new: str) -> str:
    """Replace an identifier outside string literals and comments."""
    pattern = re.compile(r"(?<![:.>])\b" + re.escape(old) + r"\b")
    out: List[str] = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(text[i:j])
            i = j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(text[i:j])
            i = j
            continue
        if text[i] in "\"'":
            quote = text[i]
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == quote:
                    j += 1
                    break
                j += 1
            out.append(text[i:j])
            i = j
            continue
        if text.startswith('R"', i) or text.startswith('LR"', i):
            j = text.find("(", i)
            if 0 <= j < i + 32 and "\\" not in text[i : j + 1] and text[i : j + 1].count('"') == 1:
                delim = text[i + 2 : j] if text.startswith('R"', i) else text[i + 3 : j]
                end = text.find(")" + delim + '"', j)
                end = n if end < 0 else end + len(delim) + 2
                out.append(text[i:end])
                i = end
                continue
        m = pattern.match(text, i)
        if m:
            out.append(new)
            i = m.end()
        else:
            out.append(text[i])
            i += 1
    return "".join(out)


def reassemble(facade: Path) -> str:
    """Inline nested .inl includes, preserving everything else verbatim."""

    def load(path: Path, seen: set) -> List[str]:
        key = str(path.resolve())
        if key in seen:
            raise RuntimeError(f"cyclic .inl include: {path}")
        seen = seen | {key}
        out: List[str] = []
        for line in read_text(path).split("\n"):
            m = INL_INCLUDE.match(line)
            if m:
                out.extend(load(path.parent / m.group(1), seen))
            else:
                out.append(line)
        return out

    return "\n".join(load(facade, set()))


@dataclass
