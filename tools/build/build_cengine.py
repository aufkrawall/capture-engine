# compile_cengine_dll: the runtime and its C ABI as cengine.dll (llm-wiki/library/06, milestone M7).
# runtime/** is compiled for the library (CE_BUILDING_LIBRARY); common/** and the role-host units the
# runtime shares (DR-15) reuse the product's x64 objects. runtime/api/cengine.def fixes the export
# table, and the built DLL is checked against include/cengine/cengine.h before it is placed.

CENGINE_DEF_FILE = os.path.join(PROJECT_ROOT, "runtime", "api", "cengine.def")
CENGINE_SDK_HEADERS = ("cengine.h", "cengine.hpp")

# Role-host code the runtime calls (DR-15: compiled into both captureengine.exe and cengine.dll). The
# entry points are the M7 exceptions in tools/module_boundaries.json (InjectionManager, TakeScreenshot,
# startup/elevation and PawnIO setup) and, since phase 2, the role entries the thread host runs
# (captureengine/roles/role_entries.h: the inject, media and logger roles, without their process hosts);
# the rest is their link closure. A unit missing here fails the DLL link with an undefined symbol, never
# silently.
CENGINE_ROLE_HOST_SOURCES = (
    "captureengine/app/mediaengine_loader.cpp",
    "captureengine/app/windows_gpu_scheduling.cpp",
    "captureengine/diagnostics/logger_ring_source.cpp",
    "captureengine/diagnostics/logger_service.cpp",
    "captureengine/elevation/elevation_client.cpp",
    "captureengine/elevation/elevation_integration.cpp",
    "captureengine/elevation/elevation_removal.cpp",
    "captureengine/elevation/elevation_runtime.cpp",
    "captureengine/elevation/elevation_setup.cpp",
    "captureengine/elevation/startup_autostart.cpp",
    "captureengine/elevation/startup_control.cpp",
    "captureengine/elevation/startup_preferences.cpp",
    "captureengine/injection/inject_config_publication.cpp",
    "captureengine/injection/inject_config.cpp",
    "captureengine/injection/inject_lifecycle.cpp",
    "captureengine/injection/inject_main.cpp",
    "captureengine/injection/injection_inject.cpp",
    "captureengine/injection/injection_manager.cpp",
    "captureengine/injection/injection_ota_watchdog.cpp",
    "captureengine/injection/injection_process_exit.cpp",
    "captureengine/injection/injection_runtime_exclusion.cpp",
    "captureengine/injection/injection_security.cpp",
    "captureengine/injection/injection_startup_launch.cpp",
    "captureengine/injection/injection_wmi_events.cpp",
    "captureengine/injection/injection.cpp",
    "captureengine/injection/process_start_etw.cpp",
    "captureengine/injection/process_start_poll.cpp",
    "captureengine/media/dxgi_dup_capture.cpp",
    "captureengine/media/frame_submission_d3d11.cpp",
    "captureengine/media/frame_submission_inject.cpp",
    "captureengine/media/inject_source_read.cpp",
    "captureengine/media/media_gpu_priority_claims.cpp",
    "captureengine/media/media_main_encoder_00_session_diagnostics.cpp",
    "captureengine/media/media_main_encoder_00_session_shutdown.cpp",
    "captureengine/media/media_main_encoder_00_session.cpp",
    "captureengine/media/media_main_encoder_01_loop_start.cpp",
    "captureengine/media/media_main_encoder_04_loop_wgc_target_pop.cpp",
    "captureengine/media/media_main_encoder_04_loop_wgc_target_stats.cpp",
    "captureengine/media/media_main_encoder_04_loop_wgc_target.cpp",
    "captureengine/media/media_main_encoder_05a_loop_wgc_select.cpp",
    "captureengine/media/media_main_encoder_05b_loop_wgc_select_uniform.cpp",
    "captureengine/media/media_main_encoder_05c_loop_inject_select.cpp",
    "captureengine/media/media_main_encoder_05d_loop_inject_horizon.cpp",
    "captureengine/media/media_main_encoder_06_loop_startup_inject.cpp",
    "captureengine/media/media_main_encoder_06_loop_startup_warmup_sync.cpp",
    "captureengine/media/media_main_encoder_06_loop_startup.cpp",
    "captureengine/media/media_main_encoder_07_loop_emit.cpp",
    "captureengine/media/media_main_encoder_08_loop_encode.cpp",
    "captureengine/media/media_main_encoder_09_loop_health.cpp",
    "captureengine/media/media_main_encoder_10_mux_budget.cpp",
    "captureengine/media/media_main_encoder_content_delay.cpp",
    "captureengine/media/media_main_encoder_inject_display_timing.cpp",
    "captureengine/media/media_main_priority.cpp",
    "captureengine/media/media_main_recording.cpp",
    "captureengine/media/media_main_start_loop.cpp",
    "captureengine/media/media_main_start_shutdown.cpp",
    "captureengine/media/media_main_start_targets.cpp",
    "captureengine/media/media_main_start.cpp",
    "captureengine/media/media_main_threads_inject.cpp",
    "captureengine/media/media_main_threads_wgc.cpp",
    "captureengine/media/media_main_wgc.cpp",
    "captureengine/media/media_main_window.cpp",
    "captureengine/media/media_main.cpp",
    "captureengine/media/media_role_context.cpp",
    "captureengine/media/media_role_entry.cpp",
    "captureengine/media/media_session_state.cpp",
    "captureengine/media/screen_grab_privacy_runtime.cpp",
    "captureengine/media/screenshot_encoding.cpp",
    "captureengine/media/screenshot_hdr_encoding.cpp",
    "captureengine/media/screenshot.cpp",
    "captureengine/media/status_overlay_sync.cpp",
    "captureengine/media/wgc_capture_duplication.cpp",
    "captureengine/media/wgc_capture_frame_pump.cpp",
    "captureengine/media/wgc_capture_frame.cpp",
    "captureengine/media/wgc_capture_helpers.cpp",
    "captureengine/media/wgc_capture_impl_format.cpp",
    "captureengine/media/wgc_capture_impl_gpu_timing.cpp",
    "captureengine/media/wgc_capture_impl_pool.cpp",
    "captureengine/media/wgc_capture_init.cpp",
    "captureengine/media/wgc_capture_pool_budget.cpp",
    "captureengine/media/wgc_capture_pool.cpp",
    "captureengine/media/wgc_capture_queue.cpp",
    "captureengine/media/wgc_capture_raii_guards.cpp",
    "captureengine/media/wgc_capture_retained_yuv.cpp",
    "captureengine/media/wgc_capture_session.cpp",
    "captureengine/media/wgc_capture_state.cpp",
    "captureengine/media/wgc_capture_stats.cpp",
    "captureengine/media/wgc_capture.cpp",
    "captureengine/sensors/pawnio_driver.cpp",
    "captureengine/sensors/pawnio_setup_core.cpp",
    "captureengine/sensors/pawnio_setup.cpp",
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
    """Build cengine.dll plus its SDK files (headers, .def, import libraries) under BIN_DIR/sdk; returns the
    clang/MinGW import library captureengine.exe links.

    `link_flags` are the product executable's library and LTO/sanitizer flags, so the runtime links
    against exactly what it links against inside captureengine.exe.
    """
    log("Compiling cengine.dll x64...")
    object_root = os.path.join(OBJ_DIR, "cengine_x64")
    runtime_pairs = [(source, cengine_object(object_root, source)) for source in module_sources("runtime")]
    # The product's only compile of runtime/ (the executable reaches the runtime through this DLL), so these are the
    # compile_commands.json entries clangd and clang-tidy read for it.
    parallel_compile(env, clang_exe, cflags + ffmpeg_cflags + ["-DCE_BUILDING_LIBRARY"], runtime_pairs)
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
    if not safe_copy_file(os.path.join(PROJECT_ROOT, "sdk", "README.md"), os.path.join(BIN_DIR, "sdk", "README.md")):
        raise RuntimeError("Cannot stage the SDK readme")
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
    stage_cmake_package(os.path.join(BIN_DIR, "sdk"))
    build_headless_sample(env, clang_exe, os.path.join(BIN_DIR, "sdk"))
    log("Built cengine.dll and its SDK files (headers, cengine.def, import libraries, CMake package, sample)")
    return temp_implib


# cengineConfigVersion.cmake: generated from the header so the package version cannot drift from CE_API_VERSION.
# Same major and an equal or older requested minor are compatible; x64 only.
CENGINE_CMAKE_VERSION_TEMPLATE = """# Generated by the CaptureEngine build from include/cengine/cengine.h (CE_API_VERSION).
set(PACKAGE_VERSION "{major}.{minor}")
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(PACKAGE_VERSION_UNSUITABLE TRUE)
elseif(PACKAGE_FIND_VERSION_MAJOR AND NOT PACKAGE_FIND_VERSION_MAJOR EQUAL {major})
    set(PACKAGE_VERSION_COMPATIBLE FALSE)
elseif(PACKAGE_FIND_VERSION VERSION_GREATER PACKAGE_VERSION)
    set(PACKAGE_VERSION_COMPATIBLE FALSE)
else()
    set(PACKAGE_VERSION_COMPATIBLE TRUE)
    if(PACKAGE_FIND_VERSION VERSION_EQUAL PACKAGE_VERSION)
        set(PACKAGE_VERSION_EXACT TRUE)
    endif()
endif()
"""


def cengine_api_version(header_text: str) -> tuple:
    major = re.search(r"#define CE_API_VERSION_MAJOR (\d+)u", header_text)
    minor = re.search(r"#define CE_API_VERSION_MINOR (\d+)u", header_text)
    if not major or not minor:
        raise RuntimeError("cengine.h declares no CE_API_VERSION_MAJOR/MINOR")
    return int(major.group(1)), int(minor.group(1))


def stage_cmake_package(sdk_root) -> None:
    """Stage sdk/lib/cmake/cengine: the package config and its version file (from the header)."""
    package_dir = os.path.join(sdk_root, "lib", "cmake", "cengine")
    os.makedirs(package_dir, exist_ok=True)
    if not safe_copy_file(os.path.join(PROJECT_ROOT, "sdk", "cmake", "cengineConfig.cmake"),
                          os.path.join(package_dir, "cengineConfig.cmake")):
        raise RuntimeError("Cannot stage cengineConfig.cmake")
    with open(os.path.join(PROJECT_ROOT, "include", "cengine", "cengine.h"), encoding="utf-8") as header:
        major, minor = cengine_api_version(header.read())
    with open(os.path.join(package_dir, "cengineConfigVersion.cmake"), "w", encoding="utf-8", newline="\n") as out:
        out.write(CENGINE_CMAKE_VERSION_TEMPLATE.format(major=major, minor=minor))


def is_frontend_source(source: str) -> bool:
    return os.path.relpath(source, PROJECT_ROOT).replace("\\", "/").startswith("frontend/")


# What a frontend object may not reach (M7 acceptance): the logging and the internals of the role host image, which
# in controller mode stay uninitialized because the app talks to the runtime in cengine.dll only.
FRONTEND_FORBIDDEN_SYMBOLS = ("Log_", "ce::engine::", "ce::runtime::", "ce::capi::")


def verify_frontend_uses_only_the_c_api(frontend_objs) -> None:
    """Fail when a frontend object references the image's logging or runtime internals instead of the C API."""
    if not frontend_objs:
        raise RuntimeError("No frontend objects to check")
    nm = os.path.join(os.path.dirname(get_llvm_readobj_exe()), "llvm-nm.exe" if IS_WINDOWS else "llvm-nm")
    offenders = []
    for obj in frontend_objs:
        result = subprocess.run([nm, "--undefined-only", "-C", "-j", obj], capture_output=True, text=True)
        if result.returncode != 0:
            raise RuntimeError(f"llvm-nm failed on {obj}: {result.stderr.strip()[:200]}")
        for symbol in result.stdout.splitlines():
            if any(symbol.startswith(prefix) for prefix in FRONTEND_FORBIDDEN_SYMBOLS):
                offenders.append(f"{os.path.relpath(obj, OBJ_DIR)}: {symbol}")
    if offenders:
        raise RuntimeError("frontend objects reach past the C API:\n  " + "\n  ".join(offenders[:20]))
    log(f"Frontend objects reference only the C API ({len(frontend_objs)} objects checked)")


def build_headless_sample(env, clang_exe, sdk_root) -> None:
    """Stage samples/headless into the SDK and build it as a client would: CMake, find_package(cengine) (M8).

    Only the staged SDK is visible to this build (CMAKE_PREFIX_PATH): a declaration a client needs that the
    shipped headers lack, an export the import library misses, or a broken package config fails here. The
    executable stays in the build tree (build/samples); it is not shipped.
    """
    staged_dir = os.path.join(sdk_root, "samples", "headless")
    os.makedirs(staged_dir, exist_ok=True)
    for name in ("main.c", "CMakeLists.txt"):
        if not safe_copy_file(os.path.join(PROJECT_ROOT, "samples", "headless", name), os.path.join(staged_dir, name)):
            raise RuntimeError(f"Cannot stage the headless sample's {name}")
    if not IS_WINDOWS:
        log("Skipping the headless sample build on a non-Windows host")
        return
    tools_dir = os.path.dirname(clang_exe)
    build_dir = os.path.join(BUILD_DIR, "samples", "headless-cmake")
    output_dir = os.path.join(BUILD_DIR, "samples")
    if os.path.isdir(build_dir) and not safe_remove_tree(build_dir):
        raise RuntimeError("Cannot clear the headless sample build directory")
    os.makedirs(build_dir, exist_ok=True)
    run_command(
        [
            os.path.join(tools_dir, "cmake.exe"),
            "-S",
            staged_dir,
            "-B",
            build_dir,
            "-G",
            "Ninja",
            f"-DCMAKE_MAKE_PROGRAM={os.path.join(tools_dir, 'ninja.exe')}",
            f"-DCMAKE_C_COMPILER={os.path.join(tools_dir, 'clang.exe')}",
            "-DCMAKE_C_FLAGS=-Werror",
            "-DCMAKE_BUILD_TYPE=Release",
            f"-DCMAKE_PREFIX_PATH={sdk_root}",
            f"-DCMAKE_RUNTIME_OUTPUT_DIRECTORY={output_dir}",
        ],
        env=env,
        cwd=build_dir,
    )
    run_command([os.path.join(tools_dir, "cmake.exe"), "--build", build_dir], env=env, cwd=build_dir)
    output = os.path.join(output_dir, "headless.exe")
    if not os.path.isfile(output):
        raise RuntimeError("The headless sample's CMake build produced no headless.exe")
    record_verification_artifact("cengine_headless_sample", output)
