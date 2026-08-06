

def evaluate_file_size_baseline(
    sizes: Mapping[str, int], lint_details: Dict[str, Any], *, mutate_baseline: bool = True
) -> None:
    """Fail when a source file grows past the ceiling; fold in shrinkage automatically.

    `AGENTS.md` keeps source files at roughly 600-800 lines. The rule was
    documentation-only for a long time, so the tree drifted well past it; this
    ratchet records the existing violations and makes any growth fatal, exactly
    like the clang-tidy ratchet above.

    There is no scope problem here: the walk is filesystem-based and always
    complete, so improvements can be folded in unconditionally.
    """
    update_requested = mutate_baseline and "--update-lint-baseline" in sys.argv
    baseline = load_file_size_baseline()
    violations = {path: count for path, count in sizes.items() if count > FILE_SIZE_LIMIT}

    lint_details["file_size_files"] = len(sizes)
    lint_details["file_size_violations"] = len(violations)
    lint_details["file_size_violation_lines"] = sum(violations.values())

    if baseline is None and not mutate_baseline:
        lint_details["file_size_baseline"] = "missing_preflight"
        log("file-size preflight has no accepted baseline to compare; deferring to the final lint stage")
        return

    if baseline is None or update_requested:
        write_file_size_baseline(violations)
        lint_details["file_size_baseline"] = "written"
        action = "Updated" if update_requested else "Created"
        log(f"{action} file-size baseline: {FILE_SIZE_BASELINE_PATH}")
        log(f"  {len(violations)} file(s) over {FILE_SIZE_LIMIT} lines, {sum(violations.values())} lines total")
        return

    regressions = []
    for path, count in sorted(violations.items()):
        allowed = baseline.get(path)
        if allowed is None:
            regressions.append(f"{path}: {count} lines, past the {FILE_SIZE_LIMIT}-line ceiling (new violation)")
        elif count > allowed:
            regressions.append(f"{path}: {count} > {allowed} accepted lines")

    if regressions:
        lint_details["file_size_baseline"] = "regressed"
        lint_details["file_size_regressions"] = regressions
        log("ERROR: source files grew past the accepted size baseline:")
        for entry in regressions:
            log(f"  {entry}")
        log(f"Baseline: {FILE_SIZE_BASELINE_PATH}")
        log(f"AGENTS.md keeps source files at roughly 600-{FILE_SIZE_LIMIT} lines.")
        log(f"Split the file (working target {FILE_SIZE_TARGET} lines) instead of growing it,")
        log("or rerun with --lint --update-lint-baseline if the increase is genuinely justified.")
        record_verification_step(
            "lint",
            "failed",
            details={**lint_details, "reason": "file_size_baseline_regression"},
        )
        sys.exit(1)

    # Shrinking a file below its recorded count - or under the ceiling entirely -
    # tightens the baseline at once, so the space cannot be reclaimed silently.
    improvements = {
        path: (allowed, violations.get(path, 0))
        for path, allowed in baseline.items()
        if violations.get(path, 0) < allowed
    }
    if improvements and mutate_baseline:
        write_file_size_baseline(violations)
        resolved = sorted(path for path, (_, now) in improvements.items() if now == 0)
        preview = ", ".join(
            f"{path} {old}->{new or 'under limit'}" for path, (old, new) in sorted(improvements.items())[:6]
        )
        log(f"file-size baseline tightened: {len(improvements)} file(s) smaller ({preview})")
        if resolved:
            log(f"  {len(resolved)} file(s) now under the ceiling and dropped from the baseline")
        lint_details["file_size_baseline"] = "tightened"
        lint_details["file_size_resolved"] = resolved
        lint_details["file_size_improvements"] = {
            path: {"was": old, "now": new} for path, (old, new) in improvements.items()
        }
        return

    lint_details["file_size_baseline"] = "unchanged"
    log(f"file-size baseline: OK ({len(violations)} accepted file(s) over {FILE_SIZE_LIMIT} lines, none grew)")


def run_verify_preflight(env: Dict[str, str]) -> None:
    """Fail cheap ratchets early and pre-lint changed translation units from the last full database."""
    log("=== Running Verification Preflight ===")
    started = time.time()
    details: Dict[str, Any] = {}
    evaluate_file_size_baseline(collect_source_file_sizes(), details, mutate_baseline=False)

    clang_tidy = os.path.join(MSYS2_DIR, "clang64", "bin", "clang-tidy.exe")
    if IS_LINUX:
        clang_tidy = shutil.which("clang-tidy") or clang_tidy
    preflight = run_snapshot_preflight(
        clang_tidy=clang_tidy,
        snapshot_dir=CLANG_TIDY_SNAPSHOT_DIR,
        build_script_sha256=sha256_file(os.path.abspath(__file__)),
        project_root=PROJECT_ROOT,
        cache_dir=CLANG_TIDY_CACHE_DIR,
        jobs=get_parallel_job_count(env, cpu_count()),
        env=env,
    )
    if preflight:
        result = preflight.result
        completed = subprocess.CompletedProcess(
            preflight.command,
            result.returncode,
            stdout=result.stdout,
            stderr=result.stderr,
        )
        write_process_diagnostics_artifact(
            "clang_tidy_preflight_diagnostics",
            "clang_tidy_preflight.log",
            list(preflight.command),
            completed,
        )
        warnings, check_counts, _ = analyze_warning_output(result.stdout, PROJECT_ROOT)
        details.update(
            {
                "clang_tidy_warnings": len(warnings),
                "clang_tidy_cache_hits": result.hits,
                "clang_tidy_cache_misses": result.misses,
                "clang_tidy_cache_uncacheable": result.uncacheable,
            }
        )
        comparable_scope = (
            clang_tidy_scope_from_entries(list(preflight.compile_database)) if result.returncode == 0 else None
        )
        evaluate_clang_tidy_baseline(check_counts, details, comparable_scope, mutate_baseline=False)
        log(
            f"Verification preflight clang-tidy cache: {result.hits} hit(s), {result.misses} miss(es), "
            f"{result.uncacheable} uncacheable"
        )
    else:
        details["clang_tidy"] = "deferred_no_compatible_full_snapshot"
        log("Verification preflight: no compatible full compile database; clang-tidy deferred to final lint")
    record_verification_step(
        "preflight",
        "passed",
        duration_seconds=time.time() - started,
        details=details,
    )


def refresh_full_compile_database_snapshot() -> None:
    """Retain only a database proven to cover the committed clang-tidy baseline scope."""
    try:
        with open(get_compile_commands_path(), "r", encoding="utf-8") as source:
            compile_database = json.load(source)
        baseline = load_clang_tidy_baseline()
        baseline_scope = baseline["scope"] if baseline else None
        current_scope = clang_tidy_scope_from_entries(compile_database)
        if clang_tidy_scope_gap(baseline_scope, current_scope):
            log("Not refreshing clang-tidy preflight database from a reduced lint scope")
            return
        write_compile_database_snapshot(
            compile_database=compile_database,
            snapshot_dir=CLANG_TIDY_SNAPSHOT_DIR,
            build_script_sha256=sha256_file(os.path.abspath(__file__)),
        )
        log(f"Refreshed full clang-tidy preflight database ({current_scope['entries']} translation units)")
    except (OSError, ValueError, TypeError, json.JSONDecodeError) as error:
        log(f"WARNING: Could not refresh clang-tidy preflight database: {error}")


def run_format(env):
    log("=== Running Auto-Format ===")
    format_start = time.time()
    format_ok = True

    # 1. C++ Format (clang-format -i)
    clang_format = None
    if IS_LINUX:
        clang_format = shutil.which("clang-format")
    else:
        clang_format = os.path.join(MSYS2_DIR, "clang64", "bin", "clang-format.exe")

    if clang_format and (IS_LINUX or os.path.exists(clang_format)):
        log("Formatting C++ files...")
        files = collect_lintable_cpp_sources()

        if files:
            chunk_size = 50
            for i in range(0, len(files), chunk_size):
                chunk = files[i : i + chunk_size]
                cmd = [
                    clang_format,
                    "-i",
                    f"--style=file:{os.path.join(PROJECT_ROOT, 'tools', 'config', '.clang-format')}",
                ] + chunk
                subprocess.run(cmd, env=env, check=True)
            log("C++ files formatted.")
    else:
        log("Error: clang-format not found.")
        format_ok = False

    # 2. Python format (black)
    try:
        subprocess.run(
            [sys.executable, "-m", "black", "--version"],
            capture_output=True,
            check=True,
        )
        has_black = True
    except Exception:
        has_black = False

    if has_black:
        log("Formatting Python files...")
        py_targets = ["build.py", "testapp"]
        cmd = [sys.executable, "-m", "black", "--line-length", "120"] + py_targets
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            log("Python format failed:")
            if res.stdout:
                log(res.stdout)
            if res.stderr:
                log(res.stderr)
            format_ok = False
        else:
            log("Python files formatted.")
    else:
        log("Error: black not found. (Run 'pip install black')")
        format_ok = False

    record_verification_step(
        "format",
        "passed" if format_ok else "failed",
        duration_seconds=time.time() - format_start,
    )

    return format_ok
