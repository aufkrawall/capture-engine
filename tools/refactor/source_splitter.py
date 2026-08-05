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
    cut = len(decl)
    for i, ch in enumerate(decl):
        if ch in "={":
            cut = i
            break
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
class Token:
    kind: str
    text: str
    line: int


def lex(text: str) -> List[Token]:
    """Tokenize C++ text; comments and string contents become opaque."""
    tokens: List[Token] = []
    i = 0
    n = len(text)
    line = 0
    while i < n:
        c = text[i]
        if c == "\n":
            line += 1
            i += 1
            continue
        if c in " \t\r":
            i += 1
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            if j < 0:
                raise RuntimeError("unterminated block comment")
            line += text.count("\n", i, j + 2)
            i = j + 2
            continue
        if text.startswith('R"', i) or text.startswith('LR"', i):
            j = text.find("(", i)
            if 0 <= j < i + 32 and "\\" not in text[i : j + 1] and text[i : j + 1].count('"') == 1:
                delim = text[i + 2 : j] if text.startswith('R"', i) else text[i + 3 : j]
                end = text.find(")" + delim + '"', j)
                if end < 0:
                    raise RuntimeError("unterminated raw string")
                line += text.count("\n", i, end + 1)
                i = end + len(delim) + 2
                continue
        if c == '"':
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == '"':
                    break
                j += 1
            if j >= n:
                raise RuntimeError("unterminated string literal")
            line += text.count("\n", i, j + 1)
            tokens.append(Token("OTHER", text[i : j + 1], line))
            i = j + 1
            continue
        if c == "'":
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == "'":
                    break
                j += 1
            if j >= n:
                raise RuntimeError("unterminated char literal")
            line += text.count("\n", i, j + 1)
            i = j + 1
            continue
        if c == "#":
            j = i
            while j < n and text[j] != "\n":
                if text[j] == "\\" and j + 1 < n and text[j + 1] == "\n":
                    j += 2
                    continue
                j += 1
            tokens.append(Token("DIR", text[i:j].strip(), line))
            line += text.count("\n", i, j)
            i = j
            continue
        if c in "{}();":
            tokens.append(Token(c, c, line))
            i += 1
            continue
        if c.isalpha() or c == "_":
            j = i + 1
            while j < n and (text[j].isalnum() or text[j] == "_"):
                j += 1
            tokens.append(Token("IDENT", text[i:j], line))
            i = j
            continue
        tokens.append(Token("OTHER", c, line))
        i += 1
    return tokens


@dataclass
class Chunk:
    kind: str  # func | var | class | namespace | enum | extern | pp_region | directive | other
    name: str
    start: int  # 0-based line
    end: int  # exclusive
    text: str
    ns_path: Tuple[str, ...] = ()
    anon_region: Optional[int] = None
    template: bool = False
    static: bool = False
    signature: str = ""


def _skip_template(tokens: Sequence[Token], idx: int) -> Tuple[int, bool]:
    if idx < len(tokens) and tokens[idx].text == "template":
        depth = 0
        i = idx
        while i < len(tokens):
            if tokens[i].text == "<":
                depth += 1
            elif tokens[i].text == ">":
                depth -= 1
                if depth == 0:
                    return i + 1, True
            i += 1
    return idx, False


def _last_ident(tokens: Sequence[Token]) -> str:
    for t in reversed(tokens):
        if t.kind == "IDENT":
            return t.text
    return ""


TYPE_KEYWORDS = {
    "static",
    "const",
    "constexpr",
    "consteval",
    "volatile",
    "inline",
    "thread_local",
    "extern",
    "unsigned",
    "signed",
    "long",
    "short",
    "struct",
    "class",
    "enum",
    "union",
    "typename",
    "auto",
    "void",
    "bool",
    "char",
    "wchar_t",
    "int",
    "float",
    "double",
    "nullptr",
    "true",
    "false",
    "NULL",
    "nullptr_t",
}


def _var_name(tokens: Sequence[Token]) -> str:
    """Name of a top-level variable statement: last identifier before the initializer."""
    cutoff = len(tokens)
    for i, t in enumerate(tokens):
        if t.text in ("=", "{", "["):
            cutoff = i
            break
    for t in reversed(tokens[:cutoff]):
        if t.kind == "IDENT" and t.text not in TYPE_KEYWORDS:
            return t.text
    return ""


def _func_name(pending: Sequence[Token]) -> str:
    for i, t in enumerate(pending):
        if t.text == "(" and i > 0:
            prev = pending[i - 1]
            if prev.kind == "IDENT" and prev.text != "operator":
                if i >= 2 and pending[i - 2].text == "~":
                    return "~" + prev.text
                return prev.text
            if prev.text == "operator":
                parts = ["operator"]
                for op in pending[i:]:
                    if op.text == "(":
                        break
                    parts.append(op.text)
                return "".join(parts)
    return _last_ident(pending)


def _looks_like_block_start(pending: Sequence[Token]) -> bool:
    """True when '{' starts a parameter-list block (function, lambda, ctor)."""
    last_close = -1
    for i, t in enumerate(pending):
        if t.text == ")":
            last_close = i
    if last_close < 0:
        return False
    depth = 0
    open_idx = -1
    for i in range(last_close, -1, -1):
        if pending[i].text == ")":
            depth += 1
        elif pending[i].text == "(":
            depth -= 1
            if depth == 0:
                open_idx = i
                break
    if open_idx < 0:
        return False
    prefix = pending[:open_idx]
    if any(t.text in ("=", "{") for t in prefix):
        return False
    if not any(t.kind == "IDENT" for t in prefix):
        return False
    tail = pending[last_close + 1 :]
    if not tail:
        return True
    for t in tail:
        if t.text in ("const", "noexcept", "override", "final", "volatile", "&", "*", "->", "<", ">", "::", "[", "]"):
            continue
        if t.kind == "IDENT":
            continue
        return False
    return True


def _classify_block(pending: Sequence[Token]) -> Tuple[str, str, bool]:
    """Classify a top-level '{...}' block from its leading tokens."""
    idx, is_tmpl = _skip_template(pending, 0)
    while idx < len(pending) and pending[idx].kind == "DIR":
        idx += 1
    is_extern_c = (
        idx < len(pending)
        and pending[idx].text == "extern"
        and idx + 1 < len(pending)
        and pending[idx + 1].kind == "OTHER"
        and pending[idx + 1].text.startswith('"')
    )
    if is_extern_c:
        idx += 2
    while (
        idx < len(pending)
        and pending[idx].text == "__declspec"
        and idx + 1 < len(pending)
        and pending[idx + 1].text == "("
    ):
        depth = 0
        i = idx + 1
        while i < len(pending):
            if pending[i].text == "(":
                depth += 1
            elif pending[i].text == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        idx = i + 1
    if idx < len(pending) and pending[idx].text in ("class", "struct"):
        name = pending[idx + 1].text if idx + 1 < len(pending) and pending[idx + 1].kind == "IDENT" else ""
        return "class", name, is_tmpl
    if idx < len(pending) and pending[idx].text == "namespace":
        if idx + 1 < len(pending) and pending[idx + 1].kind == "IDENT":
            parts = [pending[idx + 1].text]
            i = idx + 2
            while (
                i + 2 < len(pending)
                and pending[i].text == ":"
                and pending[i + 1].text == ":"
                and pending[i + 2].kind == "IDENT"
            ):
                parts.append(pending[i + 2].text)
                i += 3
            return "namespace", "::".join(parts), False
        return "namespace", "", False
    if idx < len(pending) and pending[idx].text == "enum":
        for t in pending[idx + 1 :]:
            if t.kind == "IDENT":
                return "enum", t.text, False
        return "enum", "", False
    if is_extern_c and idx >= len(pending):
        return "extern", "", False
    for i in range(idx, len(pending)):
        t = pending[i]
        if t.text == "(" and i > 0 and (
            pending[i - 1].kind == "IDENT" or pending[i - 1].text in (">", "~", ")", "=")
        ):
            return "func", _func_name(pending[idx:]), is_tmpl
    if is_extern_c:
        return "extern", "", False
    if any(t.text == "=" for t in pending):
        return "var", _last_ident(pending), False
    return "other", "", False


def _signature_of(chunk_text: str) -> str:
    depth = 0
    for idx, ch in enumerate(chunk_text):
        if ch == "{":
            if depth == 0:
                sig = re.sub(r"\s+", " ", chunk_text[:idx]).strip()
                sig = re.sub(r"\s*,\s*", ", ", sig)
                sig = re.sub(r"\s*;\s*", "; ", sig)
                return sig
            depth += 1
        elif ch == "}":
            depth -= 1
    return re.sub(r"\s+", " ", chunk_text).strip()


def _strip_defaults(sig: str) -> str:
    """Remove '= default' argument initializers from a function signature."""
    out: List[str] = []
    depth = 0
    skip = False
    i = 0
    n = len(sig)
    while i < n:
        ch = sig[i]
        if skip:
            if depth == 1 and ch in ",)":
                skip = False
                out.append(ch)
            elif ch == "(":
                depth += 1
            elif ch == ")":
                depth -= 1
            i += 1
            continue
        if ch == "(":
            depth += 1
            out.append(ch)
        elif ch == ")":
            depth -= 1
            out.append(ch)
        elif ch == "=" and depth == 1:
            skip = True
            if out and out[-1] == " ":
                out.pop()
            out.append(" ")
        else:
            out.append(ch)
        i += 1
    return "".join(out)


def _strip_definition_defaults(chunk_text: str) -> str:
    """Remove default argument initializers from a definition's signature."""
    comments, body = _split_comments(chunk_text)
    depth = 0
    for idx, ch in enumerate(body):
        if ch == "{":
            if depth == 0:
                sig = body[:idx]
                stripped = _strip_defaults(sig)
                return comments + stripped + body[idx:]
            depth += 1
        elif ch == "}":
            depth -= 1
    return chunk_text


def _find_matching_endif(tokens: Sequence[Token], start: int) -> int:
    depth = 0
    for i in range(start, len(tokens)):
        t = tokens[i]
        if t.kind != "DIR":
            continue
        m = COND_DIRECTIVE.match(t.text)
        if not m:
            continue
        d = m.group(1)
        if d in ("if", "ifdef", "ifndef"):
            depth += 1
        elif d == "endif":
            depth -= 1
            if depth == 0:
                return i
    raise RuntimeError("unterminated #if region")


class Scanner:
    def __init__(self, text: str) -> None:
        self.text = text
        self.lines = text.split("\n")
        self.chunks: List[Chunk] = []
        self.next_anon = 1

    def scan(self) -> List[Chunk]:
        self._scan_region(0, len(self.lines), ())
        return self.chunks

    def _slice(self, start_line: int, end_line: int) -> str:
        return "\n".join(self.lines[start_line:end_line])

    def _scan_region(self, start_line: int, end_line: int, ns_path: Tuple[str, ...],
                     anon: Optional[int] = None) -> None:
        tokens = lex(self._slice(start_line, end_line))
        pending: List[Token] = []
        pending_line = 0
        pending_is_dir = False
        anon_region = anon
        i = 0
        n = len(tokens)

        def emit(kind: str, name: str, p_line: int, e_line: int, sig: str = "", is_tmpl: bool = False,
                 is_static: bool = False, anon: Optional[int] = None) -> None:
            abs_start = start_line + p_line
            while abs_start > 0:
                stripped = self.lines[abs_start - 1].strip()
                if not (
                    stripped.startswith("//")
                    or stripped.startswith("/*")
                    or stripped.startswith("*")
                    or stripped.endswith("*/")
                ):
                    break
                abs_start -= 1
            self.chunks.append(
                Chunk(
                    kind=kind,
                    name=name,
                    start=abs_start,
                    end=start_line + e_line,
                    text=self._slice(abs_start, start_line + e_line),
                    ns_path=ns_path,
                    anon_region=anon_region,
                    template=is_tmpl,
                    static=is_static,
                    signature=sig,
                )
            )

        def flush_statement(stmt_line: Optional[int] = None) -> None:
            nonlocal pending, pending_is_dir
            if not pending:
                return
            start = pending_line
            end = stmt_line if stmt_line is not None else pending[-1].line
            if pending_is_dir:
                emit("directive", "", start, end + 1)
            else:
                is_static = any(t.text == "static" for t in pending)
                is_tmpl = pending[0].text == "template"
                is_decl_stmt = any(t.text in ("typedef", "using", "static_assert") for t in pending[:3])
                is_ns_alias = (
                    len(pending) >= 3
                    and pending[0].text == "namespace"
                    and pending[1].kind == "IDENT"
                    and pending[2].text == "="
                )
                first_paren = next((i for i, t in enumerate(pending) if t.text == "("), None)
                first_eq = next((i for i, t in enumerate(pending) if t.text == "="), None)
                first_param_literal = (
                    first_paren is not None
                    and first_paren + 1 < len(pending)
                    and (
                        pending[first_paren + 1].kind == "OTHER"
                        and (
                            pending[first_paren + 1].text[:1].isdigit()
                            or pending[first_paren + 1].text[:1] in ("'", '"')
                        )
                    )
                )
                is_func_decl = (
                    not is_decl_stmt
                    and not is_ns_alias
                    and not first_param_literal
                    and first_paren is not None
                    and first_paren > 0
                    and pending[first_paren - 1].kind == "IDENT"
                    and pending[first_paren - 1].text != "operator"
                    and (first_eq is None or first_eq > first_paren)
                )
            if (
                (any(t.text == "=" for t in pending) or is_static or anon_region is not None)
                and not is_decl_stmt
                and not is_ns_alias
                and not is_func_decl
            ):
                emit("var", _var_name(pending), start, end + 1, is_tmpl=is_tmpl, is_static=is_static)
            elif is_func_decl:
                sig = re.sub(r"\s+", " ", self._slice(start_line + start, start_line + end + 1)).strip()
                emit("func", _func_name(pending), start, end + 1, sig=sig, is_tmpl=is_tmpl,
                     is_static=is_static)
            else:
                name = (
                    _var_name(pending)
                    if any(t.text in ("{", "=", "[") for t in pending)
                    else _last_ident(pending)
                )
                emit("other", name, start, end + 1, is_tmpl=is_tmpl, is_static=is_static)
            pending = []
            pending_is_dir = False

        while i < n:
            t = tokens[i]
            if t.kind == "DIR":
                m = COND_DIRECTIVE.match(t.text)
                if m and not pending and m.group(1) in ("if", "ifdef", "ifndef"):
                    end_tok = _find_matching_endif(tokens, i)
                    end_line = tokens[end_tok].line
                    emit("pp_region", "", t.line, end_line + 1)
                    i = end_tok + 1
                    pending = []
                    pending_is_dir = False
                    continue
                if not pending and not m:
                    end_line = t.line + 1
                    while (
                        end_line - 1 < len(self.lines)
                        and self.lines[end_line - 1].rstrip().endswith("\\")
                    ):
                        end_line += 1
                    emit("directive", "", t.line, end_line)
                    i += 1
                    continue
                if pending:
                    pending.append(t)
                else:
                    pending = [t]
                    pending_line = t.line
                    pending_is_dir = True
                i += 1
                continue
            if t.text == ";":
                if pending:
                    flush_statement(t.line)
                i += 1
                continue
            if t.text == "{":
                if pending:
                    is_decl_block = any(
                        t2.text in ("class", "struct", "namespace", "enum", "union") for t2 in pending
                    ) or (
                        pending[0].text == "extern"
                        and any(t2.kind == "OTHER" and t2.text.startswith('"') for t2 in pending[1:3])
                    )
                    is_type_block = any(
                        t2.text in ("class", "struct", "enum", "union") for t2 in pending
                    )
                    if not _looks_like_block_start(pending) and not is_decl_block:
                        # braced-initializer statement: keep it in pending and flush at ';'
                        pending.append(t)
                        i += 1
                        continue
                    depth = 0
                    j = i
                    while j < n:
                        if tokens[j].kind == "DIR":
                            dm = COND_DIRECTIVE.match(tokens[j].text)
                            if dm and dm.group(1) in ("if", "ifdef", "ifndef"):
                                j = _find_matching_endif(tokens, j)
                            j += 1
                            continue
                        if tokens[j].text == "{":
                            depth += 1
                        elif tokens[j].text == "}":
                            depth -= 1
                            if depth == 0:
                                break
                        j += 1
                    if j >= n:
                        raise RuntimeError("unbalanced braces")
                    start = pending_line
                    end = tokens[j].line
                    trailing_ident = ""
                    if is_type_block:
                        k = j + 1
                        while k < n and tokens[k].text != ";":
                            k += 1
                        if k < n:
                            for t2 in tokens[j + 1 : k]:
                                if t2.kind == "IDENT" and t2.text not in TYPE_KEYWORDS:
                                    trailing_ident = t2.text
                            end = tokens[k].line
                            j = k
                    elif j + 1 < n and tokens[j + 1].text == ";":
                        end = tokens[j + 1].line
                        j += 1
                    kind, name, is_tmpl = _classify_block(pending)
                    is_static = any(t2.text == "static" for t2 in pending)
                    if is_type_block and trailing_ident:
                        # `static thread_local struct X {...} var;` defines a
                        # variable, not just a type; track it like one.
                        kind, name = "var", trailing_ident
                    if kind == "namespace":
                        open_abs = start_line + start
                        close_abs = start_line + end
                        inner_start = None
                        for ln in range(open_abs, close_abs):
                            if "{" in self.lines[ln]:
                                inner_start = ln + 1
                                break
                        if inner_start is None:
                            raise RuntimeError("namespace without brace")
                        child_ns = ns_path + (name,) if name else ns_path + ("",)
                        if name:
                            self._scan_region(inner_start, close_abs, child_ns, anon_region)
                        else:
                            anon = self.next_anon
                            self.next_anon += 1
                            self._scan_region(inner_start, close_abs, child_ns, anon)
                    else:
                        chunk_slice = self._slice(start_line + start, start_line + end + 1)
                        sig = _signature_of(chunk_slice) if kind == "func" else ""
                        emit(kind, name, start, end + 1, sig=sig, is_tmpl=is_tmpl, is_static=is_static)
                    i = j + 1
                    pending = []
                    pending_is_dir = False
                    continue
            if not pending:
                pending_line = t.line
            pending.append(t)
            pending_is_dir = pending_is_dir and t.kind == "DIR"
            i += 1
        if pending:
            flush_statement()


def scan(text: str) -> List[Chunk]:
    return Scanner(text).scan()


def _count_ident(text: str, name: str) -> int:
    return len(re.findall(r"\b" + re.escape(name) + r"\b", text))


def _wrap_ns(text: str, ns_path: Tuple[str, ...]) -> str:
    if not ns_path:
        return text
    open_braces = "\n".join("namespace {" if p == "" else f"namespace {p} {{" for p in ns_path)
    close_braces = "\n".join("}" for _ in ns_path)
    return f"{open_braces}\n{text}\n{close_braces}"


def split_source(facade: Path, grouping: Dict, dry_run: bool = False) -> None:
    """Split a logical source into .cpp units plus a generated internal header.

    The generated header receives: top-level non-conditional directives,
    using/typedef/extern statements, enums, template functions, classes (unless
    overridden), shared file-scope statics and shared static functions, both
    converted to `inline` and renamed with a module prefix so they cannot
    collide with same-named statics of other modules, and prototypes of every
    non-static top-level function. Chunks inside one #if/#endif region or one
    anonymous namespace are atomic: they must all land in the same unit.
    """
    text = reassemble(facade)
    chunks = scan(text)
    module = grouping["module"]
    header_name = grouping["header"]
    units: Dict[str, Dict] = grouping["units"]
    unit_names = list(units)
    assignment: Dict[int, str] = {}
    for unit_name, spec in units.items():
        for idx in spec.get("chunks", []):
            if idx in assignment:
                raise RuntimeError(f"chunk {idx} assigned twice")
            assignment[idx] = unit_name
    rest_unit = next((n for n, s in units.items() if s.get("rest")), unit_names[0])
    for idx in range(len(chunks)):
        if idx not in assignment:
            assignment[idx] = rest_unit

    # Anonymous-namespace integrity: all chunks of one anon region share a unit.
    allow_anon_split = set(grouping.get("allow_anon_split", []))
    by_anon: Dict[int, str] = {}
    for idx, c in enumerate(chunks):
        if c.anon_region is None or c.anon_region in allow_anon_split:
            continue
        prev = by_anon.get(c.anon_region)
        if prev is not None and prev != assignment[idx]:
            raise RuntimeError(f"anonymous namespace chunk {idx} split across units")
        by_anon[c.anon_region] = assignment[idx]

    unit_chunks: Dict[str, List[Chunk]] = {name: [] for name in unit_names}
    for idx, c in enumerate(chunks):
        unit_chunks[assignment[idx]].append(c)
    idx_by_id = {id(c): idx for idx, c in enumerate(chunks)}

    keep_in_units = set(grouping.get("keep_in_units", []))
    classes_in_units = set(grouping.get("classes_in_units", []))
    extern_in_units = set(grouping.get("extern_in_units", []))
    hoist_regions = set(grouping.get("hoist_regions", []))
    destatic = set(grouping.get("destatic", []))

    def hoisted_by_rule(idx: int) -> bool:
        c = chunks[idx]
        if idx in keep_in_units:
            return False
        if c.kind == "directive" and not c.ns_path:
            return True
        anon_scope = c.ns_path == ("",)
        if c.kind == "other" and (not c.ns_path or anon_scope):
            code = _code_text(c.text)
            if (
                USING_OR_TYPEDEF.match(code)
                or EXTERN_DECL.match(code)
                or FWD_DECL.match(code)
                or NS_ALIAS.match(code)
            ):
                return True
        if c.kind == "enum":
            return True
        if c.kind == "func" and c.template:
            return True
        if c.kind == "class" and idx not in classes_in_units:
            return True
        if c.kind == "extern" and idx not in extern_in_units:
            return True
        return False

    shared: set = set()
    changed = True
    while changed:
        changed = False
        for idx, c in enumerate(chunks):
            if (
                idx in shared
                or not c.name
                or c.kind not in ("var", "func")
                or c.name in TYPE_KEYWORDS
                or not re.fullmatch(r"[A-Za-z_]\w*", c.name)
            ):
                continue
            if (
                c.kind == "func"
                and not c.static
                and c.anon_region is None
                and not re.search(r"^\s*(inline|constexpr)\b", c.text)
            ):
                continue  # non-static functions get header prototypes, not renames
            if c.kind == "func" and idx in destatic:
                continue  # destatic functions stay in their unit as prototypes
            if (
                c.kind == "var"
                and not c.static
                and c.anon_region is None
                and not re.search(r"\b(constexpr|const)\b", c.text)
            ):
                continue  # mutable external globals stay in their unit
            if any(
                j != idx
                and (hoisted_by_rule(j) or j in shared or assignment[j] != assignment[idx])
                and _count_ident(chunks[j].text, c.name) > 0
                for j in range(len(chunks))
            ):
                shared.add(idx)
                changed = True

    # A non-static definition whose name matches a shared static declaration
    # had internal linkage in the original single-TU source (the earlier
    # `static` declaration governs). Hoist the definition as inline too so
    # every unit sees the same entity.
    static_decl_names = {
        chunks[i].name
        for i in shared
        if chunks[i].kind == "func" and "{" not in chunks[i].text and i not in destatic
    }
    if static_decl_names:
        for idx, c in enumerate(chunks):
            if (
                c.kind == "func"
                and c.name in static_decl_names
                and idx not in shared
                and any(
                    j != idx
                    and (hoisted_by_rule(j) or j in shared or assignment[j] != assignment[idx])
                    and _count_ident(chunks[j].text, c.name) > 0
                    for j in range(len(chunks))
                )
            ):
                shared.add(idx)

    shared_statics = [(idx, chunks[idx]) for idx in shared if chunks[idx].kind == "var"]
    shared_funcs = [(idx, chunks[idx]) for idx in shared if chunks[idx].kind == "func"]
    shared_idx = {i for i, _ in shared_statics} | {i for i, _ in shared_funcs}
    # Shared variables are renamed with a module prefix; shared functions keep
    # their names (as inline) so overload sets declared in other headers keep
    # resolving exactly as before.
    renames = {c.name: f"{module}_{c.name}" for _, c in shared_statics if c.name}
    define_rewrites = grouping.get("define_prefix_rewrites", [])

    def rename_text(text: str) -> str:
        for old, new in renames.items():
            text = _rename_ident(text, old, new)
        if define_rewrites:
            out_lines = []
            for line in text.split("\n"):
                for old_prefix, new_prefix in define_rewrites:
                    if old_prefix in line:
                        line = line.replace(old_prefix, new_prefix)
                out_lines.append(line)
            text = "\n".join(out_lines)
        return text

    def emit_shared(idx: int, c: Chunk) -> None:
        text = rename_text(c.text)
        comments, body = _split_comments(text)
        body = re.sub(r"^\s*static\s+", "", body, count=1)
        body = re.sub(r"^\s*inline\s+", "", body, count=1)
        ns = () if c.ns_path == ("",) else c.ns_path
        if c.kind == "var":
            header_parts.append(_wrap_ns(comments + "inline " + body, ns))
        elif body.lstrip().startswith("template"):
            header_parts.append(_wrap_ns(comments + body, ns))
        else:
            header_parts.append(_wrap_ns(comments + "inline " + body, ns))

    def unit_body_text(c: Chunk) -> str:
        text = rename_text(c.text)
        if idx_by_id[id(c)] in destatic:
            comments, body = _split_comments(text)
            body = re.sub(r"^\s*static\s+", "", body, count=1)
            text = comments + body
        if (
            c.kind == "func"
            and c.anon_region is None
            and (not c.static or idx_by_id[id(c)] in destatic)
        ):
            text = _strip_definition_defaults(text)
        return text

    header_parts: List[str] = ["#pragma once"]
    header_idx: set = set()

    # Pass 0: forward declarations of hoisted classes so earlier typedefs and
    # prototypes can reference them as incomplete types.
    for idx, c in enumerate(chunks):
        if idx in keep_in_units or idx in header_idx:
            continue
        if c.kind == "class" and idx not in classes_in_units:
            ns = c.ns_path
            prefix = "class" if _code_text(c.text).lstrip().startswith("class") else "struct"
            header_parts.append(_wrap_ns(f"{prefix} {c.name};", ns))

    # Pass 1: types and declarations in original order.
    for idx, c in enumerate(chunks):
        if idx in keep_in_units:
            continue
        if idx in header_idx:
            continue
        if c.kind == "directive" and not c.ns_path:
            if c.text.strip() != "#pragma once":
                header_parts.append(c.text)
            header_idx.add(idx)
        elif c.kind == "other":
            code = _code_text(c.text)
            if (
                USING_OR_TYPEDEF.match(code)
                or EXTERN_DECL.match(code)
                or FWD_DECL.match(code)
                or NS_ALIAS.match(code)
            ):
                header_parts.append(rename_text(c.text))
                header_idx.add(idx)
        elif c.kind == "enum":
            header_parts.append(_wrap_ns(rename_text(c.text), c.ns_path))
            header_idx.add(idx)
        elif c.kind == "pp_region" and (
            idx in hoist_regions or _is_pure_directive_region(c.text)
        ):
            header_parts.append(rename_text(c.text))
            header_idx.add(idx)

    # Pass 2: prototypes.
    for idx, c in enumerate(chunks):
        if idx in header_idx:
            continue
        if (
            c.kind != "func"
            or c.template
            or (c.static and idx not in destatic)
            or "" in c.ns_path
            or re.search(r"[A-Za-z_]\w*::[~A-Za-z_]\w*\s*\(", c.signature)
        ):
            continue
        if not c.signature:
            continue
        proto = rename_text(c.signature.rstrip().rstrip(";"))
        if idx in destatic:
            proto = re.sub(r"^\s*static\s+", "", proto, count=1)
        header_parts.append(_wrap_ns(proto + ";", c.ns_path))
        if "{" not in c.text:
            header_idx.add(idx)  # declaration-only: prototype replaces it in units

    # Pass 3: definitions in original order (templates, classes, extern blocks,
    # shared statics/functions at their original positions).
    for idx, c in enumerate(chunks):
        if idx in keep_in_units or idx in header_idx:
            continue
        if idx in shared_idx:
            emit_shared(idx, c)
            header_idx.add(idx)
        elif c.kind == "func" and c.template:
            header_parts.append(_wrap_ns(rename_text(c.text), c.ns_path))
            header_idx.add(idx)
        elif c.kind == "class" and idx not in classes_in_units:
            header_parts.append(_wrap_ns(rename_text(c.text), c.ns_path))
            header_idx.add(idx)
        elif c.kind == "extern" and idx not in extern_in_units:
            if not re.search(r"\)\s*\{", c.text):
                # Declaration-only extern block: safe to share via the header.
                header_parts.append(_wrap_ns(rename_text(c.text), c.ns_path))
                header_idx.add(idx)
            # Definition blocks (function bodies inside extern "C") stay in
            # their unit; hoisting them would duplicate the definitions.

    # Pass 4: extern declarations for mutable globals used from other units.
    # Emitted after shared statics so declarations that reference renamed
    # inline constants (e.g. array bounds) see their definitions.
    for idx, c in enumerate(chunks):
        if idx in header_idx or c.kind not in ("var", "other") or not c.name:
            continue
        if c.static or c.anon_region is not None or re.search(r"\b(constexpr|const)\b", c.text):
            continue
        if not any(
            j != idx
            and (hoisted_by_rule(j) or j in shared or assignment[j] != assignment[idx])
            and _count_ident(chunks[j].text, c.name) > 0
            for j in range(len(chunks))
        ):
            continue
        extern = _extern_decl(rename_text(c.text), c.name)
        if extern:
            header_parts.append(_wrap_ns(extern, c.ns_path))

    if dry_run:
        for name in unit_names:
            print(f"=== {name}: {len(unit_chunks[name])} chunks ===")
            for c in unit_chunks[name]:
                print(f"  {c.kind:<10} {c.name:<38} lines {c.start + 1}-{c.end}")
        print(f"=== header: {len(header_parts)} parts; renames: {sorted(renames)} ===")
        return

    out_dir = facade.parent
    (out_dir / header_name).write_text("\n\n".join(header_parts) + "\n", encoding="utf-8", newline="\n")
    for name in unit_names:
        body = "\n\n".join(
            _wrap_ns(unit_body_text(c), c.ns_path)
            for c in unit_chunks[name]
            if idx_by_id[id(c)] not in header_idx
        )
        unit_text = f'#include "{header_name}"\n\n{body}\n'
        (out_dir / name).write_text(unit_text, encoding="utf-8", newline="\n")
    for frag in grouping.get("delete", []):
        (out_dir / frag).unlink(missing_ok=True)
    print(f"split {module}: {len(chunks)} chunks -> {unit_names}")


def main(argv: List[str]) -> int:
    if len(argv) < 3:
        print(__doc__)
        return 1
    cmd = argv[1]
    facade = Path(argv[2])
    if cmd == "reassemble":
        out: Optional[Path] = None
        if len(argv) > 4 and argv[3] == "--out":
            out = Path(argv[4])
        text = reassemble(facade)
        if out:
            out.parent.mkdir(parents=True, exist_ok=True)
            out.write_text(text, encoding="utf-8", newline="\n")
            print(f"wrote {out} ({len(text.splitlines())} lines)")
        else:
            sys.stdout.write(text)
        return 0
    if cmd == "map":
        for i, c in enumerate(scan(reassemble(facade))):
            print(
                f"{i:4d} {c.kind:<10} {c.name:<38} lines {c.start + 1:6d}-{c.end:6d} "
                f"ns={'.'.join(c.ns_path) or '-'} anon={c.anon_region} tmpl={int(c.template)} "
                f"static={int(c.static)}"
            )
        return 0
    if cmd == "split":
        grouping = json.loads(Path(argv[3]).read_text(encoding="utf-8"))
        split_source(facade, grouping, dry_run=len(argv) > 4 and argv[4] == "--dry-run")
        return 0
    print(f"unknown command: {cmd}")
    return 1


if __name__ == "__main__":
    sys.exit(main(sys.argv))
