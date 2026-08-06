# _finalize_project_build: phases 6-8 of the product build (FG SDK headers/runtime,
# test apps, vulkan layer) plus license copying, PE hardening verification and
# packaging. Extracted from compile_project so every unit stays a semantic size.
def _finalize_project_build(env, clang_exe, cflags, skip_updates) -> None:
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

    licenses_src = os.path.join(PROJECT_ROOT, "tools", "licenses")
    licenses_dst = os.path.join(BIN_DIR, "licenses")
    if os.path.exists(licenses_src):
        if os.path.exists(licenses_dst):
            shutil.rmtree(licenses_dst)
        shutil.copytree(licenses_src, licenses_dst)
        copy_bundled_runtime_licenses(licenses_dst, os.path.join(BIN_DIR, "ffmpeg"))
        log("Copied license files to installed/captureengine/")

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

    if env.get("CE_SANITIZE") == "1" or ISOLATED_BUILD_ROOT:
        log("Skipping release archives for isolated/sanitizer validation")
    else:
        package_build_outputs()

    log("Build Complete.")

