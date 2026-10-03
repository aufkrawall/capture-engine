"""Put declarations that a generator joined onto one line (`void A();void B();HRESULT C(...);`) one per line.

    python tools/refactor/split_jammed_declarations.py [--apply]

Splits only at a `;` that sits at parenthesis depth 0, outside strings and comments, and is directly followed by
an identifier character (the generator's join pattern). Whitespace-only: prove it with
`preprocess_fingerprint.py snapshot --normalize-whitespace` before and after.
"""

from __future__ import annotations

import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
JOIN = re.compile(r";[A-Za-z_]")


def split_line(line: str) -> list[str]:
    if not JOIN.search(line) or line.lstrip().startswith(("//", "/*", "*", "#")):
        return [line]
    indent = line[: len(line) - len(line.lstrip())]
    parts: list[str] = []
    depth = 0
    start = 0
    i = 0
    in_string = None
    while i < len(line):
        ch = line[i]
        if in_string:
            if ch == "\\":
                i += 2
                continue
            if ch == in_string:
                in_string = None
        elif ch in "\"'":
            in_string = ch
        elif line.startswith("//", i):
            break
        elif ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth = max(depth - 1, 0)  # a continuation line closes a parenthesis opened above it
        elif ch == ";" and depth == 0 and i + 1 < len(line) and (line[i + 1].isalpha() or line[i + 1] == "_"):
            parts.append(line[start:i + 1])
            start = i + 1
        i += 1
    parts.append(line[start:])
    return [parts[0]] + [indent + p.lstrip() for p in parts[1:]]


def main() -> int:
    apply_changes = "--apply" in sys.argv
    files = subprocess.run(["git", "ls-files", "*.h", "*.hpp"], cwd=ROOT, capture_output=True, text=True,
                           check=True).stdout.split()
    total = 0
    for path in files:
        if path.startswith("external/"):
            continue
        full = os.path.join(ROOT, path)
        text = open(full, encoding="utf-8", newline="").read()
        eol = "\r\n" if "\r\n" in text else "\n"
        lines = text.split(eol)
        out: list[str] = []
        changed = 0
        for line in lines:
            pieces = split_line(line)
            changed += len(pieces) - 1
            out.extend(pieces)
        if changed:
            total += changed
            print(f"{path}: +{changed} lines")
            if apply_changes:
                open(full, "w", encoding="utf-8", newline="").write(eol.join(out))
    print(f"{total} declarations moved onto their own line")
    return 0


if __name__ == "__main__":
    sys.exit(main())
