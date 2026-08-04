"""Lint-stage orchestration kept outside the already oversized build.py."""

# build.py executes its fragments via exec, so its module attributes exist only
# at runtime; pyright cannot see them through the facade.
# pyright: reportAttributeAccessIssue=false

from __future__ import annotations

import json
import os
import re
import shutil
import subprocess
import sys
import time
from concurrent.futures import ThreadPoolExecutor
from typing import Any, Dict, List, Optional

from tools.clang_tidy_cache import analyze_warning_output, run_cached_clang_tidy


def run_lint(env, *, advisory: bool = False, build_module=None) -> bool:
    if build_module is None:
        # Standalone callers use the importable build module; build.py passes its
        # live module explicitly so execution as __main__ cannot create a second
        # verification context.
        import build as build_module
    b = build_module

    b.log("=== Running Linting ===")
    lint_start = time.time()
    checks_ok = True
    lint_details: Dict[str, Any] = {}
    clang_tidy_executor = None
    clang_tidy_future = None
    clang_tidy_scope: Optional[Dict[str, Any]] = None
    clang_tidy_command: List[str] = []
    clang_tidy = os.path.join(b.MSYS2_DIR, "clang64", "bin", "clang-tidy.exe")
    compile_commands_path = b.get_compile_commands_path()
    if b.IS_LINUX:
        clang_tidy = shutil.which("clang-tidy") or clang_tidy
    if clang_tidy and (b.IS_LINUX or os.path.exists(clang_tidy)) and os.path.exists(compile_commands_path):
        try:
            with open(compile_commands_path, "r", encoding="utf-8") as compile_db_file:
                compile_db_data = json.load(compile_db_file)
            lint_details["compile_database_entries"] = len(compile_db_data)
            lint_details["compile_database_sha256"] = b.sha256_file(compile_commands_path)
            clang_tidy_scope = b.clang_tidy_scope_from_entries(compile_db_data)
            b.record_verification_artifact("compile_commands", compile_commands_path)
            clang_tidy_command = [
                clang_tidy,
                "-extra-arg=-w",
                "-quiet",
                f"-p={os.path.dirname(compile_commands_path)}",
                "<content-addressed translation units>",
            ]
            clang_tidy_executor = ThreadPoolExecutor(max_workers=1)
            clang_tidy_future = clang_tidy_executor.submit(
                run_cached_clang_tidy,
                clang_tidy=clang_tidy,
                compile_database=compile_db_data,
                compile_database_dir=os.path.dirname(compile_commands_path),
                project_root=b.PROJECT_ROOT,
                cache_dir=b.CLANG_TIDY_CACHE_DIR,
                jobs=b.get_parallel_job_count(env, len(compile_db_data)),
                env=env,
            )
            b.log("Running content-addressed clang-tidy in parallel with other lint checks...")
        except Exception as error:
            lint_details["compile_database_error"] = str(error)

    clang_format = None
    if b.IS_WINDOWS:
        potential = [
            os.path.join(b.MSYS2_DIR, "clang64", "bin", "clang-format.exe"),
            shutil.which("clang-format"),
        ]
    else:
        potential = [shutil.which("clang-format"), "/usr/bin/clang-format"]
    for path in potential:
        if path and os.path.exists(path):
            clang_format = path
            break

    if clang_format:
        b.log("Running clang-format...")
        files = b.collect_lintable_cpp_sources()
        if files:
            chunk_size = 50
            issues_found = 0
            format_outputs: List[str] = []
            format_issue_files = set()
            for index in range(0, len(files), chunk_size):
                chunk = files[index : index + chunk_size]
                command = [
                    clang_format,
                    "--dry-run",
                    "-Werror",
                    f"--style=file:{os.path.join(b.PROJECT_ROOT, 'tools', 'config', '.clang-format')}",
                ] + chunk
                result = subprocess.run(command, capture_output=True, text=True, env=env)
                combined = "\n".join(part for part in (result.stdout, result.stderr) if part)
                if combined:
                    format_outputs.append(f"$ {subprocess.list2cmdline(command)}\n{combined}")
                    for line in combined.splitlines():
                        match = re.match(r"^(.+?):\d+:\d+:\s+(?:error|warning):", line)
                        if match:
                            format_issue_files.add(os.path.normpath(match.group(1)))
                if result.returncode != 0:
                    issues_found += 1
            b.write_verification_artifact(
                "clang_format_diagnostics",
                "clang_format.log",
                "\n\n".join(format_outputs) + ("\n" if format_outputs else "clang-format: no diagnostics\n"),
            )
            if issues_found:
                b.log(
                    f"WARNING: C++ style issues found in {len(format_issue_files)} file(s) "
                    f"across {issues_found} batch(es)."
                )
                b.log("Run 'python build.py --format' to fix them automatically.")
                checks_ok = False
                lint_details["clang_format_batches_with_issues"] = issues_found
                lint_details["clang_format_files_with_issues"] = len(format_issue_files)
            else:
                b.log("C++ Style: OK")
                lint_details["clang_format_batches_with_issues"] = 0
                lint_details["clang_format_files_with_issues"] = 0
    else:
        b.log("Error: clang-format not found.")
        checks_ok = False
        lint_details["clang_format_missing"] = True

    b.log("Checking source file sizes...")
    b.evaluate_file_size_baseline(b.collect_source_file_sizes(), lint_details)

    try:
        subprocess.run([sys.executable, "-m", "flake8", "--version"], capture_output=True, check=True)
        has_flake8 = True
    except Exception:
        has_flake8 = False
    if has_flake8:
        b.log("Running flake8...")
        command = [
            sys.executable,
            "-m",
            "flake8",
            f"--config={os.path.join(b.PROJECT_ROOT, 'tools', 'config', '.flake8')}",
            "build.py",
            "tools",
            "testapp",
        ]
        result = subprocess.run(command, capture_output=True, text=True)
        b.write_process_diagnostics_artifact("flake8_diagnostics", "flake8.log", command, result)
        if result.returncode:
            b.log_failure_output_tail(
                "flake8", "\n".join(part for part in (result.stdout, result.stderr) if part)
            )
            b.log("Python Style: FAILED")
            checks_ok = False
        else:
            b.log("Python Style: OK")
        lint_details["flake8_exit_code"] = result.returncode
    else:
        b.log("Error: flake8 not installed. (Run 'pip install flake8')")
        checks_ok = False
        lint_details["flake8_missing"] = True

    try:
        subprocess.run([sys.executable, "-m", "pyright", "--version"], capture_output=True, check=True)
        has_pyright = True
    except Exception:
        has_pyright = False
    if has_pyright:
        b.log("Running pyright...")
        command = [
            sys.executable,
            "-m",
            "pyright",
            "-p",
            os.path.join(b.PROJECT_ROOT, "tools", "config", "pyrightconfig.json"),
        ]
        result = subprocess.run(command, capture_output=True, text=True)
        b.write_process_diagnostics_artifact("pyright_diagnostics", "pyright.log", command, result)
        if result.returncode:
            b.log_failure_output_tail(
                "pyright", "\n".join(part for part in (result.stdout, result.stderr) if part)
            )
            b.log("Python Types: FAILED")
            checks_ok = False
        else:
            b.log("Python Types: OK")
        lint_details["pyright_exit_code"] = result.returncode
    else:
        b.log("Error: pyright not installed. (Run 'pip install pyright')")
        checks_ok = False
        lint_details["pyright_missing"] = True

    if clang_tidy_future is not None:
        try:
            cached = clang_tidy_future.result()
            completed = subprocess.CompletedProcess(
                clang_tidy_command,
                cached.returncode,
                stdout=cached.stdout,
                stderr=cached.stderr,
            )
            b.write_process_diagnostics_artifact(
                "clang_tidy_diagnostics", "clang_tidy.log", clang_tidy_command, completed
            )
            warnings, check_counts, subsystem_counts = analyze_warning_output(cached.stdout, b.PROJECT_ROOT)
            warning_count = len(warnings)
            lint_details.update(
                {
                    "clang_tidy_warnings": warning_count,
                    "clang_tidy_exit_code": cached.returncode,
                    "clang_tidy_cache_hits": cached.hits,
                    "clang_tidy_cache_misses": cached.misses,
                    "clang_tidy_cache_uncacheable": cached.uncacheable,
                    "clang_tidy_checks": dict(check_counts.most_common()),
                    "clang_tidy_subsystems": dict(subsystem_counts.most_common()),
                }
            )
            b.evaluate_clang_tidy_baseline(
                check_counts,
                lint_details,
                clang_tidy_scope if cached.returncode == 0 else None,
            )
            b.log(
                f"clang-tidy cache: {cached.hits} hit(s), {cached.misses} miss(es), "
                f"{cached.uncacheable} uncacheable"
            )
            if cached.returncode != 0 or warning_count:
                b.log(f"clang-tidy: {warning_count} warning(s) found (non-fatal)")
                if check_counts:
                    b.log(
                        "clang-tidy top checks: "
                        + ", ".join(f"{name}={count}" for name, count in check_counts.most_common(8))
                    )
                if subsystem_counts:
                    b.log(
                        "clang-tidy affected subsystems: "
                        + ", ".join(f"{name}={count}" for name, count in subsystem_counts.most_common(8))
                    )
                diagnostics_path = b.verification_artifact_path("clang_tidy.log")
                if diagnostics_path:
                    b.log(f"Complete clang-tidy diagnostics: {diagnostics_path}")
            else:
                b.log("clang-tidy: OK")
        except Exception as error:
            checks_ok = False
            lint_details["clang_tidy_cache_error"] = str(error)
            b.log(f"ERROR: clang-tidy cache execution failed: {error}")
        finally:
            assert clang_tidy_executor is not None
            clang_tidy_executor.shutdown()
    elif clang_tidy and (b.IS_LINUX or os.path.exists(clang_tidy)):
        b.log("Skipping clang-tidy (compile_commands.json missing or unreadable)")
        lint_details["clang_tidy_skipped"] = True
    else:
        b.log("clang-tidy not found. Install via MSYS2: pacman -S mingw-w64-clang-x86_64-clang-tools-extra")
        lint_details["clang_tidy_missing"] = True

    clang_tidy_has_findings = bool(lint_details.get("clang_tidy_warnings")) or bool(
        lint_details.get("clang_tidy_exit_code")
    )
    lint_status = "passed"
    if not checks_ok:
        lint_status = "warning" if advisory else "failed"
    elif clang_tidy_has_findings:
        lint_status = "warning"
    b.record_verification_step(
        "lint",
        lint_status,
        duration_seconds=time.time() - lint_start,
        details={**lint_details, "advisory": advisory},
    )
    return checks_ok
