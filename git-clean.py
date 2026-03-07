#!/usr/bin/env python3
"""Delete files ignored by .gitignore to keep the repository publish-ready."""

from __future__ import annotations

import argparse
import subprocess
import sys
from datetime import datetime
from pathlib import Path

from scripts.script_safety import PROJECT_ROOT, run_subprocess_checked

LOG_PATH = PROJECT_ROOT / "git-clean.log"


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Remove files and directories ignored by .gitignore using git clean."
    )
    parser.add_argument(
        "--dry-run",
        action="store_true",
        help="Show what would be deleted without removing anything.",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Suppress git clean output unless an error occurs.",
    )
    parser.add_argument(
        "--keep-log",
        action="store_true",
        help="Keep git-clean.log even when the command succeeds.",
    )
    return parser.parse_args()


def append_log(message: str) -> None:
    with LOG_PATH.open("a", encoding="utf-8") as log_file:
        log_file.write(f"[{datetime.now().isoformat(timespec='seconds')}] {message}\n")


def ensure_repo_root() -> None:
    result = run_subprocess_checked(["git", "rev-parse", "--show-toplevel"], cwd=PROJECT_ROOT)
    repo_root = Path(result.stdout.strip()).resolve()
    if repo_root != PROJECT_ROOT.resolve():
        raise RuntimeError(f"Expected repository root {PROJECT_ROOT}, got {repo_root}")


def build_git_clean_command(args: argparse.Namespace) -> list[str]:
    # Use double-force so ignored nested Git working trees inside build
    # directories are cleaned too.
    cmd = ["git", "clean", "-X", "-d", "-f", "-f"]
    if args.dry_run:
        cmd.append("-n")
    if args.quiet:
        cmd.append("-q")
    return cmd


def run_git_clean(cmd: list[str]) -> subprocess.CompletedProcess[str]:
    return subprocess.run(
        cmd,
        cwd=str(PROJECT_ROOT),
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
        check=False,
    )


def main() -> int:
    args = parse_args()
    if LOG_PATH.exists():
        LOG_PATH.unlink()

    append_log("Starting git-clean.py")

    try:
        ensure_repo_root()
        cmd = build_git_clean_command(args)
        append_log(f"Running command: {' '.join(cmd)}")
        result = run_git_clean(cmd)
    except RuntimeError as exc:
        append_log(f"ERROR: {exc}")
        append_log(f"Log retained at: {LOG_PATH}")
        print(f"ERROR: {exc}", file=sys.stderr)
        print(f"See log: {LOG_PATH}", file=sys.stderr)
        return 1

    output = result.stdout.strip()
    if output:
        print(output)
        append_log("STDOUT:")
        for line in output.splitlines():
            append_log(line)

    error_output = result.stderr.strip()
    if error_output:
        print(error_output, file=sys.stderr)
        append_log("STDERR:")
        for line in error_output.splitlines():
            append_log(line)

    if result.returncode != 0:
        append_log(f"git clean failed with exit code {result.returncode}")
        append_log(f"Log retained at: {LOG_PATH}")
        print(f"ERROR: git clean failed with exit code {result.returncode}", file=sys.stderr)
        print(f"See log: {LOG_PATH}", file=sys.stderr)
        return result.returncode

    if args.dry_run:
        print("Dry run complete.")
        append_log("Dry run complete.")
    else:
        print("Ignored files cleaned.")
        append_log("Ignored files cleaned.")

    if args.keep_log:
        append_log(f"Log kept at: {LOG_PATH}")
        print(f"Log kept at: {LOG_PATH}")
        return 0

    append_log("Success; deleting log file.")
    try:
        LOG_PATH.unlink(missing_ok=True)
    except OSError as exc:
        print(f"WARNING: cleaned ignored files, but could not delete log: {exc}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
