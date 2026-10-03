"""Remove compiler-proven dead code: unused static functions and side-effect-free unused variables.

    python tools/refactor/remove_unused.py <clang_tidy.log> [--apply]

Input is a lint run's clang_tidy.log (`clang-diagnostic-unused-function` / `-unused-variable` /
`-unused-const-variable`). An entity is removed only when all of these hold, otherwise it is listed for
manual review:
  - its name occurs exactly once in first-party sources and tests (so no #if branch of another
    architecture, no forward declaration and no source-policy test refers to it);
  - a function's body is found by brace matching from its definition line;
  - a variable's initializer is absent or a literal (removing a call could change behavior).
Paths from logs written before the 2026-10-02 relayout are mapped through build/relayout_mapping.json.
The proof that nothing else changed is the build plus tests; this script only does the bookkeeping.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys
from collections import defaultdict

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
WARNING = re.compile(
    r"(?P<path>[^\s:]+\.(?:cpp|h|hpp|inl)):(?P<line>\d+):(?P<col>\d+): warning: unused (?P<kind>function|variable)"
    r" '(?P<name>[^']+)' \[clang-diagnostic-unused-(?:function|variable|const-variable)\]")
LITERAL_INIT = re.compile(
    r"=\s*(?:-?[0-9][0-9a-fA-FxXuUlL.']*|true|false|nullptr|NULL|\{\s*\}|\{[^{}()]*\}|\"[^\"]*\")\s*;")


def rel(path: str) -> str:
    path = path.replace("\\", "/")
    marker = "captureproject/"
    if marker in path:
        path = path.split(marker, 1)[1]
    return path


def source_files() -> list[str]:
    out = subprocess.run(["git", "ls-files", "*.cpp", "*.h", "*.hpp", "*.inl", "*.c"], cwd=ROOT,
                         capture_output=True, text=True, check=True).stdout.split()
    return [f for f in out if not f.startswith("external/")]


def occurrence_counts(names: set[str]) -> dict[str, int]:
    counts: dict[str, int] = defaultdict(int)
    pattern = re.compile(r"\b(" + "|".join(re.escape(n) for n in sorted(names, key=len, reverse=True)) + r")\b")
    for f in source_files():
        with open(os.path.join(ROOT, f), encoding="utf-8", errors="replace") as fh:
            for match in pattern.finditer(fh.read()):
                counts[match.group(1)] += 1
    return counts


def strip_strings_and_comments(line: str) -> str:
    line = re.sub(r'"(?:\\.|[^"\\])*"', '""', line)
    line = re.sub(r"'(?:\\.|[^'\\])*'", "''", line)
    return line.split("//", 1)[0]


def function_extent(lines: list[str], index: int) -> tuple[int, int] | None:
    """Lines [start, end] of the function defined at `index` (0-based), or None."""
    depth = 0
    opened = False
    for i in range(index, min(len(lines), index + 400)):
        code = strip_strings_and_comments(lines[i])
        if not opened and ";" in code and "{" not in code:
            return None  # a declaration, not a definition
        for ch in code:
            if ch == "{":
                depth += 1
                opened = True
            elif ch == "}":
                depth -= 1
                if opened and depth == 0:
                    return index, i
    return None


def leading_block(lines: list[str], start: int) -> int:
    """Extend `start` upward over a contiguous comment block and template/attribute lines."""
    i = start
    while i > 0:
        previous = lines[i - 1].strip()
        if previous.startswith("//") or previous.startswith("template") or previous.startswith("[["):
            i -= 1
            continue
        break
    return i


def main() -> int:
    log = sys.argv[1]
    apply_changes = "--apply" in sys.argv
    mapping_path = os.path.join(ROOT, "build", "relayout_mapping.json")
    mapping = json.load(open(mapping_path, encoding="utf-8")) if os.path.exists(mapping_path) else {}
    findings = {}
    with open(log, encoding="utf-8", errors="replace") as fh:
        for match in WARNING.finditer(fh.read()):
            path = rel(match.group("path"))
            path = mapping.get(path, path)
            key = (path, int(match.group("line")), match.group("name"))
            findings[key] = match.group("kind")
    counts = occurrence_counts({name for (_p, _l, name) in findings})
    removals: dict[str, list[tuple[int, int, str]]] = defaultdict(list)
    review = []
    for (path, line, name), kind in sorted(findings.items()):
        full = os.path.join(ROOT, path)
        if not os.path.exists(full):
            review.append(f"{path}:{line} {name}: file missing")
            continue
        if counts.get(name, 0) != 1:
            review.append(f"{path}:{line} {kind} {name}: name occurs {counts.get(name, 0)}x")
            continue
        lines = open(full, encoding="utf-8", newline="").read().split("\n")
        index = line - 1
        if index >= len(lines) or name not in lines[index]:
            review.append(f"{path}:{line} {name}: line moved (log older than source)")
            continue
        if kind == "function":
            extent = function_extent(lines, index)
            if not extent:
                review.append(f"{path}:{line} function {name}: no body found")
                continue
            start, end = leading_block(lines, extent[0]), extent[1]
        else:
            statement = lines[index]
            end = index
            while ";" not in strip_strings_and_comments(lines[end]) and end < index + 5:
                end += 1
            statement = " ".join(part.strip() for part in lines[index:end + 1])
            if "=" in statement and not LITERAL_INIT.search(statement):
                review.append(f"{path}:{line} variable {name}: initializer may have effects: {statement[:100]}")
                continue
            if statement.count(";") != 1 or "(" in statement.split("=")[0] and "operator" not in statement:
                review.append(f"{path}:{line} variable {name}: not a single simple declaration: {statement[:100]}")
                continue
            start = leading_block(lines, index)
        removals[path].append((start, end, name))
    removed_lines = 0
    for path, spans in removals.items():
        full = os.path.join(ROOT, path)
        text = open(full, encoding="utf-8", newline="").read()
        newline = "\r\n" if "\r\n" in text else "\n"
        lines = text.split("\n")
        for start, end, name in sorted(spans, reverse=True):
            removed_lines += end - start + 1
            print(f"remove {path}:{start + 1}-{end + 1} {name}")
            del lines[start:end + 1]
            # Collapse a double blank line left behind.
            if 0 < start < len(lines) and not lines[start].strip("\r") and not lines[start - 1].strip("\r"):
                del lines[start]
        if apply_changes:
            with open(full, "w", encoding="utf-8", newline="") as fh:
                fh.write("\n".join(lines))
        _ = newline
    print(f"\n{sum(len(v) for v in removals.values())} entities, {removed_lines} lines "
          f"{'removed' if apply_changes else 'would be removed'}; {len(review)} left for review:")
    print("\n".join(review))
    return 0


if __name__ == "__main__":
    sys.exit(main())
