

def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)
    apply_workspace_temp_environment()

    global DETAIL_LOG_FILE, LOG_FILE, VERIFICATION_FINAL_EXIT_CODE, CONCISE_OUTPUT, VERBOSE_COMMANDS

    args = sys.argv[1:]
    VERBOSE_COMMANDS = "--verbose-commands" in args
    CONCISE_OUTPUT = not VERBOSE_COMMANDS
    log_file_override = parse_flag_value("--log-file")
    if log_file_override:
        LOG_FILE = os.path.abspath(log_file_override)
    detail_log_override = parse_flag_value("--detail-log")
    if detail_log_override:
        DETAIL_LOG_FILE = os.path.abspath(detail_log_override)
    os.makedirs(os.path.dirname(LOG_FILE), exist_ok=True)
    if DETAIL_LOG_FILE:
        os.makedirs(os.path.dirname(DETAIL_LOG_FILE), exist_ok=True)

    if os.path.exists(LOG_FILE):
        try:
            os.remove(LOG_FILE)
        except Exception:
            pass
    if DETAIL_LOG_FILE and os.path.abspath(DETAIL_LOG_FILE) != os.path.abspath(LOG_FILE) and os.path.exists(
        DETAIL_LOG_FILE
    ):
        try:
            os.remove(DETAIL_LOG_FILE)
        except Exception:
            pass

    log("=== Starting Build ===")

    # Parse flags early for force rebuild
    force_rebuild = "--force-rebuild" in sys.argv
    resume_requested = "--resume" in sys.argv
    if force_rebuild and resume_requested:
        log("ERROR: --resume and --force-rebuild are mutually exclusive")
        sys.exit(2)
    if force_rebuild:
        log("FORCE REBUILD: Cleaning all object files...")
        if os.path.exists(OBJ_DIR):
            try:
                shutil.rmtree(OBJ_DIR)
                log("Object directory cleaned.")
            except Exception as e:
                log(f"Warning: Could not clean {OBJ_DIR}: {e}")

    # Always clean object files to avoid struct layout mismatches
    # if os.path.exists(OBJ_DIR):
    #     log("Cleaning object files for fresh build...")
    #     try:
    #         shutil.rmtree(OBJ_DIR)
    #     except Exception as e:
    #         log(f"Warning: Could not clean {OBJ_DIR}: {e}")

    # Preserve log files in logs/ subfolder across builds.
    # Logs, crash dumps, and CSV files are kept so that crash analysis can
    # be performed even after a rebuild.  Delete them manually when needed.
    logs_dir = os.path.join(BIN_DIR, "logs")
    if os.path.exists(logs_dir):
        log("Preserving existing log files (not cleaning logs/).")

    # Clean legacy log files from root/bin
    legacy_logs = [
        os.path.join(BIN_DIR, "Layer"),
        os.path.join(PROJECT_ROOT, "Layer"),
    ]
    for f in legacy_logs:
        if os.path.exists(f):
            try:
                os.remove(f)
                log(f"Removed legacy log: {f}")
            except Exception:
                pass

    # Parse flags
    default_quality_mode = is_default_quality_invocation(args)
    verify_flag = "--verify" in sys.argv
    skip_updates = "--skip-updates" in sys.argv
    run_tests_flag = "--run-tests" in sys.argv
    run_integration_flag = "--run-integration-tests" in sys.argv
    full_integration_flag = "--full-integration" in sys.argv
    lint_flag = "--lint" in sys.argv
    format_flag = "--format" in sys.argv
    ccache_flag = "--ccache" in sys.argv
    no_build_flag = "--no-build" in sys.argv
    gtest_filter = parse_flag_value("--gtest-filter")
    tests_only_flag = "--tests-only" in sys.argv
    sanitize_flag = "--sanitize" in sys.argv
    sanitize_x86_flag = "--sanitize-x86" in sys.argv
    sanitize_regression_flag = "--sanitize-regression" in sys.argv
    sanitize_regression_child = "--sanitize-regression-child" in sys.argv
    run_fuzz_flag = "--run-fuzz" in sys.argv
    fuzz_seconds = 60
    fuzz_seconds_value = parse_flag_value("--fuzz-seconds")
    if fuzz_seconds_value is not None:
        try:
            fuzz_seconds = max(1, int(fuzz_seconds_value))
        except ValueError:
            log(f"Warning: invalid --fuzz-seconds value {fuzz_seconds_value!r}; using {fuzz_seconds}")
    # --production: build signed production binaries (requires CE_PRODUCTION_BUILD=1)
    # Dev builds do NOT pass this flag; signature verification is a warning only.
    production_flag = "--production" in sys.argv or "CE_PRODUCTION_BUILD" in os.environ
    # --force is now DEFAULT behavior for reliability (disable with --incremental)
    resume_flag = "--resume" in sys.argv
    incremental_flag = "--incremental" in sys.argv or resume_flag
    force_flag = not incremental_flag  # Force rebuild by default

    if resume_flag and no_build_flag:
        log("ERROR: --resume requires a build and cannot be combined with --no-build")
        sys.exit(2)
    if resume_flag and not skip_updates:
        log("ERROR: --resume requires --skip-updates so the toolchain/dependency boundary stays unchanged")
        sys.exit(2)

    # --jobs N: override parallel compilation worker count (default: all CPU cores)
    jobs_flag = None
    for i, arg in enumerate(sys.argv):
        if arg == "--jobs" and i + 1 < len(sys.argv):
            try:
                jobs_flag = int(sys.argv[i + 1])
            except ValueError:
                log(f"Warning: invalid --jobs value {sys.argv[i + 1]!r}; using auto worker count")
            break
        elif arg.startswith("--jobs="):
            try:
                jobs_flag = int(arg.split("=", 1)[1])
            except ValueError:
                log(f"Warning: invalid --jobs value {arg!r}; using auto worker count")
            break
    if VERBOSE_COMMANDS:
        log("Verbose command logging enabled (--verbose-commands)")
    else:
        log("Concise console output enabled; complete commands and subprocess output go to the detailed log")

    if sanitize_x86_flag:
        log(
            "ERROR: x86 ASan/UBSan coverage was explicitly requested, but the required MinGW x86 sanitizer "
            "runtime is unavailable. Refusing to silently skip x86 coverage."
        )
        sys.exit(2)

    try:
        current_build_number = resolve_build_number_for_invocation(
            sanitize_regression_child=sanitize_regression_child,
            resume_failed_build=resume_flag,
            no_build=no_build_flag,
            tests_only=tests_only_flag,
        )
    except RuntimeError as error:
        action = "resume failed build" if resume_flag else "reuse current build version"
        log(f"ERROR: Cannot {action}: {error}")
        sys.exit(1)
    if sanitize_regression_child:
        log(f"Reusing parent build version: 0.1.{current_build_number}")
    elif resume_flag:
        log(f"Resuming failed build version: 0.1.{current_build_number}")
    elif no_build_flag:
        log(f"No-build verification reuses current build version: 0.1.{current_build_number}")
    elif tests_only_flag:
        log(f"Tests-only build reuses current product version: 0.1.{current_build_number}")
    # Store for use by compile_project
    global CURRENT_BUILD_NUMBER
    CURRENT_BUILD_NUMBER = current_build_number
    init_verification_context(
        args,
        current_build_number,
        verify_flag or default_quality_mode,
        not sanitize_regression_child,
    )
    record_verification_artifact("live_build_log", LOG_FILE)

    if gtest_filter:
        log(f"Using unit test filter (--gtest-filter): {gtest_filter}")

    if tests_only_flag:
        log("Tests-only build mode enabled (--tests-only)")

    setup_start = time.time()
    setup_msys2(skip_updates=skip_updates)
    record_verification_step(
        "toolchain_setup",
        "passed",
        duration_seconds=time.time() - setup_start,
        details={"skip_updates": skip_updates},
    )
    if should_bootstrap_python_tools(default_quality_mode, verify_flag, lint_flag, format_flag):
        python_bootstrap_start = time.time()
        python_bootstrap_ok = check_python_lsp_tools()
        record_verification_step(
            "python_tool_bootstrap",
            "passed" if python_bootstrap_ok else "warning",
            duration_seconds=time.time() - python_bootstrap_start,
        )
    else:
        log("Skipping optional Python tooling bootstrap for this build")
        record_verification_step("python_tool_bootstrap", "skipped")
    env, clang_bin = get_env()

    if jobs_flag:
        env["CE_BUILD_JOBS"] = str(jobs_flag)
        log(f"Parallel jobs set to {jobs_flag} (--jobs)")

    if default_quality_mode:
        log("Default quality mode: running lint/LSP checks and unit/regression tests (no integration apps)")
        run_tests_flag = True
        lint_flag = True
        sanitize_regression_flag = True

    if verify_flag:
        log("Verification mode: running canonical post-change checks in one pass")
        run_tests_flag = True
        lint_flag = True
        sanitize_regression_flag = True

    if full_integration_flag:
        run_integration_flag = True
    if run_integration_flag:
        run_tests_flag = True
    if sanitize_regression_child:
        sanitize_regression_flag = False

    integration_coverage = "full" if full_integration_flag else "smoke" if run_integration_flag else "not_run"
    record_verification_coverage("integration_tests", integration_coverage)
    if tests_only_flag or no_build_flag:
        signature_coverage = "not_applicable"
    else:
        signature_coverage = "enforced" if production_flag else "advisory"
    record_verification_coverage("production_signatures", signature_coverage)
    record_verification_coverage(
        "sanitizers",
        "x64_asan_ubsan" if sanitize_flag or sanitize_regression_flag else "not_run",
    )
    record_verification_coverage("x86_sanitizers", "unavailable")
    if not run_fuzz_flag:
        record_verification_coverage("fuzz", "not_run")
    if run_integration_flag:
        test_app_coverage = "executed"
    elif tests_only_flag:
        test_app_coverage = "not_built"
    elif no_build_flag:
        test_app_coverage = "not_run"
    else:
        test_app_coverage = "compiled_not_executed"
    record_verification_coverage("test_apps", test_app_coverage)

    # Store flags in env for access in compile functions
    env["FORCE_REBUILD"] = "1" if force_flag else "0"
    if production_flag:
        env["CE_PRODUCTION_BUILD"] = "1"
        log("PRODUCTION BUILD: DLL signature verification will be enforced")
    else:
        log("DEV BUILD: DLL signature verification is advisory only")

    if incremental_flag:
        mode = "failed-build resume" if resume_flag else "incremental build"
        log(f"Validated {mode} - unchanged objects may be reused by content signature")
    else:
        log("Force rebuild (default) - ensuring clean build for reliability")

    if ccache_flag:
        log("Enabling ccache for faster builds (--ccache)")
        log(
            "WARNING: ccache may occasionally serve stale objects."
            " Use --ccache only for development, not release builds."
        )
        # Clear any existing disable flag
        if "DISABLE_CCACHE" in env:
            del env["DISABLE_CCACHE"]

    if sanitize_flag:
        log("Sanitizer build enabled (--sanitize) - adding ASan + UBSan flags")
        sanitize_compile = [
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-g",
        ]
        sanitize_link = ["-fsanitize=address,undefined"]

        def _strip_lto(flags: List[str]) -> List[str]:
            return [flag for flag in flags if not flag.startswith("-flto")]

        # ASan/UBSan + LTO is unstable with current MinGW LLVM/LLD and can crash
        # the linker with COMDAT key errors. Disable LTO for sanitizer builds.
        OPT_FLAGS_X64[:] = _strip_lto(OPT_FLAGS_X64)
        HOOK_OPT_FLAGS_X64[:] = _strip_lto(HOOK_OPT_FLAGS_X64)
        OPT_FLAGS_X86[:] = _strip_lto(OPT_FLAGS_X86)
        HOOK_OPT_FLAGS_X86[:] = _strip_lto(HOOK_OPT_FLAGS_X86)
        LD_OPT_FLAGS[:] = _strip_lto(LD_OPT_FLAGS)
        env["CE_SANITIZE"] = "1"
        env["CE_DISABLE_LTO"] = "1"

        OPT_FLAGS_X64.extend(sanitize_compile)
        HOOK_OPT_FLAGS_X64.extend(sanitize_compile)
        LD_OPT_FLAGS.extend(sanitize_link)

    if skip_updates:
        log("FFmpeg updates disabled (--skip-updates)")
    if run_tests_flag:
        log("Unit/regression test suite enabled")
    if sanitize_regression_flag:
        log("Sanitizer regression cadence enabled")
    if run_integration_flag:
        if full_integration_flag:
            log("Integration mode: full matrix (--full-integration)")
        else:
            log("Integration mode: smoke (--run-integration-tests)")

    if format_flag:
        format_ok = run_format(env)
        if not format_ok:
            log("Auto-format completed with issues.")
        if action_args(args) == ["--format"] and not lint_flag and not run_tests_flag and not run_integration_flag:
            VERIFICATION_FINAL_EXIT_CODE = 0 if format_ok else 1
            sys.exit(VERIFICATION_FINAL_EXIT_CODE)

    standalone_lint = lint_flag and is_standalone_lint_invocation(args)
    if standalone_lint:
        lint_ok = run_lint(env, advisory=False)
        if not lint_ok:
            log("Lint/LSP checks reported issues.")
        VERIFICATION_FINAL_EXIT_CODE = 0 if lint_ok else 1
        sys.exit(VERIFICATION_FINAL_EXIT_CODE)

    if (verify_flag or default_quality_mode) and lint_flag and not sanitize_regression_child and not no_build_flag:
        run_verify_preflight(env)

    externals_prepared = False
    if sanitize_regression_flag and not sanitize_regression_child and not sanitize_flag and not no_build_flag:
        # The sanitizer child and final product build use the same non-instrumented
        # source dependency/FFmpeg closure. Prepare it once before either pass.
        external_start = time.time()
        compile_custom_ffmpeg(skip_updates=skip_updates)
        externals_prepared = True
        record_verification_step(
            "external_preparation",
            "passed",
            duration_seconds=time.time() - external_start,
            details={"skip_updates": skip_updates, "consumers": ["sanitizer", "product"]},
        )

    sanitizer_executor = None
    sanitizer_future = None
    parallel_product_jobs = 0
    parallel_sanitizer_jobs = 0
    original_job_setting = env.get("CE_BUILD_JOBS")
    if sanitize_regression_flag and not sanitize_regression_child:
        if sanitize_flag:
            log("Sanitizer regression cadence requested in sanitizer mode; skipping nested pass")
        elif "--force-rebuild" not in sys.argv and sanitizer_stage_cache_hit():
            log("Reusing exact-input sanitizer success manifest; all recorded inputs and outputs match")
            record_verification_step(
                "sanitize_regression",
                "cached",
                details={"manifest": SANITIZER_STAGE_MANIFEST},
            )
        else:
            # Populate the shared download cache before isolated parent/child
            # staging begins, avoiding concurrent archive creation.
            setup_fg_sdk_for_host(skip_updates=True)
            total_jobs = get_parallel_job_count(env, cpu_count())
            product_jobs, sanitizer_jobs = verification_parallel_job_counts(total_jobs)
            if sanitizer_jobs and not no_build_flag:
                parallel_product_jobs = product_jobs
                parallel_sanitizer_jobs = sanitizer_jobs
                env["CE_BUILD_JOBS"] = str(product_jobs)
                sanitizer_executor = ThreadPoolExecutor(max_workers=1)
                sanitizer_future = sanitizer_executor.submit(
                    run_sanitizer_regression_pass,
                    ccache_flag,
                    sanitizer_jobs,
                )
                log(
                    "Running isolated sanitizer validation concurrently with the clean product build "
                    f"(product jobs={product_jobs}, sanitizer jobs={sanitizer_jobs})"
                )
                record_verification_step(
                    "verification_parallelism",
                    "running",
                    details={"product_jobs": product_jobs, "sanitizer_jobs": sanitizer_jobs},
                )
            else:
                try:
                    run_sanitizer_regression_pass(ccache_flag=ccache_flag)
                except RuntimeError as error:
                    log(f"ERROR: {error}")
                    sys.exit(1)

    build_start = time.time()
    if no_build_flag:
        log("Build skipped (--no-build)")
        if run_tests_flag:
            tests_dir = TEST_OUTPUT_DIR
            test_exe = os.path.join(tests_dir, "unit_tests.exe")
            if not os.path.exists(test_exe):
                log(f"Error: {test_exe} not found. Build first without --no-build.")
                sys.exit(1)
            if not validate_cached_link_output(test_exe, env):
                log("Error: unit_tests.exe is stale or lacks a valid link-cache manifest. Rebuild tests first.")
                sys.exit(1)
            copy_test_runtime_dlls(tests_dir)
            if not run_tests(
                env,
                test_exe,
                gtest_filter=gtest_filter,
                run_python_tools=not sanitize_regression_child,
            ):
                sys.exit(1)
    else:
        compile_project(
            env,
            clang_bin,
            skip_updates=skip_updates,
            should_run_tests=run_tests_flag,
            gtest_filter=gtest_filter,
            tests_only=tests_only_flag,
            externals_prepared=externals_prepared,
            run_python_tools=not sanitize_regression_child,
        )
    record_verification_step(
        "build",
        "passed",
        duration_seconds=time.time() - build_start,
        details={
            "tests_only": tests_only_flag,
            "run_tests": run_tests_flag,
            "run_integration": run_integration_flag,
            "skip_updates": skip_updates,
            "sanitize": sanitize_flag,
            "no_build": no_build_flag,
            "resume": resume_flag,
        },
    )
    if sanitizer_future is not None:
        try:
            sanitizer_future.result()
            record_verification_step(
                "verification_parallelism",
                "passed",
                details={
                    "product_jobs": parallel_product_jobs,
                    "sanitizer_jobs": parallel_sanitizer_jobs,
                },
            )
        except (RuntimeError, SystemExit) as error:
            log(f"ERROR: Concurrent sanitizer validation failed: {error}")
            sys.exit(1)
        finally:
            assert sanitizer_executor is not None
            sanitizer_executor.shutdown()
            if original_job_setting is None:
                env.pop("CE_BUILD_JOBS", None)
            else:
                env["CE_BUILD_JOBS"] = original_job_setting

    # Publish the database before advisory lint so clang-tidy sees this build's
    # exact compiler, flags, and sources. The atexit hook still preserves a
    # partial database when compilation fails before reaching this point.
    if COMPILE_COMMANDS:
        write_compile_commands_json()
    compile_commands_path = get_compile_commands_path()
    if os.path.exists(compile_commands_path):
        record_verification_artifact("compile_commands", compile_commands_path)
        try:
            with open(compile_commands_path, "r", encoding="utf-8") as f:
                cc_data = json.load(f)
            record_verification_step(
                "compile_commands",
                "passed",
                details={"entries": len(cc_data), "sha256": sha256_file(compile_commands_path)},
            )
        except Exception:
            record_verification_step("compile_commands", "passed")

    if lint_flag:
        lint_ok = run_lint(env, advisory=True)
        if not lint_ok:
            log("Lint/LSP checks reported advisory issues; complete diagnostics were retained as artifacts.")
        if not tests_only_flag and not no_build_flag and not sanitize_flag and not sanitize_regression_child:
            refresh_full_compile_database_snapshot()

    if run_fuzz_flag:
        run_fuzz_targets(env, fuzz_seconds)

    if run_integration_flag:
        ensure_debug_logging()
        run_integration_tests(env, full_matrix=full_integration_flag)

    VERIFICATION_FINAL_EXIT_CODE = 0


if __name__ == "__main__":
    main()
