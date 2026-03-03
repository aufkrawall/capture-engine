#!/usr/bin/env python3
"""Shared path/subprocess safety helpers for repository scripts."""

from __future__ import annotations

import os
import subprocess
import tempfile
from pathlib import Path
from typing import Optional, Sequence, Union

PROJECT_ROOT = Path(__file__).resolve().parents[1]


def resolve_repo_path(path: Union[str, Path]) -> Path:
    """Resolve a path and enforce that it stays under repository root."""
    candidate = Path(path)
    if not candidate.is_absolute():
        candidate = PROJECT_ROOT / candidate

    resolved = candidate.resolve()
    try:
        resolved.relative_to(PROJECT_ROOT)
    except ValueError as exc:
        raise ValueError(f"Path escapes repository root: {resolved}") from exc
    return resolved


def read_text_file(path: Union[str, Path], encoding: str = "utf-8") -> str:
    """Read a text file from within repository root."""
    return resolve_repo_path(path).read_text(encoding=encoding)


def write_text_atomic(
    path: Union[str, Path], content: str, encoding: str = "utf-8", newline: Optional[str] = None
) -> None:
    """Atomically write text to a repository file."""
    target = resolve_repo_path(path)
    target.parent.mkdir(parents=True, exist_ok=True)

    fd, temp_path = tempfile.mkstemp(prefix=f"{target.name}.", suffix=".tmp", dir=str(target.parent))
    try:
        with os.fdopen(fd, "w", encoding=encoding, newline=newline) as f:
            f.write(content)
        os.replace(temp_path, target)
    finally:
        if os.path.exists(temp_path):
            try:
                os.remove(temp_path)
            except OSError:
                pass


def run_subprocess_checked(
    args: Sequence[str],
    cwd: Optional[Union[str, Path]] = None,
    timeout: Optional[int] = None,
    text: bool = True,
) -> subprocess.CompletedProcess:
    """Run a subprocess and raise RuntimeError with captured output on failure."""
    working_dir = str(resolve_repo_path(cwd)) if cwd is not None else None
    result = subprocess.run(list(args), cwd=working_dir, capture_output=True, text=text, timeout=timeout, check=False)
    if result.returncode != 0:
        raise RuntimeError(
            f"Command failed ({result.returncode}): {' '.join(args)}\n"
            f"STDOUT:\n{result.stdout}\nSTDERR:\n{result.stderr}"
        )
    return result
