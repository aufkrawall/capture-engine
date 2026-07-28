

def is_x86_compile_command(arguments: List[str]) -> bool:
    normalized = [arg.replace("\\", "/") for arg in arguments]
    return any(
        arg.startswith("--target=i686-w64")
        or (arg.startswith("--sysroot=") and "/mingw32" in arg)
        or "/build/msys64/mingw32/" in arg
        for arg in normalized
    )


@lru_cache(maxsize=4)
def _clangd_extra_flags_for_arch(arch: str, compiler: str) -> List[str]:
    if not IS_WINDOWS:
        return []

    compiler = compiler.replace("\\", "/")
    resource_dir = detect_clang_resource_dir(os.environ.copy(), compiler)
    resource_flag = f"-resource-dir={resource_dir}" if resource_dir else None
    builtin_include = _find_first_existing_path([os.path.join(resource_dir, "include")]) if resource_dir else None

    if arch == "x86":
        sysroot = os.path.join(MSYS2_DIR, "mingw32")
        sysroot_norm = sysroot.replace("\\", "/")
        stdlib_root = os.path.join(sysroot, "include", "c++")
        flags: List[str] = [
            "--target=i686-w64-windows-gnu",
            f"--sysroot={sysroot_norm}",
            "-stdlib=libstdc++",
        ]
        if resource_flag:
            flags.append(resource_flag)

        include_dirs: List[Optional[str]] = []
        if stdlib_root and os.path.isdir(stdlib_root):
            stdlib_versions = [
                os.path.join(stdlib_root, d)
                for d in os.listdir(stdlib_root)
                if os.path.isdir(os.path.join(stdlib_root, d))
            ]
            stdlib_versions.sort(reverse=True)
            if stdlib_versions:
                include_dirs.extend(
                    [
                        stdlib_versions[0].replace("\\", "/"),
                        _find_first_existing_path([os.path.join(stdlib_versions[0], "i686-w64-mingw32")]),
                        _find_first_existing_path([os.path.join(stdlib_versions[0], "backward")]),
                    ]
                )
        include_dirs.extend(
            [
                builtin_include,
                _find_first_existing_path([os.path.join(sysroot, "include")]),
            ]
        )
    else:
        sysroot = os.path.join(MSYS2_DIR, "clang64")
        flags = ["--target=x86_64-w64-windows-gnu"]
        if resource_flag:
            flags.append(resource_flag)

        include_dirs = [
            _find_first_existing_path(
                [
                    os.path.join(sysroot, "x86_64-w64-mingw32", "include", "c++", "v1"),
                    os.path.join(sysroot, "include", "c++", "v1"),
                ]
            ),
            builtin_include,
            _find_first_existing_path(
                [
                    os.path.join(sysroot, "x86_64-w64-mingw32", "include"),
                    os.path.join(sysroot, "include"),
                ]
            ),
        ]

    for include_dir in include_dirs:
        if include_dir:
            flags.append(f"-isystem{include_dir}")

    return flags


def enrich_compile_command_for_clangd(command: Dict[str, Any]) -> Dict[str, Any]:
    """Add explicit toolchain context so clangd does not fall back to MSVC headers."""
    arguments = list(command.get("arguments", []))
    if not arguments:
        return command

    compiler = arguments[0].replace("\\", "/")
    if "clang++" not in os.path.basename(compiler).lower():
        command["arguments"] = [normalize_compile_command_arg(arg) for arg in arguments]
        return command

    arch = "x86" if is_x86_compile_command(arguments) else "x64"
    for flag in _clangd_extra_flags_for_arch(arch, compiler):
        if flag.startswith("--target="):
            if not _has_flag_with_prefix(arguments, "--target="):
                _append_unique_flag(arguments, flag)
        elif flag.startswith("--sysroot="):
            if not _has_flag_with_prefix(arguments, "--sysroot="):
                _append_unique_flag(arguments, flag)
        elif flag.startswith("-stdlib="):
            if not _has_flag_with_prefix(arguments, "-stdlib="):
                _append_unique_flag(arguments, flag)
        elif flag.startswith("-resource-dir="):
            if not _has_flag_with_prefix(arguments, "-resource-dir="):
                _append_unique_flag(arguments, flag)
        else:
            _append_unique_flag(arguments, flag)

    command["arguments"] = [normalize_compile_command_arg(arg) for arg in arguments]
    return command


def write_compile_commands_json() -> Optional[str]:
    """Write compile_commands.json from the global COMPILE_COMMANDS list.

    Registered with atexit so the compilation database is always persisted,
    even when the build fails part-way through. Partial/updated entries are
    better than a stale database for LSP diagnostics.
    """
    if not COMPILE_COMMANDS:
        return None
    try:
        # One source can be compiled several times with different flags: the
        # test apps build dx9_test.cpp as both plain D3D9 and D3D9Ex, plus x86
        # variants, and every variant shares the same "file" key. Ordering the
        # duplicates only by file left the survivor decided by whichever
        # parallel compile task appended last, so the recorded flags - and with
        # them the clang-tidy findings for that source - flipped between builds
        # and made the lint ratchet non-deterministic. Sort on the full key and
        # keep the first match: non-x86 sorts before x86, then flags decide.
        seen_files = set()
        unique_commands = []
        enriched_commands = [enrich_compile_command_for_clangd(dict(cmd)) for cmd in COMPILE_COMMANDS]
        sorted_commands = sorted(
            enriched_commands,
            key=lambda c: (c["file"], is_x86_compile_command(c["arguments"]), c["arguments"]),
        )
        for enriched_cmd in sorted_commands:
            if enriched_cmd["file"] not in seen_files:
                unique_commands.append(enriched_cmd)
                seen_files.add(enriched_cmd["file"])
        compile_commands_path = get_compile_commands_path()
        payload = json.dumps(unique_commands, indent=4) + "\n"
        changed = write_text_atomic_if_changed(compile_commands_path, payload)
        log(
            f"{'Generated' if changed else 'Validated'} compile_commands.json ({len(unique_commands)} entries)",
            detail=not changed,
        )
        if os.path.exists(os.path.join(PROJECT_ROOT, ".clangd")):
            log("LSP: leaving .clangd unchanged; compile_commands.json is authoritative", detail=True)
        return compile_commands_path
    except Exception as e:
        log(f"Error writing compile_commands.json: {e}")
        return None


atexit.register(write_compile_commands_json)


def write_json_atomic(path: str, payload: Any) -> None:
    """Write JSON payload atomically to avoid partial/corrupted files."""
    tmp_path = f"{path}.tmp.{os.getpid()}"
    try:
        with open(tmp_path, "w", encoding="utf-8") as f:
            json.dump(payload, f, indent=4)
            f.write("\n")
        os.replace(tmp_path, path)
    finally:
        if os.path.exists(tmp_path):
            try:
                os.remove(tmp_path)
            except OSError:
                pass


def detect_clang_resource_dir(env: Dict[str, str], clang_exe: str) -> Optional[str]:
    """Detect clang resource-dir via compiler query, then fallback to local scan."""
    if clang_exe and is_clang_compiler(clang_exe):
        detected = run_command(
            [os.path.normpath(clang_exe), "--print-resource-dir"],
            env=env,
            fail_exit=False,
        ).strip()
        if detected:
            detected_norm = os.path.normpath(detected)
            if os.path.isdir(detected_norm):
                return detected_norm.replace("\\", "/")

    clang_lib_dir = os.path.join(PROJECT_ROOT, "build", "msys64", "clang64", "lib", "clang")
    if not os.path.isdir(clang_lib_dir):
        return None

    versions = [
        d for d in os.listdir(clang_lib_dir) if os.path.isdir(os.path.join(clang_lib_dir, d)) and d and d[0].isdigit()
    ]
    if not versions:
        return None

    versions.sort(key=lambda v: [int(x) for x in re.findall(r"\d+", v)], reverse=True)
    return os.path.join(clang_lib_dir, versions[0]).replace("\\", "/")


def _verify_cross_object(obj: str, compiler: str, src: str) -> None:
    """On Linux, verify that a freshly compiled cross-object is PE/COFF as expected."""
    if not IS_LINUX:
        return
    file_exe = shutil.which("file")
    if not file_exe:
        return
    try:
        result = subprocess.run(
            [file_exe, "-b", obj],
            capture_output=True, text=True, timeout=15,
        )
        if result.returncode == 0:
            desc = result.stdout.strip()
            if not desc:
                return
            if "PE" in desc or "COFF" in desc or "MS Windows" in desc:
                log(f"Object verified as PE/COFF: {os.path.basename(obj)}", detail=True)
                return
            if desc in ("data", "empty"):
                return
            if "ELF" in desc:
                log(
                    f"ERROR: Cross-compiled object is ELF, not PE/COFF: {obj}\n"
                    f"       Compiler: {compiler}\n"
                    f"       Source: {src}\n"
                    f"       Detected: {desc}"
                )
            else:
                log(
                    f"WARNING: Cross-compiled object has unexpected type: {obj}\n"
                    f"       Compiler: {compiler}\n"
                    f"       Detected: {desc}"
                )
    except Exception as e:
        log(f"WARNING: Could not verify object format for {obj}: {e}", detail=True)


def compile_object(env: Dict[str, str], clang_exe: str, cflags: List[str], src: str, obj: str) -> bool:
    """Compile a single object file. Returns True if compiled, False if skipped."""
    dep_file = obj + ".d"

    # Add dependency tracking flags
    # -MMD: Generate dependency file, ignore system headers
    # -MF: Specify output dependency file
    compile_flags = cflags + ["-MMD", "-MF", dep_file]

    # Construct the full command list for compile_commands.json
    full_cmd_list = [clang_exe] + compile_flags + ["-c", src, "-o", obj]

    # Add to global compile commands list
    # Use 'arguments' list instead of 'command' string for better cross-platform/shell reliability
    # Normalize paths for cross-platform LSP compatibility (always use forward slashes)
    normalized_dir = os.path.abspath(PROJECT_ROOT).replace("\\", "/")
    normalized_file = os.path.abspath(src).replace("\\", "/")
    normalized_args = [normalize_compile_command_arg(arg) for arg in full_cmd_list]
    COMPILE_COMMANDS.append(
        {
            "directory": normalized_dir,
            "arguments": normalized_args,
            "file": normalized_file,
        }
    )

    # Always verify the compiled object file on Linux cross-compilation,
    # whether freshly compiled or reused from cache.
    if IS_LINUX and os.path.isfile(obj):
        _verify_cross_object(obj, clang_exe, src)

    if not should_recompile(src, obj, dep_file, env, clang_exe, compile_flags):
        return False  # Skip - up to date

    # Use ccache if available
    ccache_exe = shutil.which("ccache", path=env["PATH"])
    if env.get("DISABLE_CCACHE"):
        ccache_exe = None

    if ccache_exe:
        # ccache on Windows often dislikes absolute paths for the compiler
        cmd = [ccache_exe, os.path.basename(clang_exe)] + compile_flags + ["-c", src, "-o", obj]
    else:
        cmd = [clang_exe] + compile_flags + ["-c", src, "-o", obj]

    run_command(cmd, env=env)

    # Save compile signature after successful compilation.
    hash_file = obj + ".hash"
    try:
        dependencies = parse_dep_file(dep_file)
        with open(hash_file, "w") as f:
            f.write(compute_build_signature(src, clang_exe, compile_flags, dependencies))
    except Exception:
        pass  # Non-critical, ignore errors

    return True


def parallel_compile_varied(env, clang_exe, compile_tasks):
    """Compile (flags, source, object) tasks through one bounded worker pool."""
    compile_tasks = list(compile_tasks)
    object_owners: Dict[str, str] = {}
    for _, source, object_path in compile_tasks:
        normalized_object = os.path.normcase(os.path.abspath(object_path))
        previous_source = object_owners.get(normalized_object)
        if previous_source is not None:
            raise RuntimeError(
                f"Multiple compile tasks target the same object {object_path}: {previous_source} and {source}"
            )
        object_owners[normalized_object] = source

    # Populate this once before worker threads race through object signatures; otherwise an empty
    # functools cache can hash the same large compiler executable concurrently in several workers.
    compute_compiler_fingerprint(clang_exe)
    num_workers = get_parallel_job_count(env, len(compile_tasks))
    compiled = 0
    skipped = 0
    total = len(compile_tasks)
    completed = 0

    def compile_one(args):
        cflags, src, obj = args
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        return compile_object(env, clang_exe, cflags, src, obj), src, obj

    with ThreadPoolExecutor(max_workers=num_workers) as executor:
        futures = {executor.submit(compile_one, task): task for task in compile_tasks}
        for future in as_completed(futures):
            was_compiled, src, obj = future.result()
            completed += 1
            if was_compiled:
                compiled += 1
            else:
                skipped += 1
            if completed <= 10 or completed == total or (completed % 50) == 0:
                state = "compiled" if was_compiled else "cached"
                log(
                    f"Compile progress: {completed}/{total} ({state}) - {os.path.relpath(src, PROJECT_ROOT)}",
                    detail=True,
                )

    log(f"Compile summary: {compiled} compiled, {skipped} cached, {total} total")

    return compiled, skipped


def parallel_compile(env, clang_exe, cflags, src_obj_pairs):
    """Compile multiple same-flag source files in parallel."""
    return parallel_compile_varied(env, clang_exe, [(cflags, src, obj) for src, obj in src_obj_pairs])


def get_unit_test_object_dir(env: Dict[str, str]) -> str:
    """Keep unit-test variants separate from objects linked into product binaries."""
    variant = "x64-tests-sanitize" if env.get("CE_SANITIZE") == "1" else "x64-tests"
    return os.path.join(OBJ_DIR, variant)


def compile_tests(env, clang_exe, cflags, pkg_config, obj_dir):
    test_base_cflags = [flag for flag in cflags if not flag.startswith("-flto")]
    strict_fp_flags = get_strict_fp_flags(clang_exe)
    log(f"Compiling Tests (parallel, {get_parallel_job_count(env, 1_000_000)} threads, non-LTO)...")
    src_files = glob.glob(os.path.join(PROJECT_ROOT, "tests", "*.cpp"))
    if not src_files:
        log("No test files found.")
        return

    tests_source_dir = os.path.join(PROJECT_ROOT, "tests")
    tests_dir = TEST_OUTPUT_DIR
    os.makedirs(tests_dir, exist_ok=True)
    if ISOLATED_BUILD_ROOT:
        isolated_config_dir = os.path.join(ISOLATED_BUILD_ROOT, "captureengine")
        os.makedirs(isolated_config_dir, exist_ok=True)
        shutil.copy2(
            os.path.join(PROJECT_ROOT, "captureengine", "config.ini.template"),
            os.path.join(isolated_config_dir, "config.ini.template"),
        )
    test_exe = os.path.join(tests_dir, "unit_tests.exe")
    compile_tasks = []

    # 1. Get FFmpeg flags
    if IS_LINUX:
        ffmpeg_cflags, ffmpeg_link_flags = get_linux_ffmpeg_build_flags(env, pkg_config)
        gtest_link_inputs = resolve_msys2_gtest_link_inputs(
            get_linux_msys2_gtest_lib_dir("x64"),
            prefer_static=True,
        )
        gtest_link_inputs.append("-lwinpthread")
    else:
        # Use local FFmpeg on Windows
        env_ffmpeg = env.copy()
        env_ffmpeg["PKG_CONFIG_PATH"] = get_windows_ffmpeg_pkg_config_path(env_ffmpeg.get("PKG_CONFIG_PATH", ""))

        pkgs = ["libavcodec", "libavformat", "libavutil", "libswresample", "libswscale"]
        ffmpeg_cflags = run_command([pkg_config, "--cflags"] + pkgs, env=env_ffmpeg).strip().split()
        ffmpeg_lib_dir = os.path.join(FFMPEG_DIR, "lib")
        ffmpeg_link_flags = [
            os.path.join(ffmpeg_lib_dir, "libavformat.dll.a"),
            os.path.join(ffmpeg_lib_dir, "libavcodec.dll.a"),
            os.path.join(ffmpeg_lib_dir, "libswresample.dll.a"),
            os.path.join(ffmpeg_lib_dir, "libswscale.dll.a"),
            os.path.join(ffmpeg_lib_dir, "libavutil.dll.a"),
        ]
        gtest_link_inputs = ["-lgtest", "-lgtest_main"]

    msys2_dir = get_linux_msys2_dir() if IS_LINUX else MSYS2_DIR
    vulkan_lib = os.path.join(msys2_dir, "clang64", "lib", "libvulkan-1.dll.a")

    # Tests use a dedicated object directory. Their flags intentionally differ
    # from product flags, so sharing object paths would make every build switch
    # the cache back and forth and could link test-flag objects into CaptureEngine.
    common_objs = []
    common_src_obj_pairs = []
    for src in glob.glob(os.path.join(PROJECT_ROOT, "common", "*.cpp")):
        if os.path.basename(src) == "build_identity.cpp":
            continue
        rel_path = os.path.relpath(src, PROJECT_ROOT)
        obj = os.path.join(obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
        common_src_obj_pairs.append((src, obj))
        common_objs.append(obj)
    compile_tasks.extend((test_base_cflags, src, obj) for src, obj in common_src_obj_pairs)

    # Link against gtest, common, hook/common sources, mediaengine, and FFmpeg.
    # Keep this aligned with the actual hook/mediaengine linker inputs to avoid
    # dragging in stale transitive dependencies that are not shipped in MSYS2.
    ldflags_test = (
        [
            "-static-libgcc",
            "-static-libstdc++",
            "-Wl,--allow-multiple-definition",
        ]
        + gtest_link_inputs
        + [
            "-ld3d9",
            "-ld3d10",
            "-ld3d11",
            "-ld3d12",
            "-ld3dcompiler",
            "-ldxguid",
            "-lws2_32",
            "-lole32",
            "-lwinmm",
            "-luser32",
            "-lgdi32",
            "-lopengl32",
            vulkan_lib,
            "-lversion",
            "-ldxgi",
            "-lpdh",
            "-lpsapi",
            "-lavrt",
            "-ldbghelp",
            "-lshlwapi",
            "-ldwmapi",
            "-lshcore",
            "-lwindowscodecs",
            "-lmfplat",
            "-lmfuuid",
            "-lbcrypt",
            "-lsecur32",
            "-lmmdevapi",
            "-luuid",
            "-lsetupapi",
            "-lcfgmgr32",
            "-ladvapi32",
        ]
        + ffmpeg_link_flags
    )
    if any(flag.startswith("-fsanitize=") for flag in cflags):
        ldflags_test.append("-fsanitize=address,undefined")
    ldflags_test.extend(LD_OPT_FLAGS)
    ldflags_test.extend(get_x64_linker_flags(clang_exe))
    append_windows_pdb_linker_flag(ldflags_test, test_exe)

    # 2. Compile MediaEngine objects for tests
    me_src = glob.glob(os.path.join(PROJECT_ROOT, "mediaengine", "*.cpp"))
    me_objs = []
    src_obj_pairs = []
    strict_fp_src_obj_pairs = []
    # We need to compile MediaEngine with MEDIAENGINE_EXPORTS or similar if needed,
    # but for static linking in tests, we just need the symbols.
    # Note: AudioEncoder.cpp might rely on specific defines.
    me_cflags = test_base_cflags + ffmpeg_cflags + ["-DMEDIAENGINE_EXPORTS"]

    for src in me_src:
        rel_path = os.path.relpath(src, PROJECT_ROOT)
        obj = os.path.join(obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
        if os.path.basename(src) in STRICT_FP_MEDIA_SOURCES:
            strict_fp_src_obj_pairs.append((src, obj))
        else:
            src_obj_pairs.append((src, obj))
        me_objs.append(obj)

    compile_tasks.extend((me_cflags, src, obj) for src, obj in src_obj_pairs)
    compile_tasks.extend((me_cflags + strict_fp_flags, src, obj) for src, obj in strict_fp_src_obj_pairs)

    # 3. Compile Tests
    test_cflags = (
        test_base_cflags
        + ffmpeg_cflags
        + [
            "-DCE_UNIT_TESTS",
            "-I" + os.path.join(PROJECT_ROOT, "mediaengine"),
            "-I" + os.path.join(PROJECT_ROOT, "hook", "wrappers"),
            "-I" + os.path.join(PROJECT_ROOT, "hook", "common"),
            "-I" + os.path.join(msys2_dir, "clang64", "include"),
        ]
    )  # Ensure we can include audio_encoder.h and hook headers for stubs
    test_objs = []
    src_obj_pairs = []
    for src in src_files:
        rel_path = os.path.relpath(src, PROJECT_ROOT)
        obj = os.path.join(obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
        src_obj_pairs.append((src, obj))
        test_objs.append(obj)

    compile_tasks.extend((test_cflags, src, obj) for src, obj in src_obj_pairs)

    captureengine_test_objs = []
    for name in sorted(STRICT_FP_SCREENSHOT_SOURCES) + ["pseudo_overlay.cpp"]:
        extra = strict_fp_flags if name in STRICT_FP_SCREENSHOT_SOURCES else []
        obj = os.path.join(obj_dir, "captureengine", os.path.splitext(name)[0] + ".test.o").replace("\\", "/")
        compile_tasks.append((test_cflags + extra, os.path.join(PROJECT_ROOT, "captureengine", name), obj))
        captureengine_test_objs.append(obj)

    # 4. Compile hook/common for tests
    hook_common_src = glob.glob(os.path.join(PROJECT_ROOT, "hook", "common", "*.cpp"))
    hook_common_objs = []
    src_obj_pairs = []
    for src in hook_common_src:
        rel_path = os.path.relpath(src, PROJECT_ROOT)
        obj = os.path.join(obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
        src_obj_pairs.append((src, obj))
        hook_common_objs.append(obj)
    compile_tasks.extend((test_base_cflags, src, obj) for src, obj in src_obj_pairs)

    hook_wrapper_test_src = [os.path.join(PROJECT_ROOT, "hook", "wrappers", "hook_system.cpp")]
    hook_wrapper_test_objs = []
    src_obj_pairs = []
    for src in hook_wrapper_test_src:
        rel_path = os.path.relpath(src, PROJECT_ROOT)
        obj = os.path.join(obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
        src_obj_pairs.append((src, obj))
        hook_wrapper_test_objs.append(obj)
    compile_tasks.extend((test_cflags, src, obj) for src, obj in src_obj_pairs)

    parallel_compile_varied(env, clang_exe, compile_tasks)

    config_resource_obj = os.path.join(obj_dir, "tests", "config_template.res.o").replace("\\", "/")
    os.makedirs(os.path.dirname(config_resource_obj), exist_ok=True)
    run_command(
        [get_windres_exe("x64"), os.path.join(tests_source_dir, "config_template.rc"), "-o", config_resource_obj],
        env=env,
        cwd=tests_source_dir,
    )

    log("Linking Unit Tests...")
    # Order: Tests -> Common -> MediaEngine -> HookCommon -> HookWrappers -> Libs
    cmd = (
        [clang_exe]
        + test_objs
        + common_objs
        + me_objs
        + captureengine_test_objs
        + [config_resource_obj]
        + hook_common_objs
        + hook_wrapper_test_objs
        + ldflags_test
        + ["-o", test_exe]
    )
    required_outputs = [test_exe]
    if IS_WINDOWS:
        required_outputs.append(pdb_path_for_binary(test_exe))
    linked = run_cached_link(cmd, env, test_exe, required_outputs=required_outputs)
    if not linked:
        log("Unit test link cache hit")
    copy_test_runtime_dlls(tests_dir)
    return test_exe


def copy_test_runtime_dlls(tests_dir):
    """Copy libgtest.dll and FFmpeg DLLs next to unit_tests.exe so it can be run directly."""
    import shutil

    msys_bin = os.path.join(get_host_msys2_dir(), "clang64", "bin")
    ffmpeg_bin = os.path.join(BIN_DIR, "ffmpeg")
    source_built_names = {name.lower() for name in WINDOWS_FFMPEG_RUNTIME_DEPS + WINDOWS_FFMPEG_OPTIONAL_RUNTIME_DEPS}
    copied = []
    for dll_dir in [msys_bin, ffmpeg_bin]:
        if not os.path.isdir(dll_dir):
            continue
        for dll in os.listdir(dll_dir):
            if not dll.lower().endswith(".dll"):
                continue
            if dll_dir == msys_bin and dll.lower() in source_built_names:
                continue
            src = os.path.join(dll_dir, dll)
            dst = os.path.join(tests_dir, dll)
            if not os.path.exists(dst) or os.path.getmtime(src) > os.path.getmtime(dst):
                shutil.copy2(src, dst)
                copied.append(dll)
    if copied:
        log(f"Copied {len(copied)} runtime DLLs to tests/ for direct execution")


def record_unit_test_failure_output(cmd, result):
    """Persist failed unit-test stdout/stderr so an exit code is actionable."""
    stdout = result.stdout if isinstance(result.stdout, str) else ""
    stderr = result.stderr if isinstance(result.stderr, str) else ""
    failure_text = "\n".join(
        [
            f"command: {subprocess.list2cmdline(cmd)}",
            "",
            "[stdout]",
            stdout or "<empty>",
            "",
            "[stderr]",
            stderr or "<empty>",
            "",
        ]
    )
    failure_path = verification_artifact_path("unit_tests_failure.log")
    if not failure_path:
        return None
    write_text_atomic(failure_path, failure_text)
    record_verification_artifact("unit_tests_failure_log", failure_path)
    return failure_path


def log_unit_test_output_tail(label, output, max_lines=80):
    """Log only the diagnostic tail; the complete output is kept as an artifact."""
    if not isinstance(output, str) or not output:
        return
    lines = output.splitlines()
    for line in lines[-max_lines:]:
        log(f"[unit_tests:{label}] {line}")
