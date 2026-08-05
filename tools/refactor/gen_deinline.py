#!/usr/bin/env python3
"""De-inline class members from internal headers into out-of-line .cpp units.

Keeps the class declaration skeleton (member declarations with ';') in the
header and moves every member-function *body* into one or more semantic
`*_impl*.cpp` units as qualified out-of-line definitions. Units stay below the
project's 800-line ceiling (working target ~700).

Usage:
  gen_deinline.py <header.h> <unit-base> [--target 700] [--classes A,B]
"""

from __future__ import annotations

import re
import sys
from dataclasses import dataclass, field
from pathlib import Path


def read_text(path: Path) -> str:
    return path.read_text(encoding="utf-8-sig").replace("\r\n", "\n").replace("\r", "\n")


class Scanner:
    """Character-level C++ scanner that skips comments and string literals."""

    def __init__(self, text: str):
        self.text = text
        self.n = len(text)

    def skip_ws_and_comments(self, i: int) -> int:
        while i < self.n:
            ch = self.text[i]
            if ch in " \t\r\n":
                i += 1
            elif self.text.startswith("//", i):
                j = self.text.find("\n", i)
                i = self.n if j < 0 else j + 1
            elif self.text.startswith("/*", i):
                j = self.text.find("*/", i + 2)
                i = self.n if j < 0 else j + 2
            else:
                break
        return i

    def skip_string(self, i: int) -> int:
        quote = self.text[i]
        i += 1
        while i < self.n:
            ch = self.text[i]
            if ch == "\\":
                i += 2
                continue
            if ch == quote:
                return i + 1
            if ch == "\n" and quote == "'":
                return i
            i += 1
        return i

    def match_brace(self, open_idx: int) -> int:
        """Return index just past the '}' matching the '{' at open_idx."""
        depth = 0
        i = open_idx
        while i < self.n:
            ch = self.text[i]
            if ch == '"' or ch == "'":
                i = self.skip_string(i)
                continue
            if self.text.startswith("//", i):
                j = self.text.find("\n", i)
                i = self.n if j < 0 else j + 1
                continue
            if self.text.startswith("/*", i):
                j = self.text.find("*/", i + 2)
                i = self.n if j < 0 else j + 2
                continue
            if ch == "{":
                depth += 1
            elif ch == "}":
                depth -= 1
                if depth == 0:
                    return i + 1
            i += 1
        raise SystemExit("unbalanced braces")


@dataclass
class MemberDef:
    start: int
    body_open: int
    body_close: int
    head: str
    body: str
    name: str


@dataclass
class ClassInfo:
    name: str
    start: int
    body_open: int
    body_close: int
    members: list = field(default_factory=list)


TYPE_KEYWORDS = ("struct", "class", "enum", "union", "using", "typedef", "friend", "namespace")
ACCESS_RE = re.compile(r"^\s*(public|private|protected)\s*:")


def find_top_level_classes(text: str, sc: Scanner, only: set | None) -> list[ClassInfo]:
    classes: list[ClassInfo] = []
    i = 0
    n = len(text)
    while i < n:
        i = sc.skip_ws_and_comments(i)
        if i >= n:
            break
        m = re.match(r"(class|struct)\s+([A-Za-z_]\w*(?:::[A-Za-z_]\w*)*)", text[i:])
        if not m:
            nxt = text.find("\n", i)
            i = n if nxt < 0 else nxt + 1
            continue
        name = m.group(2)
        # Skip any inheritance/base-clause up to the '{' (or ';' for a forward decl).
        j = i + m.end()
        after = None
        while j < n:
            j = sc.skip_ws_and_comments(j)
            if j >= n:
                break
            ch = text[j]
            if ch == "{":
                after = j
                break
            if ch == ";":
                break
            j += 1
        if after is None:
            i = j + 1
            continue
        body_open = after
        body_close = sc.match_brace(body_open)
        if only is None or name in only:
            classes.append(ClassInfo(name=name, start=i, body_open=body_open, body_close=body_close))
        i = body_close
    return classes


def extract_member_definitions(text: str, sc: Scanner, cls: ClassInfo) -> None:
    """Fill cls.members with member-function definitions found in the class body."""
    i = cls.body_open + 1
    end = cls.body_close - 1
    depth = 1
    last_term = cls.body_open + 1
    while i < end:
        ch = text[i]
        if ch == '"' or ch == "'":
            i = sc.skip_string(i)
            continue
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = end if j < 0 else j + 1
            continue
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            i = end if j < 0 else j + 2
            continue
        if ch == "{" and depth == 1:
            stmt = text[last_term:i]
            stripped = stmt.strip()
            if stripped and not stripped.startswith(TYPE_KEYWORDS) and "(" in stripped:
                close = sc.match_brace(i)
                after = sc.skip_ws_and_comments(close)
                if after >= end or text[after] not in ",)]{":
                    head = stmt
                    name = method_name(head)
                    if name:
                        cls.members.append(
                            MemberDef(
                                start=last_term,
                                body_open=i,
                                body_close=close,
                                head=head,
                                body=text[i + 1 : close - 1],
                                name=name,
                            )
                        )
                        i = close
                        last_term = close
                        continue
            depth += 1
            i += 1
            continue
        if ch == "{":
            depth += 1
            i += 1
            continue
        if ch == "}":
            depth -= 1
            if depth == 1:
                last_term = i + 1
            i += 1
            continue
        if ch == ";" and depth == 1:
            last_term = i + 1
        i += 1


def method_name(head: str) -> str | None:
    h = re.sub(r"^\s*template\s*<.*?>\s*", "", head, flags=re.S)
    h = re.sub(r"^\s*(static|inline|virtual|explicit|constexpr|consteval|friend)\s+", "", h, count=1)
    h = re.sub(r"^\s*(static|inline|virtual|explicit|constexpr|consteval|friend)\s+", "", h, count=1)
    h = h.strip()
    if not h:
        return None
    if "operator" in h:
        opm = re.search(r"operator\s*([^\s(]+)", h)
        if opm:
            return "operator" + opm.group(1).strip()
    m = re.search(r"([A-Za-z_~]\w*)\s*\([^;]*$", h, flags=re.S)
    if not m:
        return None
    return m.group(1)


def strip_defaults(params: str) -> str:
    out: list[str] = []
    depth = 0
    i = 0
    part_start = 0
    n = len(params)
    while i < n:
        ch = params[i]
        if ch in "([{<":
            depth += 1
        elif ch in ")]}>":
            depth -= 1
        elif ch == "," and depth == 0:
            out.append(trim_default(params[part_start:i]))
            part_start = i + 1
        i += 1
    out.append(trim_default(params[part_start:]))
    return ", ".join(p for p in out if p.strip())


def trim_default(part: str) -> str:
    depth = 0
    i = 0
    n = len(part)
    while i < n:
        ch = part[i]
        if ch in "([{<":
            depth += 1
        elif ch in ")]}>":
            depth -= 1
        elif ch == "=" and depth == 0:
            return part[:i].rstrip()
        i += 1
    return part.rstrip()


def clean_prefix(prefix: str) -> str:
    """Strip leading comments, access specifiers and definition-only keywords."""
    lines = []
    for line in prefix.split("\n"):
        s = line.strip()
        if s.startswith(("//", "/*", "*")):
            continue
        if ACCESS_RE.match(s):
            continue
        lines.append(line)
    out = "\n".join(lines).strip()
    out = re.sub(r"^\s*(static|inline|virtual|explicit|friend)\s+", "", out)
    return out.strip()


def split_head(head: str) -> tuple[str, str, str, str, str]:
    """Return (ret, name, params, tail, init_list)."""
    depth = 0
    open_idx = -1
    for i, ch in enumerate(head):
        if ch in "([{<":
            depth += 1
            if ch == "(":
                open_idx = i
        elif ch in ")]}>":
            depth -= 1
    if open_idx < 0:
        raise SystemExit(f"no parameter list in head: {head[:80]!r}")
    d = 0
    close_idx = len(head) - 1
    for i in range(open_idx, len(head)):
        if head[i] == "(":
            d += 1
        elif head[i] == ")":
            d -= 1
            if d == 0:
                close_idx = i
                break
    prefix = clean_prefix(head[:open_idx])
    name = method_name(head) or ""
    ret = prefix
    if name:
        if "operator" in name:
            op_pos = ret.find("operator")
            ret = ret[:op_pos].strip()
        else:
            ret = re.sub(r"\s*[A-Za-z_~]\w*\s*$", "", ret).strip()
    params = head[open_idx + 1 : close_idx]
    tail = head[close_idx + 1 :].strip()
    init_list = ""
    colon = tail.find(":")
    if colon >= 0 and not tail.startswith("::"):
        init_list = tail[colon:].strip()
        tail = tail[:colon].strip()
    return ret, name, params, tail, init_list


def declaration_for(m: MemberDef) -> str:
    ret, name, params, tail, init_list = split_head(m.head)
    tail = re.sub(r"\s+", " ", tail).strip()
    decl = (ret + " " if ret else "") + name + "(" + params + ")"
    if tail:
        decl += " " + tail
    return decl + ";"


def definition_for(m: MemberDef, cls_name: str) -> str:
    ret, name, params, tail, init_list = split_head(m.head)
    tail = re.sub(r"\b(override|final)\b", "", tail)
    tail = re.sub(r"\s+", " ", tail).strip()
    params = strip_defaults(params)
    if init_list:
        return f"{ret + ' ' if ret else ''}{cls_name}::{name}({params}) {init_list} {{"
    defn = f"{ret + ' ' if ret else ''}{cls_name}::{name}({params})"
    if tail:
        defn += " " + tail
    return defn + " {"


def rebuild_class_body(text: str, cls: ClassInfo) -> str:
    out: list[str] = []
    cursor = cls.body_open + 1
    for m in sorted(cls.members, key=lambda x: x.start):
        out.append(text[cursor : m.start])
        leading = re.match(r"^\s*", m.head).group(0)
        out.append(leading + declaration_for(m))
        cursor = m.body_close
    out.append(text[cursor : cls.body_close - 1])
    return "".join(out)


def main() -> int:
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    header = Path(sys.argv[1])
    unit_base = Path(sys.argv[2])
    target = 700
    only = None
    for arg in sys.argv[3:]:
        if arg.startswith("--target="):
            target = int(arg.split("=", 1)[1])
        elif arg.startswith("--classes="):
            only = set(arg.split("=", 1)[1].split(","))
    text = read_text(header)
    sc = Scanner(text)
    classes = find_top_level_classes(text, sc, only)
    if not classes:
        print("no classes found")
        return 2
    for cls in classes:
        extract_member_definitions(text, sc, cls)
    new_text = text
    for cls in sorted(classes, key=lambda c: c.start, reverse=True):
        body = rebuild_class_body(text, cls)
        new_text = new_text[: cls.body_open + 1] + body + new_text[cls.body_close - 1 :]
    header.write_text(new_text, encoding="utf-8", newline="\n")
    total = sum(len(c.members) for c in classes)
    print(f"header {header}: {len(classes)} classes, {total} member definitions extracted")

    units: list[tuple[str, list[tuple[ClassInfo, MemberDef]]]] = []
    cur: list[tuple[ClassInfo, MemberDef]] = []
    cur_size = 0
    for cls in classes:
        for m in sorted(cls.members, key=lambda x: x.start):
            size = text.count("\n", m.start, m.body_close) + 1
            if cur and cur_size + size > target:
                units.append((f"{unit_base}.cpp" if len(units) == 0 else f"{unit_base}_{len(units) + 1}.cpp", cur))
                cur = []
                cur_size = 0
            cur.append((cls, m))
            cur_size += size
    if cur:
        units.append((f"{unit_base}.cpp" if len(units) == 0 else f"{unit_base}_{len(units) + 1}.cpp", cur))

    include = f'#include "{header.name}"\n'
    for path, members in units:
        parts = [include]
        for cls, m in members:
            parts.append(definition_for(m, cls.name))
            parts.append(m.body.rstrip())
            parts.append("}")
        unit_path = Path(path)
        unit_path.parent.mkdir(parents=True, exist_ok=True)
        unit_path.write_text("\n\n".join(parts) + "\n", encoding="utf-8", newline="\n")
        lines = sum(text.count("\n", m.start, m.body_close) + 1 for _, m in members)
        print(f"wrote {unit_path} ({lines} lines)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
