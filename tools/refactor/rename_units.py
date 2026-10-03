"""Rename identifiers and files together across code, tests and the wiki (log archives excluded).

    python tools/refactor/rename_units.py <mapping.json> [--apply]

mapping.json: {"identifiers": {"Old": "New", ...}, "files": {"old/path.cpp": "new/path.cpp", ...}}
Identifiers are replaced on word boundaries; file renames use `git mv` and every reference to the old
repo path (or bare basename) in sources, tests, tools and wiki is rewritten. History in
llm-wiki/log/archive-* is left as written. The build and the unit tests are the proof.
"""

from __future__ import annotations

import json
import os
import re
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
TEXT_SUFFIXES = (".cpp", ".h", ".hpp", ".inl", ".c", ".py", ".md", ".json", ".txt")


def tracked_text_files() -> list[str]:
    out = subprocess.run(["git", "ls-files"], cwd=ROOT, capture_output=True, text=True, check=True).stdout.split()
    return [f for f in out if f.endswith(TEXT_SUFFIXES) and not f.startswith(("external/", "llm-wiki/log/archive"))
            and f != "CHANGELOG.md" and not f.startswith("tools/refactor/")]


def main() -> int:
    mapping = json.load(open(sys.argv[1], encoding="utf-8"))
    apply_changes = "--apply" in sys.argv
    replacements: list[tuple[re.Pattern[str], str]] = []
    for old, new in mapping.get("identifiers", {}).items():
        replacements.append((re.compile(r"\b" + re.escape(old) + r"\b"), new))
    for old, new in mapping.get("files", {}).items():
        replacements.append((re.compile(r"(?<![A-Za-z0-9_])" + re.escape(old) + r"\b"), new))
        old_base, new_base = os.path.basename(old), os.path.basename(new)
        if old_base != new_base:
            replacements.append((re.compile(r"(?<![A-Za-z0-9_/])" + re.escape(old_base) + r"\b"), new_base))
    changed = 0
    for path in tracked_text_files():
        full = os.path.join(ROOT, path)
        with open(full, encoding="utf-8", errors="strict", newline="") as fh:
            try:
                text = fh.read()
            except UnicodeDecodeError:
                continue
        new_text = text
        for pattern, replacement in replacements:
            new_text = pattern.sub(replacement, new_text)
        if new_text != text:
            changed += 1
            print(f"edit {path}")
            if apply_changes:
                with open(full, "w", encoding="utf-8", newline="") as fh:
                    fh.write(new_text)
    for old, new in mapping.get("files", {}).items():
        print(f"move {old} -> {new}")
        if apply_changes:
            subprocess.run(["git", "mv", old, new], cwd=ROOT, check=True)
    print(f"{changed} files {'edited' if apply_changes else 'would change'}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
