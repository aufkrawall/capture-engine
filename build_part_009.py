

def run_tests(env, test_exe, gtest_filter=None, run_python_tools=True):
    log("=== Running Unit Tests ===")
    if not os.path.exists(test_exe):
        log("Error: Test executable not found.")
        return False

    # Ensure required DLLs are on PATH (libgtest.dll, FFmpeg DLLs)
    msys_bin = os.path.join(get_host_msys2_dir(), "clang64", "bin")
    ffmpeg_dir = os.path.join(PROJECT_ROOT, "installed", "captureengine", "ffmpeg")
    test_env = dict(env)
    test_env["PATH"] = ffmpeg_dir + os.pathsep + msys_bin + os.pathsep + test_env.get("PATH", "")

    if IS_LINUX:
        wine_exe = shutil.which("wine64") or shutil.which("wine")
        if not wine_exe:
            log("Error: Running unit_tests.exe on Linux requires Wine in PATH.")
            return False
        cmd = [wine_exe, test_exe]
    else:
        cmd = [test_exe]
    if gtest_filter:
        cmd.append(f"--gtest_filter={gtest_filter}")
        log(f"Applying unit test filter: {gtest_filter}")
    log("Launching unit_tests.exe...")
    start = time.time()
    result = subprocess.run(
        cmd,
        env=test_env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    elapsed = time.time() - start
    log(f"unit_tests.exe finished in {elapsed:.1f}s")
    record_verification_artifact("unit_test_exe", test_exe)
    if result.returncode != 0:
        failure_path = record_unit_test_failure_output(cmd, result)
        log(f"=== Unit Tests FAILED (exit code {result.returncode}) ===")
        if failure_path:
            log(f"Unit-test diagnostics saved to {failure_path}")
        log_unit_test_output_tail("stdout", result.stdout)
        log_unit_test_output_tail("stderr", result.stderr)
        record_verification_step(
            "unit_tests",
            "failed",
            duration_seconds=elapsed,
            details={"exit_code": result.returncode, "gtest_filter": gtest_filter},
        )
        return False

    log("=== Unit Tests Passed ===")
    record_verification_step(
        "unit_tests",
        "passed",
        duration_seconds=elapsed,
        details={"exit_code": result.returncode, "gtest_filter": gtest_filter},
    )
    if gtest_filter or not run_python_tools:
        if not run_python_tools:
            log("Skipping duplicate Python tool self-tests in sanitizer child")
            record_verification_step("python_tool_self_tests", "skipped", details={"reason": "sanitizer_child"})
        return True
    return run_python_tool_self_tests(env)


def run_python_tool_self_tests(env):
    log("=== Running Python Tool Self-Tests ===")
    results = run_tool_self_tests(
        project_root=PROJECT_ROOT,
        python_executable=sys.executable,
        env=env,
        jobs=get_parallel_job_count(env, 14),
    )
    ok = True
    for result in results:
        record_verification_step(
            f"python_tool_self_test.{result.name}",
            "passed" if result.returncode == 0 else "failed",
            duration_seconds=result.elapsed,
            details={"exit_code": result.returncode, "command": result.command},
        )
        if result.stdout:
            log(f"[python_tool_self_test:{result.name}:stdout]\n{result.stdout.rstrip()}", detail=True)
        if result.stderr:
            log(
                f"[python_tool_self_test:{result.name}:stderr]\n{result.stderr.rstrip()}",
                detail=result.returncode == 0,
            )
        if result.returncode != 0:
            log(f"Python tool self-test failed: {result.name} (exit code {result.returncode})")
            ok = False
    if ok:
        log("=== Python Tool Self-Tests Passed ===")
    else:
        log("=== Python Tool Self-Tests FAILED ===")
    return ok


def run_integration_tests(env, full_matrix=False):
    mode = "Full Matrix" if full_matrix else "Smoke"
    log(f"=== Running Integration Tests ({mode}) ===")
    script = os.path.join(PROJECT_ROOT, "testapp", "run_tests.py")
    if not os.path.exists(script):
        log(f"Error: {script} not found.")
        return

    logs_dir = os.path.join(PROJECT_ROOT, "installed", "captureengine", "logs")
    os.makedirs(logs_dir, exist_ok=True)

    base_cmd = [
        sys.executable,
        script,
        "--duration",
        "5",
        "--tests",
        "1",
        "--min-frames",
        "60",
        "--target-fps",
        "120",
        "--min-frame-ratio",
        "0.60",
        "--max-avg-frame-ratio",
        "1.35",
        "--max-frame-spike-ratio",
        "4.0",
        "--max-spike-pct",
        "5.0",
    ]

    if full_matrix:
        targets = [
            ("full_matrix", ["--api", "all", "--arch", "both"]),
        ]
    else:
        targets = [
            ("smoke_dx9", ["--api", "dx9", "--arch", "both"]),
            ("smoke_dx11", ["--api", "dx11", "--arch", "x64"]),
            ("smoke_modern", ["--api", "both", "--arch", "x64"]),
        ]

    for label, args in targets:
        result_json = os.path.join(logs_dir, f"integration_{label}.json")
        cmd = base_cmd + args + ["--results-json", result_json]
        log(f"Executing: {' '.join(cmd)}")
        target_start = time.time()
        result = subprocess.run(cmd, cwd=os.path.dirname(script), env=env)
        record_verification_artifact(f"integration_{label}", result_json)
        if result.returncode != 0:
            log(f"ERROR: Integration test target failed: {label}")
            record_verification_step(
                "integration_tests",
                "failed",
                duration_seconds=time.time() - target_start,
                details={"target": label, "exit_code": result.returncode, "mode": mode},
            )
            sys.exit(1)

    log("=== Integration Tests Passed ===")
    record_verification_step(
        "integration_tests",
        "passed",
        details={"mode": mode, "targets": [label for label, _ in targets]},
    )


def write_process_diagnostics_artifact(
    artifact_name: str,
    filename: str,
    command: List[str],
    result: subprocess.CompletedProcess,
) -> Optional[str]:
    text = "\n".join(
        [
            f"command: {subprocess.list2cmdline(command)}",
            f"exit_code: {result.returncode}",
            "",
            "[stdout]",
            result.stdout or "<empty>",
            "",
            "[stderr]",
            result.stderr or "<empty>",
            "",
        ]
    )
    return write_verification_artifact(artifact_name, filename, text)


CLANG_TIDY_BASELINE_PATH = os.path.join(PROJECT_ROOT, "tools", "clang_tidy_baseline.json")


def project_relative_key(source_path: str) -> str:
    """Project-relative, separator-normalized key for one source file."""
    candidate = os.path.normpath(str(source_path))
    try:
        relative = os.path.relpath(candidate, PROJECT_ROOT)
    except ValueError:
        relative = candidate
    if relative.startswith(".."):
        relative = candidate
    return relative.replace("\\", "/")


def clang_tidy_scope_path(source_path: str) -> str:
    """Project-relative, separator-normalized key for one linted translation unit."""
    return project_relative_key(source_path)


def clang_tidy_scope_from_entries(compile_database: List[Any]) -> Dict[str, Any]:
    """Describe which translation units a compilation database makes clang-tidy lint."""
    units = set()
    for entry in compile_database:
        source_path = entry.get("file") if isinstance(entry, dict) else None
        if source_path:
            units.add(clang_tidy_scope_path(source_path))
    return {"entries": len(units), "translation_units": sorted(units)}


def clang_tidy_scope_gap(
    baseline_scope: Optional[Mapping[str, Any]], current_scope: Optional[Mapping[str, Any]]
) -> Optional[List[str]]:
    """Baseline translation units this run did not lint; None when either scope is unknown.

    Sources that no longer exist are not a gap: they cannot produce warnings any
    more, so an ordinary deletion must not freeze the ratchet until someone
    regenerates the baseline by hand.
    """
    if not isinstance(baseline_scope, dict) or not isinstance(current_scope, dict):
        return None
    baseline_units = baseline_scope.get("translation_units")
    current_units = current_scope.get("translation_units")
    if not isinstance(baseline_units, list) or not isinstance(current_units, list):
        return None
    missing = set(baseline_units) - set(current_units)
    return sorted(unit for unit in missing if os.path.exists(os.path.join(PROJECT_ROOT, unit.replace("/", os.sep))))


def load_clang_tidy_baseline() -> Optional[Dict[str, Any]]:
    """Read the accepted per-check counts plus the lint scope they were measured over.

    Returns None when no baseline exists yet. ``scope`` is None for a baseline
    written before scope recording existed; its counts are then not comparable
    downward against any run.
    """
    if not os.path.exists(CLANG_TIDY_BASELINE_PATH):
        return None
    try:
        with open(CLANG_TIDY_BASELINE_PATH, "r", encoding="utf-8") as handle:
            data = json.load(handle)
        checks = {str(name): int(count) for name, count in data.get("checks", {}).items()}
        scope = None
        recorded_scope = data.get("scope")
        if isinstance(recorded_scope, dict):
            units = recorded_scope.get("translation_units")
            if isinstance(units, list) and units:
                scope = {"entries": len(set(units)), "translation_units": sorted({str(unit) for unit in units})}
        return {"checks": checks, "scope": scope}
    except (OSError, ValueError, TypeError, AttributeError) as error:
        log(f"ERROR: Unreadable clang-tidy baseline {CLANG_TIDY_BASELINE_PATH}: {error}")
        sys.exit(2)


def write_clang_tidy_baseline(check_counts: Mapping[str, int], scope: Optional[Mapping[str, Any]] = None) -> bool:
    payload: Dict[str, Any] = {
        "_comment": [
            "Accepted clang-tidy warning counts, keyed by check rather than file:line so",
            "ordinary edits do not churn the baseline. build.py fails when any check",
            "exceeds its recorded count or a previously unseen check appears.",
            "Regenerate deliberately with: python build.py --lint --update-lint-baseline",
            "Lower counts are folded in automatically; the ratchet only tightens.",
            "",
            "'scope' records the translation units these counts were measured over.",
            "A run whose compilation database covers fewer of them (for example the",
            "tests-only database a --tests-only build leaves behind) lints a subset,",
            "so its lower counts are not evidence of improvement and are never folded",
            "in. Increases stay fatal at any scope. Do not hand-edit 'scope'.",
            "",
            "The large frozen entries are accepted debt, not endorsements:",
            "  bugprone-narrowing-conversions            pervasive in graphics/timing math",
            "  bugprone-invalid-enum-default-initialization  D3D12_HEAP_PROPERTIES{} zero-init",
            "  bugprone-throwing-static-initialization   std::mutex globals; non-throwing here",
            "  bugprone-multi-level-implicit-pointer-conversion  COM void** out-params",
            "  bugprone-argument-comment                 comment/parameter name drift only",
            "  bugprone-exception-escape                 destructors; see git log for rationale",
        ],
        "checks": dict(sorted(check_counts.items())),
        "total": sum(check_counts.values()),
    }
    if scope and scope.get("translation_units"):
        units = sorted({str(unit) for unit in scope["translation_units"]})
        payload["scope"] = {"entries": len(units), "translation_units": units}
    os.makedirs(os.path.dirname(CLANG_TIDY_BASELINE_PATH), exist_ok=True)
    return write_text_atomic_if_changed(CLANG_TIDY_BASELINE_PATH, json.dumps(payload, indent=2) + "\n")


def evaluate_clang_tidy_baseline(
    check_counts: Mapping[str, int],
    lint_details: Dict[str, Any],
    current_scope: Optional[Mapping[str, Any]] = None,
    *,
    mutate_baseline: bool = True,
) -> None:
    """Fail when clang-tidy findings grow; fold in improvements automatically.

    The raw warnings stay advisory because the existing backlog is large and mostly
    low-value. Without a ratchet that backlog also hides genuinely useful findings,
    so a *regression* against the recorded counts is fatal in every flow that lints.

    Auto-tightening is only sound when this run linted everything the baseline was
    measured over. A partial compilation database - notably the tests-only one a
    `--tests-only` build leaves behind - lints a subset, so its lower counts say
    nothing about the code and would otherwise ratchet the accepted counts down to
    that subset, making the next full run fail with phantom regressions. Increases
    stay fatal at any scope: warnings are only ever counted from translation units
    that were actually linted, so a count above the baseline is a real regression
    however small the scope was.
    """
    update_requested = mutate_baseline and "--update-lint-baseline" in sys.argv
    baseline_record = load_clang_tidy_baseline()
    baseline = baseline_record["checks"] if baseline_record else None
    baseline_scope = baseline_record["scope"] if baseline_record else None

    scope_gap = clang_tidy_scope_gap(baseline_scope, current_scope)
    scope_reduced = bool(scope_gap)
    # Fold improvements in only with proven equal-or-wider coverage; an unknown
    # scope on either side is treated exactly like a reduced one.
    tightening_allowed = scope_gap is not None and not scope_gap
    if current_scope:
        lint_details["clang_tidy_scope_units"] = int(current_scope.get("entries", 0))
    if scope_gap is None:
        lint_details["clang_tidy_scope"] = "unknown"
    elif scope_reduced:
        lint_details["clang_tidy_scope"] = "reduced"
        lint_details["clang_tidy_scope_unlinted"] = len(scope_gap)
        lint_details["clang_tidy_scope_unlinted_units"] = scope_gap[:20]
    else:
        lint_details["clang_tidy_scope"] = "full"

    if scope_reduced:
        baseline_units = len((baseline_scope or {}).get("translation_units", []))
        examples = ", ".join(scope_gap[:3])
        log(
            f"clang-tidy lint scope reduced: {(current_scope or {}).get('entries', 0)} translation unit(s) linted, "
            f"{len(scope_gap)} of the baseline's {baseline_units} not covered (e.g. {examples}). "
            "Baseline auto-tightening is disabled for this run; regressions are still fatal."
        )

    if update_requested and scope_reduced:
        # Rewriting the whole baseline from a subset is exactly the corruption the
        # scope record exists to prevent, so refuse even when asked explicitly.
        log("ERROR: refusing to rewrite the clang-tidy baseline from a partial compilation database.")
        log(f"  {len(scope_gap)} baseline translation unit(s) were not linted by this run.")
        log("  Regenerate the full database first (python build.py --incremental --skip-updates --concise),")
        log("  then rerun with --no-build --lint --update-lint-baseline --skip-updates --concise.")
        lint_details["clang_tidy_baseline"] = "update_refused_reduced_scope"
        record_verification_step(
            "lint",
            "failed",
            details={**lint_details, "reason": "clang_tidy_baseline_reduced_scope_update"},
        )
        sys.exit(2)

    if baseline is None and not mutate_baseline:
        lint_details["clang_tidy_baseline"] = "missing_preflight"
        log("clang-tidy preflight has no accepted baseline to compare; deferring to the final lint stage")
        return

    if baseline is None or update_requested:
        write_clang_tidy_baseline(check_counts, current_scope)
        lint_details["clang_tidy_baseline"] = "written"
        action = "Updated" if update_requested else "Created"
        log(f"{action} clang-tidy baseline: {CLANG_TIDY_BASELINE_PATH} " f"({sum(check_counts.values())} warnings)")
        return

    regressions = []
    for check, count in sorted(check_counts.items()):
        allowed = baseline.get(check)
        if allowed is None:
            regressions.append(f"{check}: new check with {count} warning(s)")
        elif count > allowed:
            regressions.append(f"{check}: {count} > {allowed} allowed")

    improvements = {
        check: (allowed, check_counts.get(check, 0))
        for check, allowed in baseline.items()
        if check_counts.get(check, 0) < allowed
    }

    if regressions:
        lint_details["clang_tidy_baseline"] = "regressed"
        lint_details["clang_tidy_baseline_regressions"] = regressions
        log("ERROR: clang-tidy findings regressed against the accepted baseline:")
        for entry in regressions:
            log(f"  {entry}")
        log(f"Baseline: {CLANG_TIDY_BASELINE_PATH}")
        if scope_reduced:
            log("This run linted a subset of the baseline scope; the reported checks still exceed it.")
        log("Fix the new findings, or run --lint --update-lint-baseline if the increase is justified.")
        diagnostics_path = verification_artifact_path("clang_tidy.log")
        if diagnostics_path:
            log(f"Complete clang-tidy diagnostics: {diagnostics_path}")
        record_verification_step(
            "lint",
            "failed",
            details={**lint_details, "reason": "clang_tidy_baseline_regression"},
        )
        sys.exit(1)

    summary = ", ".join(f"{check} {old}->{new}" for check, (old, new) in sorted(improvements.items())[:6])
    if improvements and (not tightening_allowed or not mutate_baseline):
        reason = (
            "read-only preflight"
            if not mutate_baseline
            else "reduced lint scope"
            if scope_reduced
            else "unknown lint scope"
        )
        lint_details["clang_tidy_baseline"] = "tightening_skipped"
        lint_details["clang_tidy_baseline_tightening_skipped"] = {
            check: {"was": old, "now": new} for check, (old, new) in improvements.items()
        }
        log(
            f"clang-tidy baseline: {len(improvements)} check(s) below baseline left unchanged ({reason}); "
            f"rerun lint against the full compilation database to fold them in ({summary})"
        )
        return

    if improvements:
        # Tighten immediately so a fixed warning cannot silently come back.
        merged = dict(baseline)
        for check, (_, actual) in improvements.items():
            merged[check] = actual
        for check, count in check_counts.items():
            merged.setdefault(check, count)
        write_clang_tidy_baseline(merged, current_scope or baseline_scope)
        log(f"clang-tidy baseline tightened: {summary}")
        lint_details["clang_tidy_baseline"] = "tightened"
        lint_details["clang_tidy_baseline_improvements"] = {
            check: {"was": old, "now": new} for check, (old, new) in improvements.items()
        }
        return

    lint_details["clang_tidy_baseline"] = "unchanged"
    if mutate_baseline and tightening_allowed and write_clang_tidy_baseline(baseline, current_scope):
        # Keep the recorded scope current when sources were added, so later subset
        # detection compares against what a full run actually covers today.
        lint_details["clang_tidy_baseline"] = "scope_refreshed"
        log(f"clang-tidy baseline scope refreshed: {(current_scope or {}).get('entries', 0)} translation unit(s)")
    log(f"clang-tidy baseline: OK ({sum(check_counts.values())} warning(s), none above baseline)")


FILE_SIZE_BASELINE_PATH = os.path.join(PROJECT_ROOT, "tools", "file_size_baseline.json")
FILE_SIZE_LIMIT = 800
FILE_SIZE_TARGET = 750

LINTABLE_SOURCE_DIRS = ["common", "hook", "captureengine", "mediaengine", "testapp", "tests"]
LINTABLE_SOURCE_SUFFIXES = (".cpp", ".h", ".hpp", ".c")
# The size ratchet also governs .inl files. They are ordinary C++ source under
# the AGENTS.md rule, and the test apps use them to split a single translation
# unit, so leaving them out would be an easy way to grow past the ceiling
# unnoticed. clang-format's scope stays as it was.
FILE_SIZE_SOURCE_SUFFIXES = LINTABLE_SOURCE_SUFFIXES + (".inl",)
# Mirrors the first-party Python that flake8 already lints (`py_targets` below).
FILE_SIZE_PYTHON_DIRS = ["tools", "testapp"]
# The wiki is governed by the same ceiling. `llm-wiki/log/recent.md` is rolling
# memory that its own convention archives at ~230 lines, and with nothing
# enforcing that it reached 6212 lines / 1.45 MB - stored as 2412 distinct blobs
# over 479 commits, which is what made the repository history large. Capping
# markdown here means the rotation cannot silently lapse again.
FILE_SIZE_MARKDOWN_DIRS = ["llm-wiki"]


def collect_lintable_cpp_sources(suffixes: Tuple[str, ...] = LINTABLE_SOURCE_SUFFIXES) -> List[str]:
    """First-party C++ sources shared by clang-format and the file-size ratchet.

    External, vendored, and ImGui trees are excluded: their size is not ours to
    manage and formatting them would produce large unrelated diffs.
    """
    files: List[str] = []
    for directory in LINTABLE_SOURCE_DIRS:
        root_path = os.path.join(PROJECT_ROOT, directory)
        for root, _, filenames in os.walk(root_path):
            for name in filenames:
                if not name.endswith(suffixes):
                    continue
                path = os.path.join(root, name)
                if "external" in path or "imgui" in path:
                    continue
                files.append(path)
    return files


def collect_file_size_python_sources() -> List[str]:
    """First-party Python sources: the root build/test scripts plus `tools/`."""
    files: List[str] = []
    for name in sorted(os.listdir(PROJECT_ROOT)):
        path = os.path.join(PROJECT_ROOT, name)
        if name.endswith(".py") and os.path.isfile(path):
            files.append(path)
    for directory in FILE_SIZE_PYTHON_DIRS:
        root_path = os.path.join(PROJECT_ROOT, directory)
        for root, dirnames, filenames in os.walk(root_path):
            dirnames[:] = [name for name in dirnames if name != "__pycache__"]
            files.extend(os.path.join(root, name) for name in filenames if name.endswith(".py"))
    return files


def collect_file_size_markdown_sources() -> List[str]:
    """First-party wiki pages the size ratchet governs."""
    files: List[str] = []
    for directory in FILE_SIZE_MARKDOWN_DIRS:
        root_path = os.path.join(PROJECT_ROOT, directory)
        for root, _, filenames in os.walk(root_path):
            files.extend(os.path.join(root, name) for name in filenames if name.endswith(".md"))
    return files


def count_source_lines(path: str) -> int:
    """Line count for one source file, tolerant of the tree's mixed encodings."""
    with open(path, "rb") as handle:
        return len(handle.read().splitlines())


def collect_source_file_sizes() -> Dict[str, int]:
    """Project-relative line counts for every file the size ratchet governs."""
    sizes: Dict[str, int] = {}
    cpp_sources = collect_lintable_cpp_sources(FILE_SIZE_SOURCE_SUFFIXES)
    other_sources = collect_file_size_python_sources() + collect_file_size_markdown_sources()
    for path in cpp_sources + other_sources:
        try:
            sizes[project_relative_key(path)] = count_source_lines(path)
        except OSError as error:
            log(f"WARNING: could not measure {path}: {error}")
    return sizes


def load_file_size_baseline() -> Optional[Dict[str, int]]:
    """Read the accepted per-file line counts; None when no baseline exists yet."""
    if not os.path.exists(FILE_SIZE_BASELINE_PATH):
        return None
    try:
        with open(FILE_SIZE_BASELINE_PATH, "r", encoding="utf-8") as handle:
            data = json.load(handle)
        return {str(path): int(count) for path, count in data.get("files", {}).items()}
    except (OSError, ValueError, TypeError, AttributeError) as error:
        log(f"ERROR: Unreadable file-size baseline {FILE_SIZE_BASELINE_PATH}: {error}")
        sys.exit(2)


def write_file_size_baseline(violations: Mapping[str, int]) -> bool:
    payload: Dict[str, Any] = {
        "_comment": [
            "Accepted source files above the AGENTS.md size ceiling, with their line counts.",
            f"The ceiling is {FILE_SIZE_LIMIT} lines; the working target when splitting is {FILE_SIZE_TARGET}.",
            "build.py fails lint when a recorded file grows or a new file crosses the ceiling.",
            "Counts below baseline are folded in automatically and files that drop under the",
            "ceiling are removed, so a split file cannot silently grow back.",
            "Regenerate deliberately with:",
            "  python build.py --no-build --lint --update-lint-baseline --skip-updates --concise",
            "",
            "Every entry here is debt being worked off, not an endorsement.",
        ],
        "limit": FILE_SIZE_LIMIT,
        "target": FILE_SIZE_TARGET,
        "files": dict(sorted(violations.items())),
        "count": len(violations),
        "total": sum(violations.values()),
    }
    os.makedirs(os.path.dirname(FILE_SIZE_BASELINE_PATH), exist_ok=True)
    return write_text_atomic_if_changed(FILE_SIZE_BASELINE_PATH, json.dumps(payload, indent=2) + "\n")
