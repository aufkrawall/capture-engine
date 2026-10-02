"""Fast compile check of selected translation units from compile_commands.json.

    python tools/refactor/syntax_check.py <file-or-substring> [...]
    python tools/refactor/syntax_check.py --changed        # every TU whose source git reports as modified

Runs each matching entry with -fsyntax-only (no objects, no build state touched), in parallel.
Header edits are covered by naming a TU that includes the header. Use it between edits of
product code the --tests-only loop does not compile; the closing gate stays authoritative.
"""

from __future__ import annotations

import concurrent.futures
import json
import os
import subprocess
import sys

ROOT = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def rel(path: str) -> str:
    return os.path.normpath(os.path.relpath(path, ROOT)).replace("\\", "/")


def syntax_only(entry: dict) -> tuple[str, int, str]:
    args = list(entry.get("arguments") or entry["command"].split())
    out: list[str] = []
    skip = False
    for a in args:
        if skip:
            skip = False
            continue
        if a in ("-o", "-MF", "-MT", "-MQ"):
            skip = True
            continue
        if a in ("-c", "-MD", "-MMD") or (a.startswith("-o") and len(a) > 2) or a.startswith("-flto"):
            continue
        out.append(a)
    out.append("-fsyntax-only")
    proc = subprocess.run(out, cwd=entry.get("directory", ROOT), capture_output=True, text=True, errors="replace")
    return rel(os.path.join(entry.get("directory", ROOT), entry["file"])), proc.returncode, proc.stderr


def main() -> int:
    patterns = sys.argv[1:]
    db = json.load(open(os.path.join(ROOT, "compile_commands.json"), encoding="utf-8"))
    if patterns == ["--changed"]:
        changed = subprocess.run(["git", "diff", "--name-only", "HEAD"], cwd=ROOT, capture_output=True,
                                 text=True).stdout.split()
        patterns = [p for p in changed if p.endswith((".cpp", ".c"))]
    selected = []
    seen = set()
    for entry in db:
        source = rel(os.path.join(entry.get("directory", ROOT), entry["file"]))
        if source in seen:
            continue
        if any(p in source for p in patterns):
            seen.add(source)
            selected.append(entry)
    failures = 0
    with concurrent.futures.ThreadPoolExecutor(max_workers=os.cpu_count() or 8) as pool:
        for source, code, stderr in pool.map(syntax_only, selected):
            diagnostics = [line for line in stderr.splitlines() if "warning:" in line or "error:" in line]
            if code != 0 or diagnostics:
                failures += code != 0
                print(f"== {source} (exit {code})")
                print("\n".join(diagnostics[:20]))
    print(f"checked {len(selected)} TU(s), {failures} failed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
