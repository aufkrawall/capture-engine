

def compile_project(
    env,
    clang_bin,
    skip_updates=False,
    should_run_tests=False,
    gtest_filter=None,
    tests_only=False,
    externals_prepared=False,
    run_python_tools=True,
):
    ensure_dirs()
    remove_obsolete_process_loopback_helper_artifacts()

    if externals_prepared:
        log("Reusing external dependency and FFmpeg preparation completed before sanitizer validation")
    else:
        external_start = time.time()
        compile_custom_ffmpeg(skip_updates=skip_updates)
        record_verification_step(
            "external_preparation",
            "passed",
            duration_seconds=time.time() - external_start,
            details={"skip_updates": skip_updates},
        )
    clang_exe = get_compiler_exe("x64")
    if clang_exe is None:
        log("ERROR: Compiler for x64 not found")
        sys.exit(1)
    pkg_config = shutil.which("pkg-config") if IS_LINUX else os.path.join(clang_bin, "pkg-config.exe")

    cflags = make_cpp_cflags(
        OPT_FLAGS_X64,
        compiler_exe=clang_exe,
        production_build=env.get("CE_PRODUCTION_BUILD") == "1",
    )
    if not compiler_supports_windows_cfg(clang_exe):
        log(
            "Linux MinGW GCC does not support Windows CFG; retaining CET, stack protection, fortify, "
            "ASLR, NX, high-entropy VA, and W^X verification"
        )

    if tests_only:
        log("Tests-only mode: building only unit test dependencies/executable")
        test_exe = compile_tests(
            env,
            clang_exe,
            cflags,
            pkg_config,
            get_unit_test_object_dir(env),
        )
        if should_run_tests and test_exe:
            if not run_tests(env, test_exe, gtest_filter=gtest_filter, run_python_tools=run_python_tools):
                sys.exit(1)
        log("Tests-only mode: stopping after unit test build/run")
        return

    # --- Architecture Loop ---
    arch_targets = ["x64"]
    if env.get("CE_SANITIZE") == "1":
        # MSYS2 currently ships ASan runtime only for x64 clang target.
        # Building x86 sanitizer binaries fails at link time.
        log("Sanitizer mode: skipping x86 targets (ASan runtime unavailable)")
    elif IS_LINUX and not has_linux_x86_compiler():
        log("Linux host: mingw-w64 x86 compiler not found, skipping x86 targets")
    else:
        arch_targets.append("x86")

    for arch in arch_targets:
        curr_env = env
        curr_clang_bin = clang_bin
        mingw_lib = ""
        std_lib_path = ""

        curr_obj_dir = os.path.join(OBJ_DIR, arch)
        os.makedirs(curr_obj_dir, exist_ok=True)

        if arch == "x86":
            curr_env, curr_clang_bin = get_env_x86()
            # Propagate build flags that are set on the main env but not copied
            # by get_env_x86() (which starts from a fresh os.environ.copy()).
            propagate_build_control_environment(env, curr_env)

        curr_clang_exe = get_compiler_exe(arch)
        if curr_clang_exe is None:
            log(f"ERROR: Compiler for {arch} not found")
            continue
        curr_pkg_config = shutil.which("pkg-config") if IS_LINUX else os.path.join(curr_clang_bin, "pkg-config.exe")

        if arch == "x64":
            curr_cflags = make_cpp_cflags(
                OPT_FLAGS_X64,
                compiler_exe=curr_clang_exe,
                suppress_microsoft_exception_spec=True,
            )
        else:  # x86
            x86_arch_flags = []
            if not IS_LINUX:
                x86_arch_flags = [
                    "--target=i686-w64-mingw32",
                    "--sysroot=" + os.path.join(MSYS2_DIR, "mingw32"),
                    "-mstackrealign",
                    "-stdlib=libstdc++",
                ]
            curr_cflags = make_cpp_cflags(
                OPT_FLAGS_X86,
                compiler_exe=curr_clang_exe,
                arch_flags=x86_arch_flags,
                suppress_microsoft_exception_spec=True,
                enable_cfg=False,
            )

        if arch == "x64":
            if not IS_LINUX:
                mingw_lib = os.path.join(MSYS2_DIR, "clang64", "lib")

        if arch == "x86":
            if not IS_LINUX:
                try:
                    # Use curr_clang_exe (the x86 clang++ binary) not clang_bin
                    # (which is the x64 bin directory and cannot be executed).
                    cmd = [
                        curr_clang_exe,
                        "-print-libgcc-file-name",
                        "--target=i686-w64-mingw32",
                    ]
                    res = subprocess.check_output(cmd, encoding="utf-8").strip()
                    std_lib_path = os.path.dirname(res)
                except Exception as e:
                    log(f"Warning: Failed to find 32-bit lib path: {e}")
                    std_lib_path = ""
                if not std_lib_path:
                    # Fallback to the known mingw32 lib directory so the linker
                    # can always find the runtime libraries.
                    std_lib_path = os.path.join(MSYS2_DIR, "mingw32", "lib")

        # 1. Compile Common (ImGui removed - using custom overlay)
        log(f"Compiling Common {arch}...")
        common_src = glob.glob(os.path.join(PROJECT_ROOT, "common", "*.cpp")) + glob.glob(
            os.path.join(PROJECT_ROOT, "common", "utils", "*.cpp")
        )
        common_objs: List[str] = []
        src_obj_pairs: List[tuple[str, str]] = []
        for src in common_src:
            rel_path = os.path.relpath(src, PROJECT_ROOT)
            obj = os.path.join(curr_obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
            src_obj_pairs.append((src, obj))
            common_objs.append(obj)
        parallel_compile(curr_env, curr_clang_exe, curr_cflags, src_obj_pairs)

        # 3. Compile Hook DLL
        log(f"Compiling Hook DLL {arch}...")
        hk_src = (
            glob.glob(os.path.join(PROJECT_ROOT, "hook", "*.cpp"))
            + glob.glob(os.path.join(PROJECT_ROOT, "hook", "common", "*.cpp"))
            + glob.glob(os.path.join(PROJECT_ROOT, "hook", "apis", "*.cpp"))
            + glob.glob(os.path.join(PROJECT_ROOT, "hook", "capture", "*.cpp"))
            + glob.glob(os.path.join(PROJECT_ROOT, "hook", "wrappers", "*.cpp"))
            # safe_hook.cpp REMOVED: Using custom_hook instead
        )

        # Exclude D3D12 device/commandqueue wrappers due to MinGW ABI incompatibility
        # (MSYS2's D3D12 headers use WIDL_EXPLICIT_AGGREGATE_RETURNS which has different vtable layout)
        excluded_files = [
            os.path.join(PROJECT_ROOT, "hook", "wrappers", "d3d12_device_wrap.cpp"),
            os.path.join(PROJECT_ROOT, "hook", "wrappers", "d3d12_commandqueue_wrap.cpp"),
            os.path.join(PROJECT_ROOT, "hook", "apis", "dx12_hook_stable.cpp"),  # WIP - not ready
        ]
        hk_src = [f for f in hk_src if f not in excluded_files]

        # Custom hook system (VTable + IAT patching, replaces MinHook)

        hk_dll = os.path.join(BIN_DIR, f"capture_hook_{arch}.dll")

        # Get vulkan lib path (use compiler-resolved import library on Linux)
        vulkan_lib = get_linux_vulkan_import_lib_path(arch)
        if vulkan_lib is None:
            if IS_LINUX and arch == "x86":
                log(f"Linux host: skipping Hook DLL {arch} - Vulkan import library unavailable")
                continue
            raise RuntimeError(f"Vulkan import library unavailable for {arch}")

        # Use delay-load for graphics DLLs so the hook can load even in games that don't have them
        # This prevents crash during DLL load when injecting into games that don't use D3D12/D3D11/etc
        ldflags_hook: List[str] = [
            "-shared",
            "-static",
        ]
        if IS_LINUX:
            ldflags_hook.extend(["-static-libgcc", "-static-libstdc++"])

        # Add lib path for non-Linux
        if not IS_LINUX:
            if arch == "x86" and std_lib_path:
                ldflags_hook.append("-L" + std_lib_path)
            elif mingw_lib:
                ldflags_hook.append("-L" + mingw_lib)

        ldflags_hook.extend(
            [
                "-ld3d9",
                "-ld3d10",
                "-ld3d11",
                "-ld3dcompiler",
                "-ldxguid",
                "-lws2_32",
                "-lole32",
                "-luuid",
                "-lwinmm",
                "-luser32",
                "-lgdi32",
                "-lopengl32",
                "-lversion",
                "-ldxgi",
                "-ld3d12",
                "-lpdh",
                "-lpsapi",
                "-lavrt",
                "-ldbghelp",
                "-lbcrypt",
                "-lntdll",
                "-ladvapi32",
            ]
        )
        ldflags_hook.append(vulkan_lib)

        ldflags_hook.extend(LD_OPT_FLAGS)
        if arch == "x86" and IS_LINUX:
            ldflags_hook.append("-Wl,--allow-multiple-definition")
        if arch == "x64":
            ldflags_hook.extend(get_x64_linker_flags(curr_clang_exe))

        # LLD linker - use on Windows MSYS2, fallback to default on Linux
        if not IS_LINUX:
            ldflags_hook.extend(["-fuse-ld=lld", "-Wl,--exclude-all-symbols"])
            if arch == "x86":
                ldflags_hook.extend(
                    [
                        "--target=i686-w64-mingw32",
                        "--sysroot=" + os.path.join(MSYS2_DIR, "mingw32"),
                        "-stdlib=libstdc++",
                        "-static-libstdc++",
                        "-rtlib=libgcc",
                        "--unwindlib=libgcc",
                        "-lpthread",
                    ]
                )

        append_windows_pdb_linker_flag(ldflags_hook, hk_dll)

        # Hook DLL must use conservative arch flags (injected into game processes
        # with unknown CPU support). Replace curr_cflags march/ffast-math flags.
        if arch == "x64":
            hook_base_cflags = make_cpp_cflags(
                HOOK_OPT_FLAGS_X64,
                compiler_exe=curr_clang_exe,
                suppress_microsoft_exception_spec=True,
            )
        else:
            hook_base_cflags = make_cpp_cflags(
                HOOK_OPT_FLAGS_X86,
                compiler_exe=curr_clang_exe,
                arch_flags=(
                    [
                        "--target=i686-w64-mingw32",
                        "--sysroot=" + os.path.join(MSYS2_DIR, "mingw32"),
                        "-mstackrealign",
                        "-stdlib=libstdc++",
                    ]
                    if not IS_LINUX
                    else []
                ),
                suppress_microsoft_exception_spec=True,
                enable_cfg=False,
            )

        hk_cflags = (
            hook_base_cflags
            + ["-DVK_NO_PROTOTYPES", "-DBUILDING_CAPTURE_HOOK"]
            + [  # Vulkan hooks now in layer
                "-I" + os.path.join(PROJECT_ROOT, "hook", "common"),
                "-I" + os.path.join(PROJECT_ROOT, "hook", "apis"),
                "-I" + os.path.join(PROJECT_ROOT, "hook", "capture"),
                "-I" + os.path.join(PROJECT_ROOT, "hook", "wrappers"),
            ]
        )

        # Add Vulkan headers include path (from MSYS2 on Linux)
        if IS_LINUX:
            vulkan_include = os.path.join(get_linux_msys2_dir(), "clang64", "include")
            if os.path.exists(vulkan_include):
                hk_cflags.extend(["-idirafter", vulkan_include])

        hk_objs: List[str] = []
        src_obj_pairs: List[tuple[str, str]] = []
        for src in hk_src:
            rel_path = os.path.relpath(src, PROJECT_ROOT)
            obj = os.path.join(curr_obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
            src_obj_pairs.append((src, obj))
            hk_objs.append(obj)

        parallel_compile(curr_env, curr_clang_exe, hk_cflags, src_obj_pairs)

        log(f"Linking Hook DLL {arch}...")

        # Robust handling for locked DLLs (e.g. by DataExchangeHost, explorer, etc.)
        if os.path.exists(hk_dll):
            if not safe_delete_file(hk_dll):
                # Even if we can't delete, we can still build if we renamed it
                if os.path.exists(hk_dll):
                    log(f"[Warning] {os.path.basename(hk_dll)} is still locked, build may fail")
                    if is_file_locked(hk_dll):
                        log("[Info] File is actively locked by another process")
                        locking = find_process_locking_file(hk_dll)
                        if locking:
                            log(f"[Info] Locking process: {locking}")

        cmd: List[str] = [curr_clang_exe] + hk_objs + common_objs + ldflags_hook + ["-o", hk_dll]
        # cmd = [curr_clang_exe] + hk_objs + ldflags_hook + ["-o", hk_dll]
        run_command(cmd, env=curr_env)

        # Verify the built binary contains the correct version
        if os.path.exists(hk_dll):
            record_verification_artifact(f"hook_dll_{arch}", hk_dll)
            try:
                # Use strings to extract version from binary
                import subprocess as sp

                strings_exe = os.path.join(MSYS2_DIR, "clang64", "bin", "strings.exe")
                if not os.path.exists(strings_exe):
                    strings_exe = "strings"  # fallback
                result = sp.run([strings_exe, hk_dll], capture_output=True, text=True, timeout=10)
                expected_version = f"0.1.{CURRENT_BUILD_NUMBER}"
                if expected_version in result.stdout:
                    log(f"[OK] Hook DLL verified: {expected_version}")
                else:
                    version_match = re.search(r"0\.1\.(\d+)", result.stdout)
                    if version_match:
                        embedded_build = int(version_match.group(1))
                        if embedded_build != CURRENT_BUILD_NUMBER:
                            log(
                                f"[WARNING] Version mismatch! Header: build.{CURRENT_BUILD_NUMBER},"
                                f" DLL: build.{embedded_build}"
                            )
                        else:
                            log(f"[OK] Hook DLL verified: build.{embedded_build}")
                    else:
                        log(
                            f"[WARNING] Could not find version string {expected_version} in "
                            f"{os.path.basename(hk_dll)}"
                        )
            except Exception as e:
                log(f"[Warning] Could not verify DLL version: {e}")

        # generate_hash(hk_dll) # Removed in favor of embedded hash header

        # 4. MediaEngine (x64 only for now as requested)
        if arch == "x64":
            log("Compiling MediaEngine x64...")
            me_src = glob.glob(os.path.join(PROJECT_ROOT, "mediaengine", "*.cpp"))
            if me_src:
                # Get FFmpeg flags
                if IS_LINUX:
                    ffmpeg_flags, ffmpeg_import_libs = get_linux_ffmpeg_build_flags(curr_env, curr_pkg_config)
                else:
                    # Use local FFmpeg on Windows
                    env_ffmpeg = curr_env.copy()
                    env_ffmpeg["PKG_CONFIG_PATH"] = get_windows_ffmpeg_pkg_config_path(
                        env_ffmpeg.get("PKG_CONFIG_PATH", "")
                    )

                    pkgs = [
                        "libavcodec",
                        "libavformat",
                        "libavutil",
                        "libswresample",
                        "libswscale",
                    ]
                    pkg_cmd = [curr_pkg_config, "--cflags", "--libs"] + pkgs
                    # Removed --static for pkg-config to get shared linking flags

                    ffmpeg_flags_raw = run_command(pkg_cmd, env=env_ffmpeg).strip().split()
                    ffmpeg_flags: List[str] = [f for f in ffmpeg_flags_raw if f not in ["-ldl", "-lshaderc_shared"]]
                    ffmpeg_lib_dir = os.path.join(FFMPEG_DIR, "lib")
                    ffmpeg_import_libs: List[str] = [
                        os.path.join(ffmpeg_lib_dir, "libavformat.dll.a"),
                        os.path.join(ffmpeg_lib_dir, "libavcodec.dll.a"),
                        os.path.join(ffmpeg_lib_dir, "libswresample.dll.a"),
                        os.path.join(ffmpeg_lib_dir, "libswscale.dll.a"),
                        os.path.join(ffmpeg_lib_dir, "libavutil.dll.a"),
                    ]

                # For shared build, linking usually requires -Lpath -lavcodec.
                # We need to make sure the DLLs are findable at runtime.
                # FFmpeg shared libraries stay isolated under bin/ffmpeg.
                # captureengine/mediaengine_loader.cpp sets SetDllDirectoryA(<exeDir>\ffmpeg)
                # before loading mediaengine.dll, so these delay-loaded imports resolve from there.

                me_dll = os.path.join(BIN_DIR, "mediaengine.dll")
                me_lib = os.path.join(BIN_DIR, "libmediaengine.dll.a")

                me_ldflags: List[str] = [
                    "-shared",
                    "-static",
                    "-static-libgcc",
                    "-static-libstdc++",
                    "-Wl,--gc-sections",
                    "-Wl,--allow-multiple-definition",
                    "-lole32",
                    "-lmfplat",
                    "-lmfuuid",
                    "-lbcrypt",
                    "-lsecur32",
                    "-lshlwapi",
                    "-lpsapi",
                    "-lws2_32",
                    "-luser32",
                    "-ld3d11",
                    "-ldxgi",
                    "-lmmdevapi",
                    "-lversion",
                    "-lwinmm",
                    "-luuid",
                    "-lsetupapi",
                    "-lcfgmgr32",
                    "-ladvapi32",
                    "-lgdi32",
                ]
                me_ldflags.extend(LD_OPT_FLAGS)
                me_ldflags.extend(get_x64_linker_flags(curr_clang_exe))
                if curr_env.get("CE_DISABLE_LTO") != "1":
                    # LTO disabled for mediaengine: on MinGW/clang, LTO can strip
                    # exception handling tables needed for D3D11 SEH exception catching
                    pass  # me_ldflags.append("-flto") - DISABLED for D3D11 exception safety
                if any(flag.startswith("-fsanitize=") for flag in curr_cflags):
                    me_ldflags.append("-fsanitize=address,undefined")
                # Don't strip sections for mediaengine - needed for exception handling tables
                # --gc-sections strips .eh_frame/.gcc_except_table, preventing catch(...) from working
                me_ldflags.append("-Wl,--no-gc-sections")
                me_ldflags.append(f"-Wl,--out-implib,{me_lib}")
                append_windows_pdb_linker_flag(me_ldflags, me_dll)

                me_cflags = curr_cflags + ["-DMEDIAENGINE_EXPORTS"] + ffmpeg_flags
                # Remove LTO from mediaengine: on MinGW/clang, LTO strips exception handling
                # tables needed to catch D3D11's SEH exceptions (0xE06D7363) from OpenSharedFence
                # and other D3D11 APIs. These functions throw instead of returning HRESULT errors.
                me_cflags = [f for f in me_cflags if not f.startswith("-flto")]
                me_objs: List[str] = []
                src_obj_pairs: List[tuple[str, str]] = []
                strict_fp_src_obj_pairs: List[tuple[str, str]] = []
                for src in me_src:
                    rel_path = os.path.relpath(src, PROJECT_ROOT)
                    obj = os.path.join(curr_obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
                    if os.path.basename(src) in STRICT_FP_MEDIA_SOURCES:
                        strict_fp_src_obj_pairs.append((src, obj))
                    else:
                        src_obj_pairs.append((src, obj))
                    me_objs.append(obj)
                parallel_compile(curr_env, curr_clang_exe, me_cflags, src_obj_pairs)
                parallel_compile(
                    curr_env,
                    curr_clang_exe,
                    me_cflags + get_strict_fp_flags(curr_clang_exe),
                    strict_fp_src_obj_pairs,
                )

                log("Linking MediaEngine x64...")
                temp_me_dll = os.path.join(curr_obj_dir, "mediaengine.tmp.dll")
                safe_delete_file(temp_me_dll)
                cmd: List[str] = (
                    [curr_clang_exe] + me_objs + common_objs + me_ldflags + ffmpeg_import_libs + ["-o", temp_me_dll]
                )
                run_command(cmd, env=curr_env)
                if not safe_copy_file(temp_me_dll, me_dll):
                    log("ERROR: Failed to place mediaengine.dll (destination may be locked)")
                    sys.exit(1)
                safe_delete_file(temp_me_dll)
                record_verification_artifact("mediaengine_dll", me_dll)
                # generate_hash(me_dll) # MediaEngine doesn't need hash check for injection
                # Note: mediaengine.dll is output directly to BIN_DIR (main folder)
                # It acts as a bridge to FFmpeg DLLs in ffmpeg/ subfolder

                # Copy FFmpeg DLLs to bin/ffmpeg/ for runtime (Linux)
                if IS_LINUX:
                    log("Copying FFmpeg DLLs to bin/ffmpeg/...")
                    ffmpeg_bin_src = os.path.join(get_linux_ffmpeg_root(), "bin")
                    ffmpeg_bin_dst = os.path.join(BIN_DIR, "ffmpeg")
                    sync_ffmpeg_runtime_dlls(
                        ffmpeg_bin_src,
                        ffmpeg_bin_dst,
                        LINUX_FFMPEG_RUNTIME_DEPS,
                        [os.path.join(get_linux_msys2_dir(), "clang64", "bin")] + get_linux_mingw_runtime_dirs(arch),
                    )

    # Always compile unit-test sources so compile_commands.json contains
    # authoritative entries for tests even on non-test builds. Execute the test
    # binary only when explicitly requested.
    test_exe = compile_tests(
        env,
        clang_exe,
        cflags,
        pkg_config,
        get_unit_test_object_dir(env),
    )
    if should_run_tests and test_exe:
        if not run_tests(env, test_exe, gtest_filter=gtest_filter, run_python_tools=run_python_tools):
            sys.exit(1)

    # 5. CaptureEngine (x64 only for now)
    log("Compiling CaptureEngine x64...")
    ce_src = glob.glob(os.path.join(PROJECT_ROOT, "captureengine", "*.cpp"))
