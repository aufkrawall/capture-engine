# compile_cengine_dll: the runtime and its C ABI as cengine.dll (llm-wiki/library/06, milestone M7).
# runtime/** is compiled for the library (CE_BUILDING_LIBRARY); common/** and the role-host units the
# runtime shares (DR-15) reuse the product's x64 objects. runtime/api/cengine.def fixes the export
# table, and the built DLL is checked against include/cengine/cengine.h before it is placed.

CENGINE_DEF_FILE = os.path.join(PROJECT_ROOT, "runtime", "api", "cengine.def")
CENGINE_SDK_HEADERS = ("cengine.h", "cengine.hpp")

# Role-host code the runtime calls (DR-15: compiled into both captureengine.exe and cengine.dll). The
# entry points are the M7 exceptions in tools/module_boundaries.json (InjectionManager, TakeScreenshot,
# startup/elevation and PawnIO setup); the rest is their link closure. A unit missing here fails the
# DLL link with an undefined symbol, never silently.
CENGINE_ROLE_HOST_SOURCES = (
    "captureengine/app/mediaengine_loader.cpp",
    "captureengine/elevation/elevation_client.cpp",
    "captureengine/elevation/elevation_integration.cpp",
    "captureengine/elevation/elevation_removal.cpp",
    "captureengine/elevation/elevation_runtime.cpp",
    "captureengine/elevation/elevation_setup.cpp",
    "captureengine/elevation/startup_autostart.cpp",
    "captureengine/elevation/startup_control.cpp",
    "captureengine/elevation/startup_preferences.cpp",
    "captureengine/injection/inject_config.cpp",
    "captureengine/injection/inject_config_publication.cpp",
    "captureengine/injection/injection.cpp",
    "captureengine/injection/injection_inject.cpp",
    "captureengine/injection/injection_manager.cpp",
    "captureengine/injection/injection_ota_watchdog.cpp",
    "captureengine/injection/injection_process_exit.cpp",
    "captureengine/injection/injection_runtime_exclusion.cpp",
    "captureengine/injection/injection_security.cpp",
    "captureengine/injection/injection_startup_launch.cpp",
    "captureengine/injection/injection_wmi_events.cpp",
    "captureengine/injection/process_start_etw.cpp",
    "captureengine/injection/process_start_poll.cpp",
    "captureengine/media/dxgi_dup_capture.cpp",
    "captureengine/media/media_main_window.cpp",
    "captureengine/media/screenshot.cpp",
    "captureengine/media/screenshot_encoding.cpp",
    "captureengine/media/screenshot_hdr_encoding.cpp",
    "captureengine/media/wgc_capture.cpp",
    "captureengine/media/wgc_capture_frame.cpp",
    "captureengine/media/wgc_capture_frame_pump.cpp",
    "captureengine/media/wgc_capture_helpers.cpp",
    "captureengine/media/wgc_capture_impl_format.cpp",
    "captureengine/media/wgc_capture_impl_gpu_timing.cpp",
    "captureengine/media/wgc_capture_impl_pool.cpp",
    "captureengine/media/wgc_capture_init.cpp",
    "captureengine/media/wgc_capture_pool.cpp",
    "captureengine/media/wgc_capture_pool_budget.cpp",
    "captureengine/media/wgc_capture_queue.cpp",
    "captureengine/media/wgc_capture_raii_guards.cpp",
    "captureengine/media/wgc_capture_retained_yuv.cpp",
    "captureengine/media/wgc_capture_session.cpp",
    "captureengine/media/wgc_capture_state.cpp",
    "captureengine/sensors/pawnio_driver.cpp",
    "captureengine/sensors/pawnio_setup.cpp",
    "captureengine/sensors/pawnio_setup_core.cpp",
    "captureengine/sensors/pawnio_workers.cpp",
)


def get_llvm_dlltool_exe() -> str:
    """Resolve the host-native llvm-dlltool that writes the MSVC import library."""
    if not IS_LINUX:
        return os.path.join(MSYS2_DIR, "clang64", "bin", "llvm-dlltool.exe")
    for executable in ["llvm-dlltool"] + [f"llvm-dlltool-{version}" for version in range(22, 13, -1)]:
        resolved = shutil.which(executable)
        if resolved:
            return resolved
    raise RuntimeError("llvm-dlltool is required for the cengine import library; install the 'llvm' package")


def cengine_object(object_root: str, source: str) -> str:
    return os.path.join(object_root, os.path.splitext(os.path.relpath(source, PROJECT_ROOT))[0] + ".o").replace(
        "\\", "/"
    )


# Run in a child interpreter: the DLL's static initializers must not run inside the build process.
CENGINE_LOAD_CHECK = r"""
import ctypes, pathlib, re, sys
header = pathlib.Path(sys.argv[2]).read_text(encoding="utf-8")
major = int(re.search(r"CE_API_VERSION_MAJOR (\d+)u", header).group(1))
minor = int(re.search(r"CE_API_VERSION_MINOR (\d+)u", header).group(1))
kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.LoadLibraryW.restype = ctypes.c_void_p
kernel32.FreeLibrary.argtypes = [ctypes.c_void_p]
module = kernel32.LoadLibraryW(sys.argv[1])
if not module:
    sys.exit(f"cengine.dll does not load (error {ctypes.get_last_error()})")
dll = ctypes.WinDLL(sys.argv[1], handle=module)
dll.ce_api_version.restype = ctypes.c_uint32
dll.ce_version_string.restype = ctypes.c_char_p
version = dll.ce_api_version()
if version != (major << 16 | minor) or not dll.ce_version_string():
    sys.exit(f"cengine.dll reports API version {version:#x}, the header declares {major}.{minor}")
if not kernel32.FreeLibrary(module):
    sys.exit("cengine.dll does not unload")
"""


def compile_cengine_dll(env, clang_exe, cflags, ffmpeg_cflags, link_flags, common_objs) -> str:
    """Build cengine.dll plus its SDK files (headers, .def, import libraries) under BIN_DIR/sdk.

    `link_flags` are the product executable's library and LTO/sanitizer flags, so the runtime links
    against exactly what it links against inside captureengine.exe.
    """
    log("Compiling cengine.dll x64...")
    object_root = os.path.join(OBJ_DIR, "cengine_x64")
    runtime_pairs = [(source, cengine_object(object_root, source)) for source in module_sources("runtime")]
    # The executable's runtime objects stay the compile_commands.json entries clangd and clang-tidy read.
    compile_commands_snapshot = list(COMPILE_COMMANDS)
    try:
        parallel_compile(env, clang_exe, cflags + ffmpeg_cflags + ["-DCE_BUILDING_LIBRARY"], runtime_pairs)
    finally:
        COMPILE_COMMANDS[:] = compile_commands_snapshot
    product_root = os.path.join(OBJ_DIR, "x64")
    role_host_objs = [
        cengine_object(product_root, os.path.join(PROJECT_ROOT, source)) for source in CENGINE_ROLE_HOST_SOURCES
    ]

    sdk_lib_dir = os.path.join(BIN_DIR, "sdk", "lib")
    os.makedirs(sdk_lib_dir, exist_ok=True)
    dll = os.path.join(BIN_DIR, "cengine.dll")
    linked_dll = os.path.join(object_root, "cengine.dll")
    temp_implib = os.path.join(object_root, "libcengine.dll.a")
    ldflags: List[str] = ["-shared", "-static", "-static-libgcc", "-static-libstdc++"]
    ldflags.extend(LD_OPT_FLAGS)
    ldflags.extend(get_x64_linker_flags(clang_exe))
    # The .def is the export list; nothing else may be exported automatically.
    ldflags.extend(["-Wl,--exclude-all-symbols", f"-Wl,--out-implib,{temp_implib}"])
    ldflags.extend(link_flags)
    # -mwindows adds GDI for the executable; the desktop overlay's GDI calls need it named here.
    ldflags.append("-lgdi32")
    # Named for the placed DLL: the image's debug record carries the bare PDB name.
    append_windows_pdb_linker_flag(ldflags, dll)
    log("Linking cengine.dll x64...")
    objects = [obj for _, obj in runtime_pairs] + role_host_objs + common_objs
    command = [clang_exe] + objects + [CENGINE_DEF_FILE] + ldflags + ["-o", linked_dll]
    required = [linked_dll, temp_implib] + ([pdb_path_for_binary(dll)] if IS_WINDOWS else [])
    run_cached_link(
        command,
        env,
        linked_dll,
        required_outputs=required,
        execute_command=prepare_command_with_response_file(command, os.path.join(object_root, "cengine_link.rsp")),
    )

    # The header is the ABI: a missing export fails GetProcAddress at the client, an extra one widens it.
    run_command(
        [
            sys.executable,
            os.path.join(PROJECT_ROOT, "tools", "check_cengine_exports.py"),
            "--def",
            CENGINE_DEF_FILE,
            "--dll",
            linked_dll,
            "--tool",
            get_llvm_readobj_exe(),
        ],
        env=env,
        cwd=PROJECT_ROOT,
    )
    # Loadable with only its import closure, reports the header's API version and unloads again.
    if IS_WINDOWS:
        header = os.path.join(PROJECT_ROOT, "include", "cengine", "cengine.h")
        run_command([sys.executable, "-I", "-c", CENGINE_LOAD_CHECK, linked_dll, header], env=env)
    if not safe_copy_file(linked_dll, dll):
        log("ERROR: Failed to place cengine.dll (destination may be locked)")
        sys.exit(1)
    record_verification_artifact("cengine_dll", dll)

    # SDK: the shipped headers, the export list and import libraries for clang/MinGW and MSVC linkers.
    sdk_include_dir = os.path.join(BIN_DIR, "sdk", "include", "cengine")
    os.makedirs(sdk_include_dir, exist_ok=True)
    for header in CENGINE_SDK_HEADERS:
        source = os.path.join(PROJECT_ROOT, "include", "cengine", header)
        if not safe_copy_file(source, os.path.join(sdk_include_dir, header)):
            raise RuntimeError(f"Cannot stage SDK header {header}")
    if not safe_copy_file(CENGINE_DEF_FILE, os.path.join(sdk_lib_dir, "cengine.def")):
        raise RuntimeError("Cannot stage cengine.def")
    if not safe_copy_file(temp_implib, os.path.join(sdk_lib_dir, "libcengine.dll.a")):
        raise RuntimeError("Cannot stage libcengine.dll.a")
    run_command(
        [
            get_llvm_dlltool_exe(),
            "-m",
            "i386:x86-64",
            "-d",
            CENGINE_DEF_FILE,
            "-D",
            "cengine.dll",
            "-l",
            os.path.join(sdk_lib_dir, "cengine.lib"),
        ],
        env=env,
    )
    log("Built cengine.dll and its SDK files (headers, cengine.def, libcengine.dll.a, cengine.lib)")
    return dll
