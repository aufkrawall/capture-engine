# FG flow tests: the hook's real frame-generation switching code in an isolated test process.
#
# build/flow_tests/ holds a flow-test build of the x64 hook DLL (every hook DLL source except DllMain,
# the common sources and tests/flow/flow_hook_entry.cpp, compiled with the product hook flags minus LTO),
# the fake runtime DLLs and fg_flow_tests.exe (a WARP D3D12 game plus the scenarios). Each scenario runs
# in its own process because the hook's state is process-global. Design: llm-wiki/refactor-roadmap.md
# ("FG flow harness").

FLOW_TEST_SOURCE_DIR = os.path.join(PROJECT_ROOT, "tests", "flow")
FLOW_TEST_OUTPUT_DIR = (
    os.path.join(ISOLATED_BUILD_ROOT, "flow_tests") if ISOLATED_BUILD_ROOT else os.path.join(BUILD_DIR, "flow_tests")
)
FLOW_HOOK_ENTRY_SOURCE = "flow_hook_entry.cpp"
FLOW_TEST_TIMEOUT_SECONDS = 180
FLOW_TEST_PARALLEL_PROCESSES = 4


def flow_hook_cflags(hook_cflags: List[str]) -> List[str]:
    """Product hook flags without LTO, at -O1: the same code paths, compiled in a fraction of the time."""
    flags = [flag for flag in hook_cflags if not flag.startswith("-flto")]
    return [("-O1" if flag in ("-O2", "-O3") else flag) for flag in flags]


def flow_test_host_sources() -> List[str]:
    return sorted(
        os.path.join(FLOW_TEST_SOURCE_DIR, name)
        for name in os.listdir(FLOW_TEST_SOURCE_DIR)
        if name.endswith(".cpp") and name != FLOW_HOOK_ENTRY_SOURCE
    )


def build_flow_tests(env, clang_exe, hook_cflags, hook_ldflags, hook_sources) -> Optional[str]:
    """Builds the flow-test hook DLL and fg_flow_tests.exe; returns the executable, None when skipped."""
    if not os.path.isdir(FLOW_TEST_SOURCE_DIR):
        return None
    log("Compiling FG flow tests (hook DLL test build + WARP game)...")
    os.makedirs(FLOW_TEST_OUTPUT_DIR, exist_ok=True)
    obj_dir = os.path.join(OBJ_DIR, "x64-flow")

    dllmain = os.path.join(PROJECT_ROOT, "hook", "runtime", "main_dllmain.cpp")
    dll_sources = [src for src in hook_sources if os.path.normcase(src) != os.path.normcase(dllmain)]
    dll_sources += [src for src in common_sources()]
    dll_sources.append(os.path.join(FLOW_TEST_SOURCE_DIR, FLOW_HOOK_ENTRY_SOURCE))
    dll_pairs = []
    for src in dll_sources:
        rel_path = os.path.relpath(src, PROJECT_ROOT)
        dll_pairs.append((src, os.path.join(obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")))
    # These objects repeat product sources with test flags; like the fuzz build they must not replace the
    # product entries clangd and the clang-tidy ratchet read from compile_commands.json.
    compile_commands_snapshot = list(COMPILE_COMMANDS)
    try:
        parallel_compile(env, clang_exe, flow_hook_cflags(hook_cflags), dll_pairs)
    finally:
        COMPILE_COMMANDS[:] = compile_commands_snapshot

    flow_dll = os.path.join(FLOW_TEST_OUTPUT_DIR, "capture_hook_x64.dll")
    dll_ldflags = [flag for flag in hook_ldflags if not flag.startswith(("-Wl,--pdb=", "-Wl,/pdbaltpath:"))]
    append_windows_pdb_linker_flag(dll_ldflags, flow_dll)
    dll_cmd = [clang_exe] + [obj for _, obj in dll_pairs] + dll_ldflags + ["-o", flow_dll]
    dll_required = [flow_dll] + ([pdb_path_for_binary(flow_dll)] if IS_WINDOWS else [])
    run_cached_link(
        dll_cmd,
        env,
        flow_dll,
        required_outputs=dll_required,
        execute_command=prepare_command_with_response_file(dll_cmd, os.path.join(obj_dir, "flow_hook_link.rsp")),
    )

    host_cflags = make_cpp_cflags(UNIT_TEST_OPT_FLAGS_X64, compiler_exe=clang_exe) + [
        "-I" + os.path.join(MSYS2_DIR, "clang64", "include")
    ]
    host_pairs = []
    for src in flow_test_host_sources():
        rel_path = os.path.relpath(src, PROJECT_ROOT)
        host_pairs.append((src, os.path.join(obj_dir, os.path.splitext(rel_path)[0] + ".host.o").replace("\\", "/")))
    parallel_compile(env, clang_exe, host_cflags, host_pairs)
    flow_exe = os.path.join(FLOW_TEST_OUTPUT_DIR, "fg_flow_tests.exe")
    msys_lib = os.path.join(MSYS2_DIR, "clang64", "lib")
    exe_ldflags = [
        "-static",
        "-fuse-ld=lld",
        os.path.join(msys_lib, "libgtest_main.a"),
        os.path.join(msys_lib, "libgtest.a"),
        "-ld3d12",
        "-ldxgi",
        "-luser32",
        "-ladvapi32",
    ]
    append_windows_pdb_linker_flag(exe_ldflags, flow_exe)
    exe_cmd = [clang_exe] + [obj for _, obj in host_pairs] + exe_ldflags + ["-o", flow_exe]
    run_cached_link(
        exe_cmd,
        env,
        flow_exe,
        required_outputs=[flow_exe] + ([pdb_path_for_binary(flow_exe)] if IS_WINDOWS else []),
    )
    shutil.copy2(
        os.path.join(PROJECT_ROOT, "captureengine", "config.ini.template"),
        os.path.join(FLOW_TEST_OUTPUT_DIR, "config.ini"),
    )
    return flow_exe


def list_flow_tests(flow_exe: str, gtest_filter: Optional[str]) -> List[str]:
    cmd = [flow_exe, "--gtest_list_tests"] + ([f"--gtest_filter={gtest_filter}"] if gtest_filter else [])
    output = subprocess.run(cmd, capture_output=True, text=True, timeout=60, cwd=FLOW_TEST_OUTPUT_DIR).stdout
    tests, suite = [], ""
    for line in output.splitlines():
        if line.startswith("  ") and suite:
            tests.append(suite + line.split()[0])
        elif line.strip().endswith("."):
            suite = line.strip()
    return tests


def run_flow_tests(env, flow_exe: Optional[str], gtest_filter: Optional[str] = None) -> bool:
    """Runs every flow scenario in its own process; returns False on any failure."""
    if not flow_exe or not os.path.exists(flow_exe):
        return True
    tests = list_flow_tests(flow_exe, gtest_filter)
    if not tests:
        if gtest_filter:
            log(f"No FG flow test matches the filter {gtest_filter}")
        return True
    log(f"=== Running FG flow tests ({len(tests)} scenario process(es)) ===")
    start = time.time()

    def run_one(name: str):
        logs = os.path.join(FLOW_TEST_OUTPUT_DIR, "logs", name)
        shutil.rmtree(logs, ignore_errors=True)
        try:
            result = subprocess.run(
                [flow_exe, f"--gtest_filter={name}"],
                capture_output=True,
                text=True,
                encoding="utf-8",
                errors="replace",
                timeout=FLOW_TEST_TIMEOUT_SECONDS,
                cwd=FLOW_TEST_OUTPUT_DIR,
            )
            return name, result.returncode, result.stdout + result.stderr
        except subprocess.TimeoutExpired as timeout:
            output = (timeout.stdout or b"").decode("utf-8", "replace") if isinstance(timeout.stdout, bytes) else ""
            return name, -1, f"timed out after {FLOW_TEST_TIMEOUT_SECONDS} s\n{output}"

    failures = []
    with ThreadPoolExecutor(max_workers=min(FLOW_TEST_PARALLEL_PROCESSES, len(tests))) as executor:
        for name, returncode, output in executor.map(run_one, tests):
            if returncode != 0:
                failures.append(name)
                log(f"[flow:{name}] FAILED (exit code {returncode}); hook log: "
                    f"{os.path.join(FLOW_TEST_OUTPUT_DIR, 'logs', name)}")
                log_unit_test_output_tail(f"flow:{name}", output)
    elapsed = time.time() - start
    record_verification_step(
        "flow_tests",
        "failed" if failures else "passed",
        duration_seconds=elapsed,
        details={"scenarios": len(tests), "failed": failures, "gtest_filter": gtest_filter},
    )
    if failures:
        log(f"=== FG flow tests FAILED ({len(failures)} of {len(tests)}) ===")
        return False
    log(f"=== FG flow tests passed ({len(tests)} scenarios, {elapsed:.1f}s) ===")
    return True
