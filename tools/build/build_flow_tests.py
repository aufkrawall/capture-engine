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
# Fake frame-generation runtimes, built under the real module names (CE identifies runtimes by module):
# (DLL, sources under tests/flow/fakes - .cpp compiled, .rc through windres, .def handed to the linker).
FLOW_FAKE_MODULES = (
    ("sl.interposer.dll", ("streamline/sl_interposer.cpp", "streamline/sl_version.rc", "streamline/sl.interposer.def")),
    ("sl.common.dll", ("streamline/sl_common.cpp", "streamline/sl_version.rc")),
    ("sl.dlss_g.dll", ("streamline/sl_dlss_g.cpp", "streamline/sl_version.rc")),
    ("sl.reflex.dll", ("streamline/sl_reflex.cpp", "streamline/sl_version.rc")),
    ("sl.pcl.dll", ("streamline/sl_pcl.cpp", "streamline/sl_version.rc")),
    ("amd_fidelityfx_framegeneration_dx12.dll", ("fidelityfx/ffx_framegeneration.cpp",)),
    ("nvngx.dll", ("ngx/nvngx.cpp",)),
)
FLOW_TEST_TIMEOUT_SECONDS = 180
FLOW_TEST_PARALLEL_PROCESSES = 4


def flow_hook_cflags(hook_cflags: List[str]) -> List[str]:
    """Product hook flags without LTO, at -O1: the same code paths, compiled in a fraction of the time."""
    flags = [flag for flag in hook_cflags if not flag.startswith("-flto")]
    return [("-O1" if flag in ("-O2", "-O3") else flag) for flag in flags] + ["-DCE_FLOW_TEST"]


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
        "-I" + os.path.join(MSYS2_DIR, "clang64", "include"),
        "-I" + os.path.join(FG_SDK_INCLUDE_DIR, "streamline", "include"),
        "-I" + os.path.join(FG_SDK_INCLUDE_DIR, "fidelityfx", "Kits", "FidelityFX", "api", "include"),
        "-I" + os.path.join(FG_SDK_INCLUDE_DIR, "fidelityfx", "Kits", "FidelityFX", "framegeneration", "include"),
    ]
    def host_objects(sources):
        pairs = []
        for src in sources:
            rel_path = os.path.relpath(src, PROJECT_ROOT)
            pairs.append((src, os.path.join(obj_dir, os.path.splitext(rel_path)[0] + ".host.o").replace("\\", "/")))
        return pairs

    host_pairs = host_objects(flow_test_host_sources())
    parallel_compile(env, clang_exe, host_cflags, host_pairs)
    # The game plays CaptureEngine's inject host (it publishes the host memory with the host's own
    # UpdateSharedMemoryFromConfig), so it links that unit and common - kept out of compile_commands.json.
    host_side_pairs = host_objects(list(common_sources()) + [find_module_source("captureengine", "inject_config.cpp")])
    compile_commands_snapshot = list(COMPILE_COMMANDS)
    try:
        parallel_compile(env, clang_exe, host_cflags, host_side_pairs)
    finally:
        COMPILE_COMMANDS[:] = compile_commands_snapshot
    host_pairs += host_side_pairs
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
        "-lole32",
        "-lshell32",
        "-lshlwapi",
        "-lbcrypt",
        "-lversion",
        "-luuid",
        "-lws2_32",
        "-lgdi32",
        "-lpsapi",
        "-ldbghelp",
    ]
    append_windows_pdb_linker_flag(exe_ldflags, flow_exe)
    exe_cmd = [clang_exe] + [obj for _, obj in host_pairs] + exe_ldflags + ["-o", flow_exe]
    run_cached_link(
        exe_cmd,
        env,
        flow_exe,
        required_outputs=[flow_exe] + ([pdb_path_for_binary(flow_exe)] if IS_WINDOWS else []),
    )
    build_flow_fake_modules(env, clang_exe, host_cflags, obj_dir)
    shutil.copy2(
        os.path.join(PROJECT_ROOT, "captureengine", "config.ini.template"),
        os.path.join(FLOW_TEST_OUTPUT_DIR, "config.ini"),
    )
    return flow_exe


def build_flow_fake_modules(env, clang_exe, host_cflags, obj_dir) -> None:
    fakes_dir = os.path.join(FLOW_TEST_SOURCE_DIR, "fakes")
    fake_cflags = host_cflags
    compile_pairs, links = [], []
    for dll, sources in FLOW_FAKE_MODULES:
        module_obj_dir = os.path.join(obj_dir, "fakes", os.path.splitext(dll)[0])
        inputs = []
        for source in sources:
            path = os.path.join(fakes_dir, *source.split("/"))
            stem = os.path.splitext(os.path.basename(source))[0]
            if source.endswith(".cpp"):
                obj = os.path.join(module_obj_dir, stem + ".o").replace("\\", "/")
                compile_pairs.append((path, obj))
                inputs.append(obj)
            elif source.endswith(".rc"):
                res_obj = os.path.join(module_obj_dir, stem + ".res.o").replace("\\", "/")
                os.makedirs(module_obj_dir, exist_ok=True)
                run_command([get_windres_exe("x64"), path, "-o", res_obj], env=env, cwd=os.path.dirname(path))
                inputs.append(res_obj)
            else:
                inputs.append(path)
        links.append((dll, inputs))
    parallel_compile(env, clang_exe, fake_cflags, compile_pairs)
    for dll, inputs in links:
        output = os.path.join(FLOW_TEST_OUTPUT_DIR, dll)
        ldflags = ["-shared", "-static", "-fuse-ld=lld", "-ld3d12", "-ldxgi", "-luser32"]
        import_library = os.path.join(FLOW_TEST_OUTPUT_DIR, "sl.interposer.lib") if dll == "sl.interposer.dll" else None
        if import_library:
            ldflags.append("-Wl,--out-implib=" + import_library)
        append_windows_pdb_linker_flag(ldflags, output)
        run_cached_link(
            [clang_exe] + inputs + ldflags + ["-o", output],
            env,
            output,
            required_outputs=[output] + ([pdb_path_for_binary(output)] if IS_WINDOWS else [])
            + ([import_library] if import_library else []),
        )
    probe_source = os.path.join(fakes_dir, "startup", "static_import_probe.cpp")
    probe_object = os.path.join(obj_dir, "fakes", "startup", "static_import_probe.o")
    parallel_compile(env, clang_exe, fake_cflags, [(probe_source, probe_object)])
    probe = os.path.join(FLOW_TEST_OUTPUT_DIR, "static_import_probe.exe")
    flags = ["-static", "-fuse-ld=lld", "-lpsapi"]
    append_windows_pdb_linker_flag(flags, probe)
    run_cached_link(
        [clang_exe, probe_object, os.path.join(FLOW_TEST_OUTPUT_DIR, "sl.interposer.lib")] + flags + ["-o", probe],
        env, probe, required_outputs=[probe] + ([pdb_path_for_binary(probe)] if IS_WINDOWS else []),
    )
    launcher_source = os.path.join(fakes_dir, "startup", "startup_launcher_probe.cpp")
    launcher_object = os.path.join(obj_dir, "fakes", "startup", "startup_launcher_probe.o")
    parallel_compile(env, clang_exe, fake_cflags, [(launcher_source, launcher_object)])
    launcher = os.path.join(FLOW_TEST_OUTPUT_DIR, "startup_launcher_probe.exe")
    flags = ["-static", "-fuse-ld=lld"]
    append_windows_pdb_linker_flag(flags, launcher)
    run_cached_link([clang_exe, launcher_object] + flags + ["-o", launcher], env, launcher,
                    required_outputs=[launcher] + ([pdb_path_for_binary(launcher)] if IS_WINDOWS else []))


def build_flow_tests_standalone(env) -> Optional[str]:
    """The --tests-only --flow-tests loop: the flow harness alone, from the x64 hook flags."""
    clang_exe = get_compiler_exe("x64")
    vulkan_lib = get_linux_vulkan_import_lib_path("x64")
    if clang_exe is None or vulkan_lib is None:
        log("FG flow tests: x64 compiler or Vulkan import library unavailable")
        return None
    mingw_lib = "" if IS_LINUX else os.path.join(MSYS2_DIR, "clang64", "lib")
    excluded = {os.path.join(PROJECT_ROOT, *rel.split("/")) for rel in HOOK_DLL_EXCLUDED_SOURCES}
    hook_sources = [src for src in hook_dll_sources() if src not in excluded]
    return build_flow_tests(
        env,
        clang_exe,
        make_hook_cflags("x64", clang_exe),
        make_hook_ldflags("x64", clang_exe, mingw_lib, "", vulkan_lib),
        hook_sources,
    )


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
