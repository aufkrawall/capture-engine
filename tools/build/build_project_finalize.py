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

    # Install only CaptureEngine's first-party LHM setup text. The bridge itself
    # is compiled into captureengine.exe, so nothing executable is placed here.
    # Never clear this directory: users place their separately licensed LHM files
    # here.
    lhm_support_src = os.path.join(PROJECT_ROOT, "plugins", "LibreHardwareMonitor")
    lhm_support_dst = os.path.join(BIN_DIR, "plugins", "LibreHardwareMonitor")
    os.makedirs(lhm_support_dst, exist_ok=True)
    for filename in ("README.txt",):
        source = os.path.join(lhm_support_src, filename)
        destination = os.path.join(lhm_support_dst, filename)
        if not safe_copy_file(source, destination):
            raise RuntimeError(f"Failed to install LibreHardwareMonitor bridge support file: {filename}")
    log("Copied optional LibreHardwareMonitor setup notes")

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

    if IS_WINDOWS and env.get("CE_SANITIZE") != "1" and not ISOLATED_BUILD_ROOT:
        scrub_and_verify_privacy_paths()

    clear_stale_hook_pdb_cache()

    if env.get("CE_SANITIZE") == "1" or ISOLATED_BUILD_ROOT:
        log("Skipping release archives for isolated/sanitizer validation")
    elif env.get("CE_SKIP_PACKAGE") == "1":
        log("Skipping release archives (--skip-package)")
        record_verification_step("package_archives", "skipped", details={"reason": "--skip-package"})
    # Release archives are created by build_cli concurrently with the advisory
    # lint pass (see should_package_outputs); the finalize phase must not
    # serialize them in front of lint.

    log("Build Complete.")


def scrub_and_verify_privacy_paths() -> None:
    """Scrub the developer profile root from shipped PDBs and fail if any shipped
    artifact retains it, so release artifacts never leak the maintainer's Windows
    user name (UTF-8 and UTF-16LE spellings). First-party images must be clean
    via /pdbaltpath (a regression stays loud); PDBs and third-party FFmpeg
    closure DLLs (which embed local recipe build paths) are scrubbed in place."""
    if not profile_path_spellings():
        return
    # Mirror the binary packaging boundary. The plugins directory has a separate
    # text-only allowlist in build_packaging.py, and tracked-source privacy tests
    # cover those two first-party support files without reading user-supplied DLLs.
    # Elsewhere bak/captures/logs/screenshots and excluded suffixes stay local.
    excluded_dirs = {"bak", "captures", "logs", "screenshots"}
    excluded_suffixes = (".csv", ".dmp", ".link-cache.json", ".log", ".tmp")
    included_root_dirs = {"ffmpeg", "licenses"}
    packaged_x64_testapps = set(PACKAGED_X64_TEST_APPS)
    packaged_x86_testapps = set(PACKAGED_X86_TEST_APPS)
    targets: List[str] = []
    for root in (BIN_DIR, TESTAPP_BIN_DIR):
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            relative_directory = os.path.relpath(dirpath, root)
            kept_directories = []
            for directory in dirnames:
                lower = directory.lower()
                if lower in excluded_dirs or os.path.islink(os.path.join(dirpath, directory)):
                    continue
                if root == BIN_DIR and relative_directory == "." and lower not in included_root_dirs:
                    continue
                kept_directories.append(directory)
            dirnames[:] = kept_directories
            for filename in filenames:
                lower = filename.lower()
                if lower.endswith(excluded_suffixes):
                    continue
                if lower in {"config.ini", "nul"} or ".old." in lower:
                    continue
                if root == TESTAPP_BIN_DIR:
                    testapp_name = os.path.splitext(filename)[0]
                    packaged_names = (
                        packaged_x86_testapps
                        if os.path.basename(dirpath).lower() == "x86"
                        else packaged_x64_testapps
                    )
                    if testapp_name not in packaged_names:
                        continue
                if lower.endswith((".dll", ".exe", ".pdb")):
                    targets.append(os.path.join(dirpath, filename))
    targets = sorted(set(targets))
    scrubbed = 0
    for target in targets:
        with open(target, "rb") as handle:
            data = handle.read()
        hits = count_profile_path_hits(data)
        if hits:
            relative = (
                os.path.relpath(target, BIN_DIR) if target.startswith(BIN_DIR + os.sep) else target
            )
            is_scrubbable = target.lower().endswith(".pdb") or relative.lower().startswith(
                "ffmpeg" + os.sep
            )
            if not is_scrubbable:
                raise RuntimeError(
                    f"privacy scan found developer profile path in {target}; "
                    "PE PDB references must embed a bare filename (/pdbaltpath)"
                )
            scrubbed_data = scrub_profile_path_bytes(data)
            if len(scrubbed_data) != len(data):
                raise RuntimeError(f"privacy scrub changed byte length of {target}")
            with open(target, "wb") as handle:
                handle.write(scrubbed_data)
            scrubbed += 1
            log(f"Scrubbed {hits} profile-path occurrence(s) from {target}")
            if count_profile_path_hits(scrubbed_data):
                raise RuntimeError(f"privacy scan still finds developer profile path in {target}")
            data = scrubbed_data
        # The machine name is verified, never scrubbed: nothing in the toolchain
        # should embed host state, so a hit is a new leak source to identify
        # rather than a known spelling to rewrite.
        machine_hits = count_machine_name_hits(data)
        if machine_hits:
            raise RuntimeError(
                f"privacy scan found the build machine name {machine_hits} time(s) in {target}; "
                "no build step should embed host state - find what recorded it "
                "instead of scrubbing the artifact"
            )
    machine_skip = machine_name_scan_skip_reason()
    if machine_skip:
        log(f"Privacy scan: machine-name check skipped ({machine_skip})")
    log(f"Privacy scan clean: {len(targets)} shipped binaries/PDBs, {scrubbed} scrubbed")
