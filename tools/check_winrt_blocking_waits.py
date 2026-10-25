"""Fail on blocking waits for WinRT asynchronous operations outside the threads that may block.

C++/WinRT's `.get()` and `.wait_for()` on an IAsyncAction/IAsyncOperation block the calling thread until the
operation completes. Windows components written as C++/WinRT coroutines resume on the apartment that started them, so
a thread in a single-threaded apartment (the runtime's engine thread) that waits there without pumping deadlocks once
the answer arrives on another thread: the 2026-10-24 screenshot freeze (llm-wiki/wgc-capture.md). C++/WinRT flags such
a wait only with a debug-build WINRT_ASSERT, which release builds compile out, so this check stands in for it. Every
remaining wait is listed in MTA_OWNED with the reason its thread is in the multithreaded apartment.
"""

from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path
from typing import Iterable

ROOTS = ("captureengine", "common", "elevationservice", "frontend", "hook", "include", "installer", "mediaengine",
         "runtime")
SOURCE_SUFFIXES = {".c", ".cpp", ".h", ".hpp", ".inl"}

# Path -> why every blocking WinRT wait in it runs on a thread of the multithreaded apartment.
MTA_OWNED = {
    "captureengine/media/wgc_border_access.cpp":
        "AskWindows runs only on the request's own thread, which joins RO_INIT_MULTITHREADED first",
}

LITERALS = re.compile(
    r'(?:u8|u|U|L)?R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)"'
    r'|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|//[^\n]*|/\*.*?\*/',
    re.DOTALL,
)
# A call of a WinRT-style asynchronous method (RequestAccessAsync, ...); std::async is lower case and blocks nothing
# apartment-bound.
ASYNC_CALL = re.compile(r"\b\w*[a-z0-9_]Async\s*\(")
BLOCKING_SUFFIX = re.compile(r"\s*\.\s*(get|wait_for)\s*\(")
# `auto operation = SomethingAsync(...)`: the name later waited on. A co_await is not a blocking wait.
ASYNC_HANDLE = re.compile(r"\b(?P<name>[A-Za-z_]\w*)\s*(?:=|\{)\s*(?P<rest>[^;{}]*?)\b\w*[a-z0-9_]Async\s*\(")


def mask(text: str) -> str:
    """Blank comments and literals, keeping offsets and line numbers."""
    return LITERALS.sub(lambda match: re.sub(r"[^\n]", " ", match.group(0)), text)


def closing_paren(text: str, open_index: int) -> int:
    depth = 0
    for index in range(open_index, len(text)):
        if text[index] == "(":
            depth += 1
        elif text[index] == ")":
            depth -= 1
            if depth == 0:
                return index
    return -1


def blocking_waits(text: str) -> list[tuple[int, str]]:
    """(line, description) of every blocking wait on a WinRT asynchronous operation in C++ source text."""
    if "Async" not in text:
        return []
    code = mask(text)
    found: dict[int, str] = {}

    def line_of(offset: int) -> int:
        return code.count("\n", 0, offset) + 1

    for call in ASYNC_CALL.finditer(code):
        end = closing_paren(code, call.end() - 1)
        if end < 0:
            continue
        suffix = BLOCKING_SUFFIX.match(code, end + 1)
        if suffix:
            name = call.group(0).rstrip("( \t\r\n")
            found.setdefault(line_of(suffix.start(1)), f"{name}(...).{suffix.group(1)}()")

    for handle in ASYNC_HANDLE.finditer(code):
        if "co_await" in handle.group("rest"):
            continue
        name = handle.group("name")
        for wait in re.finditer(rf"\b{re.escape(name)}\s*\.\s*(get|wait_for)\s*\(", code[handle.end():]):
            offset = handle.end() + wait.start(1)
            found.setdefault(line_of(offset), f"{name}.{wait.group(1)}()")
    return sorted(found.items())


def source_files(root: Path) -> Iterable[Path]:
    for top in ROOTS:
        base = root / top
        if base.is_dir():
            yield from sorted(path for path in base.rglob("*") if path.is_file() and path.suffix in SOURCE_SUFFIXES)


def evaluate(root: Path, allowed: dict[str, str] | None = None) -> list[str]:
    allowed = MTA_OWNED if allowed is None else allowed
    problems: list[str] = []
    waiting: set[str] = set()
    for path in source_files(root):
        relative = path.relative_to(root).as_posix()
        waits = blocking_waits(path.read_text(encoding="utf-8-sig", errors="replace"))
        if not waits:
            continue
        waiting.add(relative)
        if relative in allowed:
            continue
        for line, what in waits:
            problems.append(
                f"{relative}:{line}: blocking wait on a WinRT asynchronous operation ({what}). On a single-threaded "
                "apartment such as the engine thread it deadlocks when the operation resumes on that apartment. "
                "Do not wait, or wait only on a thread this code owns in the multithreaded apartment and list the "
                "file in tools/check_winrt_blocking_waits.py MTA_OWNED with that reason.")
    for relative in sorted(set(allowed) - waiting):
        problems.append(f"{relative}: listed in MTA_OWNED but has no blocking WinRT wait any more; remove the entry.")
    return problems


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    args = parser.parse_args(argv)
    problems = evaluate(args.root)
    for problem in problems:
        print(problem)
    print(f"WinRT blocking waits: {'FAILED' if problems else 'passed'} ({len(MTA_OWNED)} MTA-owned file(s) listed)")
    return 1 if problems else 0


if __name__ == "__main__":
    sys.exit(main())
