

def ensure_debug_logging():
    """Ensure at least log_level=debug in bin/config.ini."""
    config_path = os.path.join(BIN_DIR, "config.ini")
    if not os.path.exists(config_path):
        log("config.ini missing, skipping debug_logging check.")
        return

    try:
        with open(config_path, "r") as f:
            lines = f.readlines()

        changed = False
        new_lines = []
        saw_log_level = False
        for line in lines:
            stripped = line.strip()
            if stripped.startswith("log_level="):
                saw_log_level = True
                level = stripped.split("=", 1)[1].strip().lower()
                if level in {"off", "error", "warn", "info"}:
                    new_lines.append("log_level=debug\n")
                    changed = True
                else:
                    new_lines.append(line)
            elif stripped.startswith("debug_logging="):
                if "true" not in stripped.lower():
                    new_lines.append("debug_logging=true\n")
                    changed = True
                else:
                    new_lines.append(line)
            else:
                new_lines.append(line)

        if not saw_log_level:
            inserted = False
            for i, line in enumerate(new_lines):
                if line.strip().lower() == "[logging]":
                    new_lines.insert(i + 1, "log_level=debug\n")
                    inserted = True
                    changed = True
                    break
            if not inserted:
                new_lines = [
                    "[Logging]\n",
                    "log_level=debug\n",
                    "\n",
                    *new_lines,
                ]
                changed = True

        if changed:
            with open(config_path, "w") as f:
                f.writelines(new_lines)
            log("Forced log_level=debug in config.ini for testing.")
    except Exception as e:
        log(f"Warning: Failed to update config.ini: {e}")


def verification_parallel_job_counts(total_jobs: int) -> Tuple[int, int]:
    """Reserve most workers for the larger product build and the rest for sanitizer validation."""
    if total_jobs < 2:
        return total_jobs, 0
    sanitizer_jobs = max(1, (total_jobs + 2) // 3)
    return total_jobs - sanitizer_jobs, sanitizer_jobs


def sanitizer_stage_outputs() -> List[str]:
    capture_dir = os.path.join(SANITIZER_STAGE_ROOT, "installed", "captureengine")
    return [
        os.path.join(SANITIZER_STAGE_ROOT, "tests", "unit_tests.exe"),
        os.path.join(capture_dir, "captureengine.exe"),
        os.path.join(capture_dir, "mediaengine.dll"),
        os.path.join(capture_dir, "capture_hook_x64.dll"),
        os.path.join(capture_dir, "VK_LAYER_CE_overlay.dll"),
    ]


def sanitizer_stage_discovered_inputs() -> List[str]:
    return discover_project_inputs(PROJECT_ROOT)


def sanitizer_stage_cache_hit() -> bool:
    return success_manifest_matches(
        SANITIZER_STAGE_MANIFEST,
        discovered_inputs=sanitizer_stage_discovered_inputs(),
    )


def sanitizer_stage_link_inputs() -> List[str]:
    """Recover the exact linker/object/library closure recorded by stage link manifests."""
    return collect_link_manifest_inputs(
        SANITIZER_STAGE_ROOT,
        lambda command, cwd: collect_link_dependency_paths(command, command[0], cwd or PROJECT_ROOT),
    )


def record_sanitizer_stage_success() -> None:
    glslang, spirv_val = get_vulkan_fg_shader_tools()
    discovered, inputs = collect_stage_inputs(
        project_root=PROJECT_ROOT,
        stage_root=SANITIZER_STAGE_ROOT,
        extra_files=[
            get_compiler_exe("x64") or "",
            get_windres_exe("x64"),
            get_llvm_readobj_exe(),
            glslang,
            spirv_val,
            *sanitizer_stage_link_inputs(),
        ],
        extra_roots=[
            os.path.join(FFMPEG_DIR, "lib"),
            os.path.join(FFMPEG_DIR, "bin"),
            os.path.join(MSYS2_DIR, "var", "lib", "pacman", "local"),
        ],
    )
    write_success_manifest(
        SANITIZER_STAGE_MANIFEST,
        discovered_inputs=discovered,
        all_inputs=inputs,
        outputs=sanitizer_stage_outputs(),
    )


def sanitizer_regression_command(ccache_flag: bool, jobs: Optional[int] = None) -> List[str]:
    """Build the isolated sanitizer command; external inputs are already prepared by the parent."""
    cmd = [
        sys.executable,
        os.path.abspath(__file__),
        "--run-tests",
        "--sanitize",
        "--incremental",
        "--sanitize-regression-child",
        "--skip-updates",
        "--concise",
    ]
    if ccache_flag:
        cmd.append("--ccache")
    if jobs:
        cmd.append(f"--jobs={jobs}")
    return cmd


def run_sanitizer_regression_pass(ccache_flag: bool, jobs: Optional[int] = None) -> None:
    """Run a second validation pass with ASan/UBSan + unit tests."""
    cmd = sanitizer_regression_command(ccache_flag, jobs)

    log("=== Running sanitizer regression cadence pass ===")
    sanitizer_start = time.time()
    sanitizer_log = verification_artifact_path("sanitize_regression.log")
    sanitizer_detail_log = verification_artifact_path("sanitize_regression.details.log")
    if sanitizer_log:
        cmd.append(f"--log-file={sanitizer_log}")
    if sanitizer_detail_log:
        cmd.append(f"--detail-log={sanitizer_detail_log}")
    child_env = os.environ.copy()
    child_env["CE_ISOLATED_BUILD_ROOT"] = SANITIZER_STAGE_ROOT
    result = run_logged_subprocess(cmd, cwd=PROJECT_ROOT, env=child_env)
    elapsed = time.time() - sanitizer_start
    if result.returncode != 0:
        log(f"ERROR: Sanitizer regression pass failed (exit code {result.returncode})")
        record_verification_step(
            "sanitize_regression",
            "failed",
            duration_seconds=elapsed,
            details={"exit_code": result.returncode},
        )
        raise RuntimeError(f"sanitizer regression failed with exit code {result.returncode}")
    log("=== Sanitizer regression cadence pass: OK ===")
    if sanitizer_log:
        record_verification_artifact("sanitize_regression_log", sanitizer_log)
    if sanitizer_detail_log:
        record_verification_artifact("sanitize_regression_detail_log", sanitizer_detail_log)
    record_sanitizer_stage_success()
    record_verification_step("sanitize_regression", "passed", duration_seconds=elapsed)


# Each harness must declare its seed corpus. A missing entry fails the stage rather
# than fuzzing from an empty corpus, which is how the original harnesses rotted
# unnoticed: they were never built, never run, and their corpora were empty.
FUZZ_TARGET_CORPUS = {
    "fuzz_config_parser.cpp": "config",
    "fuzz_ipc_deserialize.cpp": "ipc",
}

# libFuzzer instrumentation must be requested manually: the clang driver rejects
# -fsanitize=fuzzer for the x86_64-w64-windows-gnu target even though the runtime
# archive ships with the toolchain.
FUZZ_COVERAGE_FLAGS = [
    "-fsanitize-coverage=inline-8bit-counters,trace-cmp,trace-div,trace-gep,pc-table",
]

# Two narrowly-scoped ASan relaxations, both for reports that arise entirely outside
# project code on this MinGW toolchain and were verified to contain no project frames:
#   detect_container_overflow=0  MSYS2 ships a non-instrumented libc++, so mixing it
#                                with instrumented TUs reports false container
#                                overflows inside libFuzzer's own corpus reader.
#   intercept_strlen=0           ASan's strlen interceptor fires on ucrtbase/ntdll
#                                internal buffers during libFuzzer driver start-up.
# Heap use-after-free, double-free, and out-of-bounds detection stay fully enabled.
FUZZ_ASAN_OPTIONS = "detect_container_overflow=0:intercept_strlen=0"


def find_libfuzzer_archive() -> Optional[str]:
    """Locate libclang_rt.fuzzer for the host clang; None when unavailable."""
    pattern = os.path.join(MSYS2_DIR, "clang64", "lib", "clang", "*", "lib", "windows", "libclang_rt.fuzzer-x86_64.a")
    matches = sorted(glob.glob(pattern))
    return matches[-1] if matches else None


def run_fuzz_targets(env, max_total_time: int) -> None:
    """Build and run every libFuzzer harness in tests/fuzz against its seed corpus."""
    fuzz_src_dir = os.path.join(PROJECT_ROOT, "tests", "fuzz")
    harnesses = sorted(glob.glob(os.path.join(fuzz_src_dir, "*.cpp")))
    if not harnesses:
        log("ERROR: --run-fuzz was requested but tests/fuzz contains no harness")
        record_verification_step("fuzz", "failed", details={"reason": "no_harness"})
        sys.exit(2)

    if IS_LINUX:
        log("ERROR: fuzz targets require the native Windows Clang toolchain")
        record_verification_step("fuzz", "failed", details={"reason": "unsupported_host"})
        sys.exit(2)

    clang_exe = get_compiler_exe("x64")
    if clang_exe is None:
        log("ERROR: x64 compiler discovery failed; cannot build fuzz targets")
        record_verification_step("fuzz", "failed", details={"reason": "missing_compiler"})
        sys.exit(2)

    fuzzer_archive = find_libfuzzer_archive()
    if not fuzzer_archive:
        # Fail closed. A silently skipped fuzz stage is the exact failure this
        # infrastructure exists to prevent.
        log("ERROR: libclang_rt.fuzzer-x86_64.a not found in the MSYS2 Clang toolchain")
        record_verification_step("fuzz", "failed", details={"reason": "missing_libfuzzer"})
        sys.exit(2)

    log(f"=== Building fuzz targets ({len(harnesses)}) ===")
    fuzz_start = time.time()
    obj_dir = os.path.join(OBJ_DIR, "x64-fuzz")
    out_dir = os.path.join(BUILD_DIR, "fuzz")
    os.makedirs(out_dir, exist_ok=True)

    base_cflags = [
        "-std=c++20",
        "-g",
        "-O1",
        "-fno-omit-frame-pointer",
        "-fsanitize=address",
        "-DCE_FUZZING",
        "-I" + PROJECT_ROOT,
        "-I" + os.path.join(PROJECT_ROOT, "common"),
        "-I" + os.path.join(PROJECT_ROOT, "hook", "common"),
    ] + FUZZ_COVERAGE_FLAGS

    # Link the whole common/ tree rather than a hand-maintained per-harness source
    # list. A curated list silently rots when the code under test grows a new
    # dependency, which is precisely how the previous harnesses stopped linking.
    compile_tasks = []
    common_objs = []
    for src in sorted(glob.glob(os.path.join(PROJECT_ROOT, "common", "*.cpp"))):
        rel_path = os.path.relpath(src, PROJECT_ROOT)
        obj = os.path.join(obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
        compile_tasks.append((base_cflags, src, obj))
        common_objs.append(obj)

    harness_objs = {}
    for src in harnesses:
        name = os.path.splitext(os.path.basename(src))[0]
        obj = os.path.join(obj_dir, "tests", "fuzz", name + ".o").replace("\\", "/")
        compile_tasks.append((base_cflags, src, obj))
        harness_objs[src] = obj

    # These sanitizer/coverage compilations must not reach compile_commands.json:
    # they cover the same common/*.cpp files as the product build and would replace
    # the real product flags in the LSP and clang-tidy view of those sources.
    compile_commands_snapshot = list(COMPILE_COMMANDS)
    try:
        parallel_compile_varied(env, clang_exe, compile_tasks)
    finally:
        COMPILE_COMMANDS[:] = compile_commands_snapshot

    fuzz_ldflags = [
        "-fsanitize=address",
        "-lshlwapi",
        "-lbcrypt",
        "-ladvapi32",
        "-lole32",
        "-luser32",
        "-lshell32",
        "-lversion",
        "-lpsapi",
        "-ldxgi",
        "-ld3d11",
        "-ldxguid",
        "-lgdi32",
        "-lwinmm",
        "-ldbghelp",
        "-lshcore",
        "-luuid",
        "-lpdh",
    ]

    executables = {}
    for src, obj in harness_objs.items():
        name = os.path.splitext(os.path.basename(src))[0]
        exe = os.path.join(out_dir, name + ".exe")
        run_command([clang_exe, obj] + common_objs + [fuzzer_archive] + fuzz_ldflags + ["-o", exe], env=env)
        executables[src] = exe
    log(f"Fuzz targets built: {len(executables)}")

    # libFuzzer binaries resolve libc++/ASan from the toolchain, not the product bundle.
    fuzz_env = env.copy()
    clang_bin = os.path.join(MSYS2_DIR, "clang64", "bin")
    fuzz_env["PATH"] = clang_bin + os.pathsep + fuzz_env.get("PATH", "")
    fuzz_env["ASAN_OPTIONS"] = FUZZ_ASAN_OPTIONS

    results = {}
    for src, exe in sorted(executables.items()):
        harness_name = os.path.basename(src)
        corpus_name = FUZZ_TARGET_CORPUS.get(harness_name)
        if not corpus_name:
            log(f"ERROR: fuzz harness {harness_name} has no seed corpus registered in FUZZ_TARGET_CORPUS")
            record_verification_step(
                "fuzz",
                "failed",
                details={"reason": "unregistered_harness", "harness": harness_name},
            )
            sys.exit(2)
        corpus_dir = os.path.join(fuzz_src_dir, "corpus", corpus_name)
        seeds = [f for f in glob.glob(os.path.join(corpus_dir, "*")) if os.path.isfile(f)]
        if not seeds:
            log(f"ERROR: fuzz corpus {corpus_dir} is empty; refusing to fuzz without seeds")
            record_verification_step("fuzz", "failed", details={"reason": "empty_corpus", "corpus": corpus_name})
            sys.exit(2)

        target_name = os.path.splitext(harness_name)[0]
        log(f"Fuzzing {target_name} for {max_total_time}s ({len(seeds)} seed(s))...")
        artifact_dir = verification_artifact_path(f"fuzz_{corpus_name}") or os.path.join(
            out_dir, f"artifacts_{corpus_name}"
        )
        os.makedirs(artifact_dir, exist_ok=True)
        # libFuzzer writes newly discovered units into the FIRST corpus directory and
        # treats later ones as read-only seeds. Keep generated units in a scratch dir
        # so the committed seed corpus stays a curated, reviewable test asset.
        scratch_corpus = os.path.join(out_dir, f"corpus_{corpus_name}")
        os.makedirs(scratch_corpus, exist_ok=True)
        cmd = [
            exe,
            f"-max_total_time={max_total_time}",
            "-print_final_stats=1",
            f"-artifact_prefix={os.path.join(artifact_dir, '')}",
            scratch_corpus,
            corpus_dir,
        ]
        result = run_logged_subprocess(cmd, cwd=out_dir, env=fuzz_env)
        write_process_diagnostics_artifact(f"fuzz_{corpus_name}_diagnostics", f"fuzz_{corpus_name}.log", cmd, result)
        executed = 0
        match = re.search(r"stat::number_of_executed_units:\s*(\d+)", result.stdout or "")
        if match:
            executed = int(match.group(1))
        results[target_name] = {"exit_code": result.returncode, "executed_units": executed}

        if result.returncode != 0:
            log(f"ERROR: fuzz target {target_name} failed (exit code {result.returncode})")
            log(f"Reproducer and complete output: {artifact_dir}")
            record_verification_step(
                "fuzz",
                "failed",
                duration_seconds=time.time() - fuzz_start,
                details={"target": target_name, "results": results},
            )
            record_verification_coverage("fuzz", "failed")
            sys.exit(result.returncode)

        # A target that links and starts but never reaches the harness body would
        # pass silently and rebuild the original blind spot; require real execution.
        if executed <= 0:
            log(f"ERROR: fuzz target {target_name} executed no units; it never reached LLVMFuzzerTestOneInput")
            record_verification_step(
                "fuzz",
                "failed",
                duration_seconds=time.time() - fuzz_start,
                details={"target": target_name, "reason": "no_units_executed", "results": results},
            )
            record_verification_coverage("fuzz", "failed")
            sys.exit(2)
        log(f"Fuzz target {target_name}: OK ({executed} units executed)")

    elapsed = time.time() - fuzz_start
    log("=== Fuzz targets passed ===")
    record_verification_step("fuzz", "passed", duration_seconds=elapsed, details={"results": results})
    record_verification_coverage("fuzz", f"x64_libfuzzer_asan_{max_total_time}s")


def parse_flag_value(flag_name: str):
    for i, arg in enumerate(sys.argv):
        if arg == flag_name and i + 1 < len(sys.argv):
            value = sys.argv[i + 1]
            return None if value.startswith("--") else value
        if arg.startswith(flag_name + "="):
            return arg.split("=", 1)[1]
    return None


PRESENTATION_FLAGS = {"--concise", "--verbose-commands"}
PRESENTATION_VALUE_FLAGS = {"--jobs", "--log-file", "--detail-log"}


def action_args(args: List[str]) -> List[str]:
    """Remove presentation-only options without changing the requested build policy."""
    actions: List[str] = []
    index = 0
    while index < len(args):
        arg = args[index]
        if arg in PRESENTATION_FLAGS:
            index += 1
            continue
        if arg in PRESENTATION_VALUE_FLAGS:
            index += 1
            if index < len(args) and not args[index].startswith("--"):
                index += 1
            continue
        if any(arg.startswith(flag + "=") for flag in PRESENTATION_VALUE_FLAGS):
            index += 1
            continue
        actions.append(arg)
        index += 1
    return actions


def is_default_quality_invocation(args: List[str]) -> bool:
    return not action_args(args)


def is_standalone_lint_invocation(args: List[str]) -> bool:
    """Return whether lint is the only requested action and therefore an explicit gate."""
    return action_args(args) == ["--lint"]
