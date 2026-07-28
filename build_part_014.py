if False:
    if ce_src:
        ce_exe = os.path.join(BIN_DIR, "captureengine.exe")
        me_lib = os.path.join(BIN_DIR, "libmediaengine.dll.a")
        ce_obj_dir = os.path.join(OBJ_DIR, "x64")
        if IS_LINUX:
            ce_ffmpeg_cflags, _ = get_linux_ffmpeg_build_flags(env, pkg_config)
        else:
            env_ffmpeg = env.copy()
            env_ffmpeg["PKG_CONFIG_PATH"] = get_windows_ffmpeg_pkg_config_path(env_ffmpeg.get("PKG_CONFIG_PATH", ""))
            pkgs = [
                "libavcodec",
                "libavformat",
                "libavutil",
                "libswresample",
                "libswscale",
            ]
            ce_ffmpeg_cflags: List[str] = run_command([pkg_config, "--cflags"] + pkgs, env=env_ffmpeg).strip().split()

        ce_objs: List[str] = []
        src_obj_pairs: List[tuple[str, str]] = []
        strict_fp_src_obj_pairs: List[tuple[str, str]] = []
        for src in ce_src:
            if "screen_capture.cpp" in src:
                continue
            rel_path = os.path.relpath(src, PROJECT_ROOT)
            obj = os.path.join(ce_obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
            if os.path.basename(src) in STRICT_FP_SCREENSHOT_SOURCES:
                strict_fp_src_obj_pairs.append((src, obj))
            else:
                src_obj_pairs.append((src, obj))
            ce_objs.append(obj)
        parallel_compile(env, clang_exe, cflags + ce_ffmpeg_cflags, src_obj_pairs)
        parallel_compile(
            env,
            clang_exe,
            cflags + ce_ffmpeg_cflags + get_strict_fp_flags(clang_exe),
            strict_fp_src_obj_pairs,
        )

        # Resource file
        rc_file = os.path.join(PROJECT_ROOT, "captureengine", "captureengine.rc")
        rc_obj = os.path.join(ce_obj_dir, "captureengine", "captureengine.res.o").replace("\\", "/")
        if os.path.exists(rc_file):
            windres = get_windres_exe("x64")
            log("Compiling resource file (manifest)...")
            cmd = [windres, rc_file, "-o", rc_obj]
            run_command(cmd, env=env, cwd=os.path.join(PROJECT_ROOT, "captureengine"))
            ce_objs.append(rc_obj)

        log("Linking CaptureEngine x64...")
        ffmpeg_lib_dir = os.path.join(get_linux_ffmpeg_root(), "lib") if IS_LINUX else os.path.join(FFMPEG_DIR, "lib")
        ce_ldflags: List[str] = [
            "-mwindows",
            "-static",
            "-static-libgcc",
            "-static-libstdc++",
        ]
        ce_ldflags.extend(LD_OPT_FLAGS)
        ce_ldflags.extend(get_x64_linker_flags(clang_exe))
        ce_ldflags.extend(
            [
                "-ld3d11",
                "-ldxgi",
                "-luser32",
                "-lshell32",
                "-lshlwapi",
                "-lpsapi",
                "-lwinmm",
                "-lavrt",
                "-lruntimeobject",
                "-lole32",
                "-loleaut32",
                "-lwindowscodecs",
                "-ldbghelp",
                "-lwbemuuid",
                "-lbcrypt",
                "-lwintrust",
                "-lpdh",
                "-lversion",
                "-lntdll",
                "-ladvapi32",
                # FFmpeg for HDR screenshot encoding (AVIF via libaom) — delay-loaded so SetDllDirectory works
                os.path.join(ffmpeg_lib_dir, "libavformat.dll.a"),
                os.path.join(ffmpeg_lib_dir, "libavcodec.dll.a"),
                os.path.join(ffmpeg_lib_dir, "libavutil.dll.a"),
            ]
        )
        if not IS_LINUX:
            ffmpeg_runtime_names = resolve_ffmpeg_runtime_dll_names(os.path.join(FFMPEG_DIR, "bin"))
            ce_ldflags.extend(
                f"-Wl,--delayload={ffmpeg_runtime_names[prefix]}" for prefix in ("avformat", "avcodec", "avutil")
            )
        if env.get("CE_DISABLE_LTO") != "1":
            ce_ldflags.append("-flto")
        if any(flag.startswith("-fsanitize=") for flag in cflags):
            ce_ldflags.append("-fsanitize=address,undefined")
        # Don't link mediaengine.dll on Linux - load dynamically instead
        if not IS_LINUX:
            ce_ldflags.append(me_lib)
        # Delay-load only works with MSYS2's lld on Windows
        # On Linux, we need to copy FFmpeg DLLs to main folder or use LoadLibrary
        if not IS_LINUX:
            ce_ldflags.append("-Wl,--delayload,mediaengine.dll")
            ce_ldflags.append("-ldelayimp")
        append_windows_pdb_linker_flag(ce_ldflags, ce_exe)
        # x64 common objects
        x64_common_objs = [
            os.path.join(OBJ_DIR, "x64", os.path.relpath(s, PROJECT_ROOT).replace(".cpp", ".o"))
            for s in glob.glob(os.path.join(PROJECT_ROOT, "common", "*.cpp"))
        ]
        temp_ce_exe = os.path.join(ce_obj_dir, "captureengine.tmp.exe")
        safe_delete_file(temp_ce_exe)
        cmd: List[str] = [clang_exe] + ce_objs + x64_common_objs + ce_ldflags + ["-o", temp_ce_exe]
        run_command(cmd, env=env)
        if not safe_copy_file(temp_ce_exe, ce_exe):
            log("ERROR: Failed to place captureengine.exe (destination may be locked)")
            sys.exit(1)
        safe_delete_file(temp_ce_exe)
        record_verification_artifact("captureengine_exe", ce_exe)

        stale_layer_register_exe = os.path.join(BIN_DIR, "vulkan_layer_register.exe")
        if os.path.exists(stale_layer_register_exe):
            if safe_delete_file(stale_layer_register_exe):
                log("Removed stale vulkan_layer_register.exe")

    # compile_custom_ffmpeg() already synchronized the complete runtime closure
    # before compilation. Re-verify that final bundle here without deleting and
    # recopying the same DLLs a second time.
    if not IS_LINUX:
        ffmpeg_bin_dst = os.path.join(BIN_DIR, "ffmpeg")
        objdump_exe = os.path.join(MSYS2_DIR, "clang64", "bin", "llvm-objdump.exe")
        verify_pe_import_closure(ffmpeg_bin_dst, objdump_exe, logger=log)
        remove_redundant_root_runtime_dlls(
            BIN_DIR,
            WINDOWS_FFMPEG_RUNTIME_DEPS + WINDOWS_FFMPEG_OPTIONAL_RUNTIME_DEPS,
        )
        if env.get("CE_SANITIZE") == "1":
            sync_windows_sanitizer_runtime_dlls(BIN_DIR)
        else:
            remove_stale_windows_sanitizer_runtime_dlls(BIN_DIR)

    # 6. Prepare FG SDK headers on every host and native runtime DLLs on Windows.
    setup_fg_sdk_for_host(skip_updates=skip_updates)

    # 7. Compile Test Applications (DX9/10/11/12, Vulkan, OpenGL; x64/x86)
    x86_env_for_tests = None
    if not IS_LINUX or has_linux_x86_compiler():
        x86_env_for_tests, _ = get_env_x86()
        propagate_build_control_environment(env, x86_env_for_tests)
    compile_testapps(env, x86_env_for_tests, clang_exe, cflags)

    # 8. Compile Vulkan Layer (VK_LAYER_CE_overlay)
    compile_vulkan_layer(env, clang_exe, cflags, "x64")
    record_verification_artifact("vulkan_layer_x64", os.path.join(BIN_DIR, "VK_LAYER_CE_overlay.dll"))
    # x86 layer using mingw32 toolchain (disabled for sanitizer builds)
    if get_env_x86 and env.get("CE_SANITIZE") != "1":
        if IS_LINUX and not has_linux_x86_compiler():
            log("Linux host: skipping x86 Vulkan layer (x86 compiler unavailable)")
        else:
            x86_env, x86_clang_bin = get_env_x86()
            propagate_build_control_environment(env, x86_env)
            x86_clang = get_compiler_exe("x86")
            # Check if x86 compiler exists (on Linux it might not)
            if x86_clang and (IS_LINUX or os.path.exists(x86_clang)):
                x86_cflags = make_cpp_cflags(
                    ["-O3"],
                    compiler_exe=x86_clang,
                    arch_flags=(
                        []
                        if IS_LINUX
                        else [
                            "--target=i686-w64-mingw32",
                            "--sysroot=" + os.path.join(MSYS2_DIR, "mingw32"),
                        ]
                    ),
                    enable_cfg=False,
                )
                compile_vulkan_layer(x86_env, x86_clang, x86_cflags, "x86")
    elif env.get("CE_SANITIZE") == "1":
        log("Sanitizer mode: skipping x86 Vulkan layer (ASan runtime unavailable)")

    # Cleanup import libraries (use safe delete for consistency)
    me_lib = os.path.join(BIN_DIR, "libmediaengine.dll.a")
    if os.path.exists(me_lib):
        if safe_delete_file(me_lib):
            log(f"Removed {me_lib}")
        # If it fails, it's not critical - just a .a file, not a loaded DLL

    # Copy License files
    log("Copying License files...")

    licenses_src = os.path.join(PROJECT_ROOT, "licenses")
    licenses_dst = os.path.join(BIN_DIR, "licenses")
    if os.path.exists(licenses_src):
        if os.path.exists(licenses_dst):
            shutil.rmtree(licenses_dst)
        shutil.copytree(licenses_src, licenses_dst)
        copy_bundled_runtime_licenses(licenses_dst, os.path.join(BIN_DIR, "ffmpeg"))
        log("Copied licenses/ directory to installed/captureengine/")

    assert_no_obsolete_process_loopback_helper_artifacts()

    # Keep runtime DLLs in tests/ current so unit_tests.exe can run directly
    tests_dir = TEST_OUTPUT_DIR
    if os.path.exists(os.path.join(tests_dir, "unit_tests.exe")):
        copy_test_runtime_dlls(tests_dir)

    pe_hardening_verifier = os.path.join(PROJECT_ROOT, "tools", "verify_pe_hardening.py")
    llvm_readobj = get_llvm_readobj_exe()
    pe_verifier_host_flags: List[str] = []
    if not compiler_supports_windows_cfg(clang_exe):
        # Debian/Ubuntu MinGW GCC cannot emit Clang/LLD's Windows CFG metadata.
        # Defer only CFG while retaining every other PE mitigation and import
        # check for both first-party binaries and the packaged runtime closure.
        pe_verifier_host_flags.append("--allow-missing-x64-cfg")
    if not IS_WINDOWS:
        # Linux cross-builds retain DWARF in the PE images instead of producing
        # native CodeView/PDB sidecars.
        pe_verifier_host_flags.append("--allow-missing-pdb")
    pe_verifier_command = [
        sys.executable,
        pe_hardening_verifier,
        "--llvm-readobj",
        llvm_readobj,
        "--root",
        BIN_DIR,
        # The current clang64 -> mingw32 cross-linker marks x86 images for CFG
        # but cannot populate their target tables. Keep all other x86 PE checks
        # active while effective x86 CFG is deferred.
        "--allow-missing-x86-cfg",
    ] + pe_verifier_host_flags
    if IS_LINUX:
        pe_verifier_command.append("--skip-ffmpeg-imports")
    if env.get("CE_SANITIZE") == "1":
        # The sanitizer pass intentionally produces only x64 developer artifacts.
        # Ignore stale x86 outputs from the preceding product build and recognize
        # the toolchain-resolved runtimes. They remain subject to PE hardening
        # and import-closure checks, but are not first-party PDB-bearing files.
        pe_verifier_command.extend(
            [
                "--skip-x86",
                "--allow-runtime-dll",
                "libclang_rt.asan_dynamic-x86_64.dll",
                "--allow-runtime-dll",
                "libc++.dll",
            ]
        )
    run_command(
        pe_verifier_command,
        cwd=PROJECT_ROOT,
        env=env,
    )
    if env.get("CE_SANITIZE") != "1":
        for testapp_root in (TESTAPP_BIN_DIR, os.path.join(TESTAPP_BIN_DIR, "x86")):
            run_command(
                [
                    sys.executable,
                    pe_hardening_verifier,
                    "--llvm-readobj",
                    llvm_readobj,
                    "--root",
                    testapp_root,
                    "--executables-only",
                    "--allow-missing-x86-cfg",
                ]
                + pe_verifier_host_flags,
                cwd=PROJECT_ROOT,
                env=env,
            )
    log("Verified PE mitigations, architecture, section permissions, effective CFG, imports, and PDBs")

    clear_stale_hook_pdb_cache()

    log("Build Complete.")
