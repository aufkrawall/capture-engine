

PACKAGED_X64_TEST_APPS = (
    "directdraw7_test",
    "dx6_test",
    "dx7_test",
    "dx8_test",
    "dx9_test",
    "dx9ex_test",
    "dx10_test",
    "dx11_test",
    "dx12_test",
    "dx12_av_sync_test",
    "dx12_fsr_fg_test",
    "dx12_dlss_fg_test",
    "dx12_fg_switch_test",
    "opengl_test",
    "opengl_legacy_test",
    "vulkan_test",
    "vulkan_fg_switch_test",
)
PACKAGED_X86_TEST_APPS = tuple(
    name
    for name in PACKAGED_X64_TEST_APPS
    if name not in {"dx12_fsr_fg_test", "dx12_dlss_fg_test", "dx12_fg_switch_test", "vulkan_fg_switch_test"}
)
CAPTURE_PACKAGE_EXCLUDED_DIRECTORIES = {"bak", "captures", "logs", "screenshots"}
CAPTURE_PACKAGE_EXCLUDED_SUFFIXES = (".csv", ".dmp", ".link-cache.json", ".log", ".tmp")
CAPTURE_PACKAGE_INCLUDED_DIRECTORIES = {"ffmpeg", "licenses", "plugins"}
CAPTURE_PACKAGE_ROOT_SUFFIXES = (".dll", ".exe", ".json", ".pdb")
# Everything CaptureEngine is entitled to ship from the plugins directory: its
# own setup notes plus the pinned LibreHardwareMonitor runtime files installed by
# tools/build/build_lhm_plugin.py. Anything else a user drops in there - the GUI
# executable, PDBs, storage/SMBus helpers - is still excluded.
CAPTURE_PACKAGE_PLUGIN_FILES = {
    "plugins/librehardwaremonitor/readme.txt",
    "plugins/librehardwaremonitor/pawnio_setup.exe",
    "plugins/librehardwaremonitor/librehardwaremonitorlib.dll",
    "plugins/librehardwaremonitor/system.memory.dll",
    "plugins/librehardwaremonitor/system.numerics.vectors.dll",
    "plugins/librehardwaremonitor/system.runtime.compilerservices.unsafe.dll",
}


def _validate_workspace_cleanup_target(path: str) -> str:
    target = os.path.abspath(path)
    workspace_temp = os.path.abspath(WORKSPACE_TEMP_DIR)
    try:
        inside_workspace_temp = os.path.commonpath([target, workspace_temp]) == workspace_temp
    except ValueError:
        inside_workspace_temp = False
    if not inside_workspace_temp or target == workspace_temp:
        raise RuntimeError(f"Refusing package cleanup outside the workspace temp directory: {target}")
    return target


def _reset_package_staging_directory(path: str) -> None:
    target = _validate_workspace_cleanup_target(path)
    if os.path.exists(target):
        shutil.rmtree(target)
    os.makedirs(target, exist_ok=True)


def _capture_package_file_allowed(relative_path: str) -> bool:
    normalized = relative_path.replace("\\", "/")
    parts = normalized.split("/")
    if any(part.lower() in CAPTURE_PACKAGE_EXCLUDED_DIRECTORIES for part in parts[:-1]):
        return False
    filename = parts[-1].lower()
    if filename in {"config.ini", "nul"} or ".old." in filename:
        return False
    if parts[0].lower() == "plugins":
        # Optional third-party files are user-supplied and must never leak into
        # a CaptureEngine archive assembled from a local installation.
        return normalized.lower() in CAPTURE_PACKAGE_PLUGIN_FILES
    if len(parts) == 1 and not filename.endswith(CAPTURE_PACKAGE_ROOT_SUFFIXES):
        return False
    return not filename.endswith(CAPTURE_PACKAGE_EXCLUDED_SUFFIXES)


def _stage_captureengine_package(source_root: str, destination_root: str, clean_config: str) -> List[str]:
    if not os.path.isfile(os.path.join(source_root, "captureengine.exe")):
        raise RuntimeError("Cannot package CaptureEngine: captureengine.exe is missing")
    copied: List[str] = []
    for current_root, directories, filenames in os.walk(source_root, topdown=True):
        relative_directory = os.path.relpath(current_root, source_root)
        kept_directories = []
        for directory in directories:
            source_directory = os.path.join(current_root, directory)
            if directory.lower() in CAPTURE_PACKAGE_EXCLUDED_DIRECTORIES:
                continue
            if relative_directory == "." and directory.lower() not in CAPTURE_PACKAGE_INCLUDED_DIRECTORIES:
                continue
            if os.path.islink(source_directory):
                raise RuntimeError(f"Refusing to package directory symlink: {source_directory}")
            kept_directories.append(directory)
        directories[:] = kept_directories

        for filename in filenames:
            relative = filename if relative_directory == "." else os.path.join(relative_directory, filename)
            if not _capture_package_file_allowed(relative):
                continue
            source = os.path.join(current_root, filename)
            if os.path.islink(source):
                raise RuntimeError(f"Refusing to package file symlink: {source}")
            destination = os.path.join(destination_root, relative)
            os.makedirs(os.path.dirname(destination), exist_ok=True)
            shutil.copy2(source, destination)
            copied.append(relative.replace("\\", "/"))

    if not os.path.isfile(clean_config):
        raise RuntimeError(f"Cannot package CaptureEngine: default config template is missing: {clean_config}")
    config_destination = os.path.join(destination_root, "config.ini")
    os.makedirs(destination_root, exist_ok=True)
    shutil.copy2(clean_config, config_destination)
    copied.append("config.ini")
    return sorted(copied)


def _stage_testapps_package(
    source_root: str, destination_root: str, runtime_note: str, testapp_config: str
) -> List[str]:
    copied: List[str] = []
    staged_x86 = False
    for relative_directory, app_names, required in (
        ("", PACKAGED_X64_TEST_APPS, True),
        ("x86", PACKAGED_X86_TEST_APPS, False),
    ):
        source_directory = os.path.join(source_root, relative_directory)
        destination_directory = os.path.join(destination_root, relative_directory)
        for app_name in app_names:
            executable_name = app_name + ".exe"
            executable = os.path.join(source_directory, executable_name)
            if not os.path.isfile(executable):
                if required:
                    raise RuntimeError(f"Cannot package test apps: required executable is missing: {executable}")
                continue
            os.makedirs(destination_directory, exist_ok=True)
            destination = os.path.join(destination_directory, executable_name)
            shutil.copy2(executable, destination)
            relative_executable = os.path.join(relative_directory, executable_name).replace("\\", "/")
            copied.append(relative_executable)
            if relative_directory:
                staged_x86 = True

            pdb = os.path.join(source_directory, app_name + ".pdb")
            if os.path.isfile(pdb):
                pdb_name = app_name + ".pdb"
                shutil.copy2(pdb, os.path.join(destination_directory, pdb_name))
                copied.append(os.path.join(relative_directory, pdb_name).replace("\\", "/"))
            elif IS_WINDOWS:
                raise RuntimeError(f"Cannot package test apps: required PDB is missing: {pdb}")

    if not os.path.isfile(runtime_note):
        raise RuntimeError(f"Cannot package test apps: runtime requirements note is missing: {runtime_note}")
    note_name = os.path.basename(runtime_note)
    shutil.copy2(runtime_note, os.path.join(destination_root, note_name))
    copied.append(note_name)
    if not os.path.isfile(testapp_config):
        raise RuntimeError(f"Cannot package test apps: default config template is missing: {testapp_config}")
    config_name = os.path.basename(testapp_config)
    config_destination = os.path.join(destination_root, config_name)
    shutil.copy2(testapp_config, config_destination)
    copied.append(config_name)
    if staged_x86:
        x86_config_destination = os.path.join(destination_root, "x86", config_name)
        os.makedirs(os.path.dirname(x86_config_destination), exist_ok=True)
        shutil.copy2(testapp_config, x86_config_destination)
        copied.append("x86/" + config_name)
    return sorted(copied)


def _get_cmake_archiver() -> str:
    candidates = [
        os.path.join(MSYS2_DIR, "clang64", "bin", "cmake.exe"),
        shutil.which("cmake"),
    ]
    for candidate in candidates:
        if candidate and os.path.isfile(candidate):
            return candidate
    raise RuntimeError("CMake is required to create the automatic .7z packages")


def _create_7z_archive(cmake_exe: str, staging_root: str, root_name: str, archive_path: str) -> List[str]:
    os.makedirs(os.path.dirname(archive_path), exist_ok=True)
    temporary_archive = archive_path + ".tmp"
    if os.path.exists(temporary_archive):
        os.remove(temporary_archive)
    try:
        run_command(
            [cmake_exe, "-E", "tar", "cf", temporary_archive, "--format=7zip", root_name],
            cwd=staging_root,
        )
        listing = run_command([cmake_exe, "-E", "tar", "tf", temporary_archive], cwd=staging_root)
        members = sorted(
            line.strip().replace("\\", "/")
            for line in listing.splitlines()
            if line.strip()
        )
        expected_prefix = root_name + "/"
        if not members or any(member != root_name and not member.startswith(expected_prefix) for member in members):
            raise RuntimeError(f"Archive {archive_path} contains an invalid root layout")
        os.replace(temporary_archive, archive_path)
        return members
    finally:
        if os.path.exists(temporary_archive):
            os.remove(temporary_archive)


def package_build_outputs() -> Tuple[str, ...]:
    """Create clean binary and corresponding-source archives after a successful product build."""
    package_start = time.time()
    staging_root = os.path.join(WORKSPACE_TEMP_DIR, "package-staging")
    _reset_package_staging_directory(staging_root)
    capture_stage = os.path.join(staging_root, "captureengine")
    testapps_stage = os.path.join(staging_root, "testapps")
    source_stage = os.path.join(staging_root, "ffmpeg-corresponding-source")
    try:
        capture_files = _stage_captureengine_package(
            CAPTURE_BIN_DIR,
            capture_stage,
            os.path.join(PROJECT_ROOT, "captureengine", "config.ini.template"),
        )
        testapp_files = _stage_testapps_package(
            TESTAPP_BIN_DIR, testapps_stage, TESTAPP_RUNTIME_NOTE, TESTAPP_CONFIG_TEMPLATE
        )
        source_files = []
        if IS_WINDOWS:
            source_files = _stage_ffmpeg_corresponding_source(
                os.path.join(PROJECT_ROOT, "ffmpeg_build", "repos", "ffmpeg"),
                os.path.join(PROJECT_ROOT, "ffmpeg_build", "dependencies", "downloads"),
                source_stage,
            )

        cmake_exe = _get_cmake_archiver()
        capture_archive = os.path.join(PACKAGE_OUTPUT_DIR, CAPTUREENGINE_PACKAGE_NAME)
        testapps_archive = os.path.join(PACKAGE_OUTPUT_DIR, TESTAPPS_PACKAGE_NAME)
        source_archive = os.path.join(PACKAGE_OUTPUT_DIR, FFMPEG_SOURCE_PACKAGE_NAME)
        capture_members = _create_7z_archive(cmake_exe, staging_root, "captureengine", capture_archive)
        testapp_members = _create_7z_archive(cmake_exe, staging_root, "testapps", testapps_archive)
        source_members = []
        if IS_WINDOWS:
            source_members = _create_7z_archive(
                cmake_exe, staging_root, "ffmpeg-corresponding-source", source_archive
            )

        required_capture_member = "captureengine/captureengine.exe"
        required_note_member = "testapps/" + os.path.basename(TESTAPP_RUNTIME_NOTE)
        required_config_member = "testapps/" + os.path.basename(TESTAPP_CONFIG_TEMPLATE)
        if (
            required_capture_member not in capture_members
            or required_note_member not in testapp_members
            or required_config_member not in testapp_members
        ):
            raise RuntimeError("Automatic package verification failed: required archive member is missing")
        if any(member.startswith("testapps/x86/") for member in testapp_members):
            x86_config_member = "testapps/x86/" + os.path.basename(TESTAPP_CONFIG_TEMPLATE)
            if x86_config_member not in testapp_members:
                raise RuntimeError("Automatic package verification failed: x86 test-app config is missing")
        if any(member.lower().endswith(".dll") for member in testapp_members):
            raise RuntimeError("Automatic package verification failed: test-app archive contains a vendor DLL")
        if IS_WINDOWS and not {
            "ffmpeg-corresponding-source/SOURCE_MANIFEST.txt",
            "ffmpeg-corresponding-source/ffmpeg/configure",
        }.issubset(source_members):
            raise RuntimeError("Automatic package verification failed: corresponding-source archive is incomplete")

        record_verification_artifact("captureengine_package", capture_archive)
        record_verification_artifact("testapps_package", testapps_archive)
        if IS_WINDOWS:
            record_verification_artifact("ffmpeg_source_package", source_archive)
        record_verification_step(
            "package_archives",
            "passed",
            duration_seconds=time.time() - package_start,
            details={
                "captureengine_files": len(capture_files),
                "testapp_files": len(testapp_files),
                "ffmpeg_source_files": len(source_files),
                "captureengine_archive": capture_archive,
                "testapps_archive": testapps_archive,
                "ffmpeg_source_archive": source_archive if IS_WINDOWS else "not produced on this host",
            },
        )
        log(f"Packaged CaptureEngine: {capture_archive} ({os.path.getsize(capture_archive)} bytes)")
        log(f"Packaged test apps: {testapps_archive} ({os.path.getsize(testapps_archive)} bytes)")
        archives = [capture_archive, testapps_archive]
        if IS_WINDOWS:
            log(f"Packaged FFmpeg corresponding source: {source_archive} ({os.path.getsize(source_archive)} bytes)")
            archives.append(source_archive)
        return tuple(archives)
    except Exception as error:
        record_verification_step(
            "package_archives",
            "failed",
            duration_seconds=time.time() - package_start,
            details={"error": str(error)},
        )
        log(f"ERROR: Automatic package creation failed: {error}")
        raise
    finally:
        target = _validate_workspace_cleanup_target(staging_root)
        if os.path.exists(target):
            shutil.rmtree(target)


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


def should_package_outputs(
    *,
    tests_only: bool,
    no_build: bool,
    sanitize: bool,
    isolated_root: bool,
    skip_package: bool,
) -> bool:
    """Decide whether this top-level run produces the release archives.

    The finalize phase intentionally never packages: build_cli schedules the
    archive creation concurrently with the advisory lint pass so the fixed
    per-gate packaging cost overlaps lint instead of adding to it. Isolated
    sanitizer children, sanitizer-mode builds, tests-only runs, and
    `--no-build` runs produce no shippable closure and stay unpackaged.
    """
    return not tests_only and not no_build and not sanitize and not isolated_root and not skip_package


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
    "fuzz_hardware_sensor_protocol.cpp": "hardware_sensor_protocol",
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

    # ParseBridgeMessage is intentionally owned by the controller-side plugin
    # implementation rather than the common/ tree linked into every product.
    sensor_plugin_obj = os.path.join(obj_dir, "captureengine", "sensor_plugin.fuzz.o").replace("\\", "/")
    compile_tasks.append(
        (base_cflags, os.path.join(PROJECT_ROOT, "captureengine", "sensor_plugin.cpp"), sensor_plugin_obj)
    )

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
        support_objs = [sensor_plugin_obj] if name == "fuzz_hardware_sensor_protocol" else []
        run_command(
            [clang_exe, obj] + support_objs + common_objs + [fuzzer_archive] + fuzz_ldflags + ["-o", exe], env=env
        )
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
