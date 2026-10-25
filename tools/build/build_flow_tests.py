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
    ("sl.interposer.dll", ("streamline/sl_interposer.cpp", "streamline/sl_interposer_identity.cpp",
                           "streamline/sl_version.rc", "streamline/sl.interposer.def")),
    # The resident copy for the takeover scenario whose game imports a name the configured copy lacks.
    ("takeover_resident/sl.interposer.dll", ("streamline/sl_interposer.cpp", "streamline/sl_interposer_identity.cpp",
                                             "streamline/sl_interposer_resident_only.cpp", "streamline/sl_version.rc",
                                             "streamline/sl.interposer.def")),
    ("sl.common.dll", ("streamline/sl_common.cpp", "streamline/sl_version.rc")),
    ("sl.dlss_g.dll", ("streamline/sl_dlss_g.cpp", "streamline/sl_version.rc")),
    ("sl.reflex.dll", ("streamline/sl_reflex.cpp", "streamline/sl_version.rc")),
    ("sl.pcl.dll", ("streamline/sl_pcl.cpp", "streamline/sl_version.rc")),
    ("amd_fidelityfx_framegeneration_dx12.dll", ("fidelityfx/ffx_framegeneration.cpp",)),
    # A tool's own FidelityFX copy under the same name (OptiScaler's folder), loaded beside the game's.
    ("optiscaler/amd_fidelityfx_framegeneration_dx12.dll", ("fidelityfx/ffx_framegeneration.cpp",)),
    # A tool's module serving the game's DLSS-G API in Streamline's place (OptiScaler's slGetFeatureFunction hook).
    ("optiscaler/dlssg_api_tool.dll", ("tool/dlssg_api_tool.cpp",)),
    ("libxess_fg.dll", ("xefg/xefg_swapchain.cpp",)),
    ("libxell.dll", ("xefg/xell.cpp",)),
    ("nvngx.dll", ("ngx/nvngx.cpp",)),
    # A stand-in named like a third-party overlay hook (RivaTuner): the foreign-overlay report must name it.
    ("third_party/RTSSHooks64.dll", ("overlay/third_party_overlay_stand_in.cpp",)),
    ("third_party/capture_hook_x64.dll", ("overlay/third_party_overlay_stand_in.cpp",)),
    # One d3d9.dll file name, two translators told apart by version resource (FlowDX11OverlayApiLabel).
    ("dxup/d3d9.dll", ("translators/frontend_stand_in.cpp", "translators/dxup_version.rc")),
    ("reshade/d3d9.dll", ("translators/frontend_stand_in.cpp", "translators/reshade_version.rc")),
)
# Game modules that statically import a fake interposer: (DLL, source, interposer build whose import library
# it links). The in-process sl.interposer.dll takeover scenarios load them from a vendor folder.
FLOW_TAKEOVER_GAME_MODULES = (
    ("takeover_game.dll", "startup/takeover_game_module.cpp", "sl.interposer.dll"),
    ("takeover_game_unsupported.dll", "startup/takeover_game_unsupported.cpp", "takeover_resident/sl.interposer.dll"),
)
FLOW_TEST_TIMEOUT_SECONDS = 300
FLOW_TEST_PARALLEL_PROCESSES = 4
# Each scenario's wall time from the last run, read back to start the longest scenarios first.
FLOW_TEST_DURATIONS_FILE = os.path.join(FLOW_TEST_OUTPUT_DIR, "scenario_seconds.json")
FLOW_TEST_SLOWEST_REPORTED = 8
# The input key of the last unfiltered run that passed: the same inputs cannot change the result.
FLOW_TEST_PASS_FILE = os.path.join(FLOW_TEST_OUTPUT_DIR, "passed_inputs.json")
FLOW_TEST_RERUN_FLAG = "--rerun-flow-tests"
# The Windows side of every scenario: the WARP device and the D3D12 runtime and debug layer it validates with.
FLOW_TEST_SYSTEM_MODULES = ("d3d10warp.dll", "D3D12Core.dll", "d3d12SDKLayers.dll", "d3d12.dll", "dxgi.dll")


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
        "-DCE_FLOW_TEST",  # the game is the flow hook's host: both sides spell session names alike
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
        output = os.path.join(FLOW_TEST_OUTPUT_DIR, *dll.split("/"))
        os.makedirs(os.path.dirname(output), exist_ok=True)
        ldflags = ["-shared", "-static", "-fuse-ld=lld", "-ld3d12", "-ldxgi", "-luser32"]
        import_library = (os.path.join(os.path.dirname(output), "sl.interposer.lib")
                          if os.path.basename(dll) == "sl.interposer.dll" else None)
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
    for dll, source, interposer in FLOW_TAKEOVER_GAME_MODULES:
        game_object = os.path.join(obj_dir, "fakes", "startup", os.path.splitext(dll)[0] + ".o")
        parallel_compile(env, clang_exe, fake_cflags, [(os.path.join(fakes_dir, *source.split("/")), game_object)])
        game_module = os.path.join(FLOW_TEST_OUTPUT_DIR, dll)
        interposer_library = os.path.join(FLOW_TEST_OUTPUT_DIR, *os.path.dirname(interposer).split("/"),
                                          "sl.interposer.lib")
        game_flags = ["-shared", "-static", "-fuse-ld=lld"]
        append_windows_pdb_linker_flag(game_flags, game_module)
        run_cached_link(
            [clang_exe, game_object, interposer_library] + game_flags + ["-o", game_module],
            env, game_module, required_outputs=[game_module] + ([pdb_path_for_binary(game_module)] if IS_WINDOWS else []),
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


# Each scenario process runs on a private desktop of its own. A third-party overlay (RivaTuner, Steam, ...) reaches a
# process through a global window hook, and the system delivers such a hook only to the processes on the desktop of the
# thread that installed it. The scenario process keeps its Present path clean on a desktop nobody else hooks; the
# foreign-overlay report in the flow host stays as the backstop.
if IS_WINDOWS:
    import contextlib
    import msvcrt
    import tempfile
    import uuid
    from ctypes import wintypes

    _FLOW_DESKTOP_ALL_ACCESS = 0x01FF
    _FLOW_STARTF_USESTDHANDLES = 0x00000100
    _FLOW_WAIT_TIMEOUT = 0x00000102
    _FLOW_INFINITE = 0xFFFFFFFF
    _FLOW_UOI_NAME = 2
    # CreateProcess copies every inheritable handle of the caller: the scenario's three standard handles are made
    # inheritable only while one CreateProcess call copies them, so a parallel scenario never inherits them.
    _flow_spawn_lock = threading.Lock()

    class _FlowStartupInfo(ctypes.Structure):
        _fields_ = [
            ("cb", wintypes.DWORD), ("lpReserved", wintypes.LPWSTR), ("lpDesktop", wintypes.LPWSTR),
            ("lpTitle", wintypes.LPWSTR), ("dwX", wintypes.DWORD), ("dwY", wintypes.DWORD),
            ("dwXSize", wintypes.DWORD), ("dwYSize", wintypes.DWORD), ("dwXCountChars", wintypes.DWORD),
            ("dwYCountChars", wintypes.DWORD), ("dwFillAttribute", wintypes.DWORD), ("dwFlags", wintypes.DWORD),
            ("wShowWindow", wintypes.WORD), ("cbReserved2", wintypes.WORD), ("lpReserved2", ctypes.c_void_p),
            ("hStdInput", wintypes.HANDLE), ("hStdOutput", wintypes.HANDLE), ("hStdError", wintypes.HANDLE),
        ]

    class _FlowProcessInformation(ctypes.Structure):
        _fields_ = [
            ("hProcess", wintypes.HANDLE), ("hThread", wintypes.HANDLE),
            ("dwProcessId", wintypes.DWORD), ("dwThreadId", wintypes.DWORD),
        ]

    _flow_user32 = ctypes.WinDLL("user32", use_last_error=True)
    _flow_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _flow_user32.CreateDesktopW.argtypes = [wintypes.LPCWSTR, wintypes.LPCWSTR, ctypes.c_void_p, wintypes.DWORD,
                                            wintypes.DWORD, ctypes.c_void_p]
    _flow_user32.CreateDesktopW.restype = wintypes.HANDLE
    _flow_user32.CloseDesktop.argtypes = [wintypes.HANDLE]
    _flow_user32.GetProcessWindowStation.restype = wintypes.HANDLE
    _flow_user32.GetUserObjectInformationW.argtypes = [wintypes.HANDLE, ctypes.c_int, ctypes.c_void_p,
                                                       wintypes.DWORD, ctypes.c_void_p]
    _flow_kernel32.CreateProcessW.argtypes = [wintypes.LPCWSTR, wintypes.LPWSTR, ctypes.c_void_p, ctypes.c_void_p,
                                              wintypes.BOOL, wintypes.DWORD, ctypes.c_void_p, wintypes.LPCWSTR,
                                              ctypes.POINTER(_FlowStartupInfo),
                                              ctypes.POINTER(_FlowProcessInformation)]
    _flow_kernel32.CreateProcessW.restype = wintypes.BOOL
    _flow_kernel32.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    _flow_kernel32.WaitForSingleObject.restype = wintypes.DWORD
    _flow_kernel32.TerminateProcess.argtypes = [wintypes.HANDLE, wintypes.UINT]
    _flow_kernel32.GetExitCodeProcess.argtypes = [wintypes.HANDLE, ctypes.POINTER(wintypes.DWORD)]
    _flow_kernel32.CloseHandle.argtypes = [wintypes.HANDLE]

    def _flow_window_station_name() -> str:
        name = ctypes.create_unicode_buffer(256)
        station = _flow_user32.GetProcessWindowStation()
        if station and _flow_user32.GetUserObjectInformationW(station, _FLOW_UOI_NAME, name, ctypes.sizeof(name), None):
            return name.value
        return "WinSta0"

    def _flow_create_desktop() -> Tuple[Optional[int], Optional[str]]:
        """A new desktop of this process's window station with a unique name; (None, None) when it cannot be made."""
        name = "CE_FG_" + uuid.uuid4().hex
        desktop = _flow_user32.CreateDesktopW(name, None, None, 0, _FLOW_DESKTOP_ALL_ACCESS, None)
        if not desktop:
            log(f"[flow] private desktop not created ({ctypes.WinError(ctypes.get_last_error())}); "
                "the scenario runs on the current desktop")
            return None, None
        return desktop, f"{_flow_window_station_name()}\\{name}"

    def _flow_read_output(stream) -> str:
        stream.seek(0)
        return stream.read().decode("utf-8", "replace").replace("\r\n", "\n")

    def _flow_run_on_desktop(command: List[str], cwd: str, timeout: float) -> Tuple[int, str]:
        desktop, desktop_path = _flow_create_desktop()
        try:
            with contextlib.ExitStack() as files:
                stdin = files.enter_context(open(os.devnull, "rb"))
                stdout = files.enter_context(tempfile.TemporaryFile())
                stderr = files.enter_context(tempfile.TemporaryFile())
                handles = [msvcrt.get_osfhandle(stream.fileno()) for stream in (stdin, stdout, stderr)]
                startup = _FlowStartupInfo()
                startup.cb = ctypes.sizeof(startup)
                startup.lpDesktop = desktop_path
                startup.dwFlags = _FLOW_STARTF_USESTDHANDLES
                startup.hStdInput, startup.hStdOutput, startup.hStdError = handles
                info = _FlowProcessInformation()
                command_line = ctypes.create_unicode_buffer(subprocess.list2cmdline(command))
                with _flow_spawn_lock:
                    for handle in handles:
                        os.set_handle_inheritable(handle, True)
                    try:
                        created = _flow_kernel32.CreateProcessW(None, command_line, None, None, True, 0, None, cwd,
                                                                ctypes.byref(startup), ctypes.byref(info))
                        error = ctypes.get_last_error()
                    finally:
                        for handle in handles:
                            os.set_handle_inheritable(handle, False)
                if not created:
                    raise ctypes.WinError(error)
                try:
                    timed_out = _flow_kernel32.WaitForSingleObject(
                        info.hProcess, int(timeout * 1000)) == _FLOW_WAIT_TIMEOUT
                    if timed_out:
                        _flow_kernel32.TerminateProcess(info.hProcess, 1)
                        _flow_kernel32.WaitForSingleObject(info.hProcess, _FLOW_INFINITE)
                    exit_code = wintypes.DWORD()
                    _flow_kernel32.GetExitCodeProcess(info.hProcess, ctypes.byref(exit_code))
                finally:
                    _flow_kernel32.CloseHandle(info.hThread)
                    _flow_kernel32.CloseHandle(info.hProcess)
                output = _flow_read_output(stdout) + _flow_read_output(stderr)
        finally:
            # The desktop goes once the scenario process has exited; its own handle is the only one held here.
            if desktop:
                _flow_user32.CloseDesktop(desktop)
        if timed_out:
            return -1, f"timed out after {timeout} s\n{output}"
        return ctypes.c_int32(exit_code.value).value, output


def run_flow_scenario_process(command: List[str], cwd: str, timeout: float) -> Tuple[int, str]:
    """Runs one scenario process on its own desktop; returns its exit code (-1 after a timeout) and its output."""
    if IS_WINDOWS:
        return _flow_run_on_desktop(command, cwd, timeout)
    try:
        result = subprocess.run(command, capture_output=True, text=True, encoding="utf-8", errors="replace",
                                timeout=timeout, cwd=cwd)
    except subprocess.TimeoutExpired as expired:
        partial = expired.stdout.decode("utf-8", "replace") if isinstance(expired.stdout, bytes) else ""
        return -1, f"timed out after {timeout} s\n{partial}"
    return result.returncode, result.stdout + result.stderr


def preserve_failed_flow_logs(logs: str, destination: Optional[str]) -> Optional[str]:
    """Copies a failed scenario's log folder into the gate's verification run folder; returns the copy.

    The next run of the scenario recreates build/flow_tests/logs/<name>, so an intermittent failure's hook and
    debug-layer logs were lost to the first rerun (FlowFSR, 2026-10-17)."""
    if not destination or not os.path.isdir(logs):
        return None
    try:
        shutil.copytree(logs, destination, dirs_exist_ok=True)
    except OSError as error:
        log(f"Could not keep the failed flow scenario's logs ({error})")
        return None
    return destination


def load_flow_durations(path: str) -> Dict[str, float]:
    """The scenario wall times a previous run recorded; empty when there are none or the file is unreadable."""
    try:
        with open(path, "r", encoding="utf-8") as stream:
            data = json.load(stream)
    except (OSError, ValueError):
        return {}
    if not isinstance(data, dict):
        return {}
    return {name: float(seconds) for name, seconds in data.items()
            if isinstance(name, str) and isinstance(seconds, (int, float))}


def order_flow_tests(tests: List[str], durations: Dict[str, float]) -> List[str]:
    """Longest first, so the slowest scenarios do not start last and stretch the run alone. A scenario without a
    recorded time (new, or renamed) counts as the longest; equal times keep the listed order."""
    position = {name: index for index, name in enumerate(tests)}
    return sorted(tests, key=lambda name: (-durations.get(name, float("inf")), position[name]))


def save_flow_durations(path: str, durations: Dict[str, float]) -> None:
    """Merges this run's times into the recorded ones (a filtered run keeps the others)."""
    recorded = load_flow_durations(path)
    recorded.update(durations)
    try:
        write_text_atomic(path, json.dumps(recorded, indent=1, sort_keys=True) + "\n")
    except OSError as error:
        log(f"Could not record the FG flow scenario times ({error})")


def flow_test_input_key() -> str:
    """What a flow run's result depends on, as one digest: every source and header the flow binaries compile (their
    depfiles), the build scripts that set their flags and run them, the compiler, the config the game loads and the
    Windows WARP/D3D12 modules it renders and validates with. The build version header is left out: only
    build_identity.cpp reads it, and a version string changes no scenario."""
    inputs = depfile_dependencies(os.path.join(OBJ_DIR, "x64-flow"),
                                  excluded=[os.path.join(PROJECT_ROOT, "common", "build_version.h")])
    inputs += glob.glob(os.path.join(PROJECT_ROOT, "tools", "build", "*.py"))
    inputs += [os.path.join(PROJECT_ROOT, "build.py"), os.path.join(FLOW_TEST_OUTPUT_DIR, "config.ini"),
               get_compiler_exe("x64") or ""]
    system = os.path.join(os.environ.get("SystemRoot", r"C:\Windows"), "System32")
    inputs += [os.path.join(system, name) for name in FLOW_TEST_SYSTEM_MODULES]
    return inputs_digest(path for path in inputs if path)


def flow_tests_passed_with(key: str) -> Optional[str]:
    """When the last unfiltered run with exactly these inputs passed; None when it did not."""
    try:
        with open(FLOW_TEST_PASS_FILE, "r", encoding="utf-8") as stream:
            record = json.load(stream)
    except (OSError, ValueError):
        return None
    if isinstance(record, dict) and record.get("key") == key and isinstance(record.get("passed_at"), str):
        return record["passed_at"]
    return None


def record_flow_tests_passed(key: str, scenarios: int) -> None:
    record = {"key": key, "passed_at": time.strftime("%Y-%m-%d %H:%M:%S"), "scenarios": scenarios}
    try:
        write_text_atomic(FLOW_TEST_PASS_FILE, json.dumps(record, indent=1) + "\n")
    except OSError as error:
        log(f"Could not record the passing FG flow run ({error})")


def flow_test_rerun_required(gtest_filter: Optional[str]) -> bool:
    """A filtered run is asked for by name, a release verification runs everything, and the flag forces a run."""
    return bool(gtest_filter) or FLOW_TEST_RERUN_FLAG in sys.argv or "--verify" in sys.argv


def run_flow_tests(env, flow_exe: Optional[str], gtest_filter: Optional[str] = None) -> bool:
    """Runs every flow scenario in its own process; returns False on any failure. An unfiltered run whose inputs
    match the last passing one is skipped: the same binaries on the same system give the same result."""
    if not flow_exe or not os.path.exists(flow_exe):
        return True
    key = flow_test_input_key()
    passed_at = None if flow_test_rerun_required(gtest_filter) else flow_tests_passed_with(key)
    if passed_at:
        log(f"=== FG flow tests skipped: inputs unchanged since the passing run of {passed_at} "
            f"(key {key[:12]}; {FLOW_TEST_RERUN_FLAG} runs them anyway) ===")
        record_verification_step("flow_tests", "skipped",
                                 details={"reason": "inputs_unchanged", "key": key, "passed_at": passed_at})
        return True
    tests = list_flow_tests(flow_exe, gtest_filter)
    if not tests:
        if gtest_filter:
            log(f"No FG flow test matches the filter {gtest_filter}")
        return True
    workers = min(FLOW_TEST_PARALLEL_PROCESSES, len(tests))
    log(f"=== Running FG flow tests ({len(tests)} scenario process(es), {workers} at a time) ===")
    tests = order_flow_tests(tests, load_flow_durations(FLOW_TEST_DURATIONS_FILE))
    start = time.time()

    def run_one(name: str):
        logs = os.path.join(FLOW_TEST_OUTPUT_DIR, "logs", name)
        shutil.rmtree(logs, ignore_errors=True)
        started = time.time()
        returncode, output = run_flow_scenario_process(
            [flow_exe, f"--gtest_filter={name}"], FLOW_TEST_OUTPUT_DIR, FLOW_TEST_TIMEOUT_SECONDS)
        return name, returncode, output, time.time() - started

    failures = []
    durations: Dict[str, float] = {}
    with ThreadPoolExecutor(max_workers=workers) as executor:
        for name, returncode, output, seconds in executor.map(run_one, tests):
            durations[name] = round(seconds, 2)
            if returncode != 0:
                failures.append(name)
                logs = os.path.join(FLOW_TEST_OUTPUT_DIR, "logs", name)
                kept = preserve_failed_flow_logs(logs, verification_artifact_path(os.path.join("flow_logs", name)))
                log(f"[flow:{name}] FAILED (exit code {returncode}); hook log: {kept or logs}")
                log_unit_test_output_tail(f"flow:{name}", output)
    elapsed = time.time() - start
    save_flow_durations(FLOW_TEST_DURATIONS_FILE, durations)
    slowest = sorted(durations.items(), key=lambda item: item[1], reverse=True)[:FLOW_TEST_SLOWEST_REPORTED]
    busy = sum(durations.values())
    log(f"FG flow scenario time: {busy:.0f}s in {elapsed:.0f}s wall ({workers} at a time); slowest: "
        + ", ".join(f"{name} {seconds:.1f}s" for name, seconds in slowest))
    record_verification_step(
        "flow_tests",
        "failed" if failures else "passed",
        duration_seconds=elapsed,
        details={"scenarios": len(tests), "failed": failures, "gtest_filter": gtest_filter, "workers": workers,
                 "scenario_seconds_total": round(busy, 1), "slowest": dict(slowest)},
    )
    if failures:
        log(f"=== FG flow tests FAILED ({len(failures)} of {len(tests)}) ===")
        return False
    if not gtest_filter:
        record_flow_tests_passed(key, len(tests))
    log(f"=== FG flow tests passed ({len(tests)} scenarios, {elapsed:.1f}s) ===")
    return True
