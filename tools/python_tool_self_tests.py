"""Parallel execution for build.py's Python tooling regression suites."""

from __future__ import annotations

import os
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor, as_completed
from dataclasses import dataclass
from typing import Dict, List, Mapping, Optional, Sequence


@dataclass(frozen=True)
class ToolSelfTestResult:
    name: str
    command: Sequence[str]
    returncode: int
    stdout: str
    stderr: str
    elapsed: float


def _commands(project_root: str, python_executable: str) -> List[tuple[str, List[str]]]:
    def unittest_command(module: str) -> List[str]:
        return [python_executable, "-m", "unittest", "-v", f"tools.tests.{module}"]

    def self_test_command(*relative_path: str) -> List[str]:
        return [python_executable, os.path.join(project_root, "tools", *relative_path), "--self-test"]

    return [
        ("ffmpeg_patch_utils", unittest_command("test_ffmpeg_patch_utils")),
        ("ffmpeg_dependencies", unittest_command("test_ffmpeg_dependencies")),
        ("build_flag_policy", unittest_command("test_build_flags")),
        ("build_gtest_link_inputs", unittest_command("test_build_gtest_link_inputs")),
        ("build_testapp_tasks", unittest_command("test_build_testapp_tasks")),
        ("pe_hardening_policy", unittest_command("test_pe_hardening")),
        ("clang_tidy_baseline_scope", unittest_command("test_clang_tidy_baseline")),
        ("clang_tidy_cache", unittest_command("test_clang_tidy_cache")),
        ("verification_parallelism", unittest_command("test_verification_parallelism")),
        ("verification_stage_cache", unittest_command("test_verification_stage_cache")),
        ("file_size_baseline", unittest_command("test_file_size_baseline")),
        ("build_lint_policy", unittest_command("test_build_lint_policy")),
        ("git_clean_paths", unittest_command("test_git_clean")),
        ("privacy_paths", unittest_command("test_privacy_paths")),
        ("build_packaging", unittest_command("test_packaging")),
        ("analyze_av_sync_stimulus", self_test_command("analysis", "analyze_av_sync_stimulus.py")),
        ("analyze_capture_av", self_test_command("analysis", "analyze_capture_av.py")),
        ("run_av_sync_matrix", self_test_command("analysis", "run_av_sync_matrix.py")),
    ]


def run_tool_self_tests(
    *,
    project_root: str,
    python_executable: str = sys.executable,
    env: Optional[Mapping[str, str]] = None,
    jobs: int,
) -> List[ToolSelfTestResult]:
    commands = _commands(project_root, python_executable)

    def run_one(item: tuple[str, List[str]]) -> ToolSelfTestResult:
        name, command = item
        started = time.time()
        result = subprocess.run(
            command,
            env=dict(env) if env is not None else None,
            cwd=project_root,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        return ToolSelfTestResult(
            name,
            command,
            result.returncode,
            result.stdout or "",
            result.stderr or "",
            time.time() - started,
        )

    results: Dict[str, ToolSelfTestResult] = {}
    with ThreadPoolExecutor(max_workers=max(1, min(jobs, len(commands)))) as executor:
        futures = [executor.submit(run_one, item) for item in commands]
        for future in as_completed(futures):
            result = future.result()
            results[result.name] = result
    return [results[name] for name, _ in commands]
