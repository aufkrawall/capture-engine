# Setup program and uninstaller: native Win32/GDI executables that import only
# Windows system DLLs. The uninstaller ships inside the setup payload; the setup
# executable is the stub plus the packaged program files (tools/installer_payload.py).

INSTALLER_SOURCE_DIR = os.path.join(PROJECT_ROOT, "installer")
INSTALLER_OUTPUT_DIR = os.path.join(BUILD_DIR, "installer")
SETUP_PACKAGE_NAME = "captureengine-setup.exe"
INSTALLER_STUB_NAME = "captureengine_setup_stub.exe"
INSTALLER_UNINSTALLER_NAME = "captureengine_uninstall.exe"

# Both binaries share these units; payload decoding and the install engine are
# compiled into the setup stub only. An uninstaller that statically carries a
# payload reader and an install orchestrator looks like a dropper to
# reputation heuristics even when none of it runs, so it is kept out of the image.
INSTALLER_COMMON_SOURCES = (
    "main.cpp",
    "util.cpp",
    "version.cpp",
    "process_control.cpp",
    "registration.cpp",
    "shortcuts.cpp",
    "integration.cpp",
    "files.cpp",
    "engine_uninstall.cpp",
    "ui_theme.cpp",
    "ui_pages.cpp",
    "ui_results.cpp",
    "ui_wizard.cpp",
)
INSTALLER_SETUP_ONLY_SOURCES = ("payload.cpp", "engine_install.cpp", "ui_preview.cpp")
# dwmapi, uxtheme and cabinet are loaded at run time; nothing else is imported.
INSTALLER_LINK_LIBS = ["-luser32", "-lgdi32", "-ladvapi32", "-lshell32", "-lole32", "-luuid", "-luserenv"]

# Every payload must carry these, or the installed program is incomplete.
INSTALLER_REQUIRED_MEMBERS = (
    "captureengine.exe",
    "captureengine_elevation_service.exe",
    "captureengine_uninstall.exe",
    "mediaengine.dll",
    "capture_hook_x64.dll",
    "config.ini",
    "plugins/LibreHardwareMonitor/PawnIO_setup.exe",
)

# Strings that only the install path may contain. Finding one in the
# uninstaller means a shared unit started pulling install code back in.
INSTALLER_UNINSTALLER_FORBIDDEN = (
    "CESETUP1",
    "cabinet.dll",
    "CreateDecompressor",
)


def installer_sources(uninstaller: bool) -> List[str]:
    names = INSTALLER_COMMON_SOURCES + (() if uninstaller else INSTALLER_SETUP_ONLY_SOURCES)
    return [os.path.join(INSTALLER_SOURCE_DIR, name) for name in names]


def _compile_installer_binary(env, clang_exe, cflags, uninstaller: bool) -> str:
    variant = "uninstall" if uninstaller else "setup"
    defines = ["-DCE_UNINSTALLER=1"] if uninstaller else []
    pairs = []
    for source in installer_sources(uninstaller):
        obj = os.path.join(OBJ_DIR, "installer", variant, os.path.splitext(os.path.basename(source))[0] + ".o")
        pairs.append((source, obj.replace("\\", "/")))
    parallel_compile(env, clang_exe, cflags + defines, pairs)
    resource = os.path.join(OBJ_DIR, "installer", variant, "setup.res.o")
    os.makedirs(os.path.dirname(resource), exist_ok=True)
    run_command(
        [get_windres_exe("x64")] + defines + [os.path.join(INSTALLER_SOURCE_DIR, "setup.rc"), "-o", resource],
        env=env,
        cwd=INSTALLER_SOURCE_DIR,
    )
    os.makedirs(INSTALLER_OUTPUT_DIR, exist_ok=True)
    output = os.path.join(INSTALLER_OUTPUT_DIR, INSTALLER_UNINSTALLER_NAME if uninstaller else INSTALLER_STUB_NAME)
    flags = ["-mwindows", "-municode", "-static", "-static-libgcc", "-static-libstdc++"]
    flags += LD_OPT_FLAGS + get_x64_linker_flags(clang_exe) + INSTALLER_LINK_LIBS
    if env.get("CE_DISABLE_LTO") != "1":
        flags.append("-flto")
    append_windows_pdb_linker_flag(flags, output)
    temporary = os.path.join(OBJ_DIR, "installer", variant, "link.tmp.exe")
    safe_delete_file(temporary)
    run_command([clang_exe] + [obj for _, obj in pairs] + [resource] + flags + ["-o", temporary], env=env)
    if not safe_copy_file(temporary, output):
        raise RuntimeError(f"Cannot place {os.path.basename(output)} (destination may be locked)")
    safe_delete_file(temporary)
    return output


def verify_uninstaller_surface(path: str) -> None:
    """The removal-only image must not contain the payload reader or install text."""
    with open(path, "rb") as handle:
        data = handle.read()
    found = [
        needle
        for needle in INSTALLER_UNINSTALLER_FORBIDDEN
        if needle.encode("ascii") in data or needle.encode("utf-16le") in data
    ]
    if found:
        raise RuntimeError(f"{os.path.basename(path)} contains install-only code or text: {', '.join(found)}")
    if b"captureengine_uninstall" not in data and "captureengine_uninstall".encode("utf-16le") not in data:
        raise RuntimeError(f"{os.path.basename(path)} lost its uninstaller identity")


def compile_installer(env, clang_exe, cflags) -> None:
    if not IS_WINDOWS:
        log("Skipping the installer: it is a Windows-only target")
        return
    if env.get("CE_SANITIZE") == "1" or ISOLATED_BUILD_ROOT:
        log("Skipping the installer for isolated/sanitizer validation")
        return
    log("Compiling setup program and uninstaller x64...")
    uninstaller = _compile_installer_binary(env, clang_exe, cflags, True)
    stub = _compile_installer_binary(env, clang_exe, cflags, False)
    verify_uninstaller_surface(uninstaller)
    # The images must not carry the developer's profile path; PDB references are
    # already bare names (append_windows_pdb_linker_flag), so any hit is a regression.
    for binary in (uninstaller, stub):
        with open(binary, "rb") as handle:
            hits = count_profile_path_hits(handle.read())
        if hits:
            raise RuntimeError(f"privacy scan found the developer profile path in {binary}")
    run_command(
        [
            sys.executable,
            os.path.join(PROJECT_ROOT, "tools", "verify_pe_hardening.py"),
            "--llvm-readobj",
            get_llvm_readobj_exe(),
            "--root",
            INSTALLER_OUTPUT_DIR,
            "--executables-only",
            "--allow-missing-x86-cfg",
        ],
        cwd=PROJECT_ROOT,
        env=env,
    )
    record_verification_artifact("installer_uninstaller_exe", uninstaller)
    record_verification_artifact("installer_setup_stub_exe", stub)


def assemble_setup_executable(staged_root: str, output_path: str) -> str:
    """Append the staged program files and the uninstaller to the setup stub."""
    from tools import installer_payload

    stub_path = os.path.join(INSTALLER_OUTPUT_DIR, INSTALLER_STUB_NAME)
    uninstaller_path = os.path.join(INSTALLER_OUTPUT_DIR, INSTALLER_UNINSTALLER_NAME)
    for path in (stub_path, uninstaller_path):
        if not os.path.isfile(path):
            raise RuntimeError(f"Cannot assemble the setup program: {os.path.basename(path)} was not built")
    files: List[Tuple[str, bytes]] = []
    for current, directories, names in os.walk(staged_root):
        directories.sort()
        for name in sorted(names):
            source = os.path.join(current, name)
            if os.path.islink(source):
                raise RuntimeError(f"Refusing to package a symlink: {source}")
            relative = os.path.relpath(source, staged_root).replace("\\", "/")
            with open(source, "rb") as handle:
                files.append((relative, handle.read()))
    with open(uninstaller_path, "rb") as handle:
        files.append((INSTALLER_UNINSTALLER_NAME, handle.read()))
    present = {path for path, _ in files}
    missing = [member for member in INSTALLER_REQUIRED_MEMBERS if member not in present]
    if missing:
        raise RuntimeError("Cannot assemble the setup program: payload is missing " + ", ".join(missing))
    files.sort(key=lambda item: item[0])
    with open(stub_path, "rb") as handle:
        stub = handle.read()
    data = installer_payload.assemble(stub, files)
    # Decode everything with the independent reader before the file is published.
    installer_payload.verify(data, files)
    os.makedirs(os.path.dirname(output_path), exist_ok=True)
    temporary = output_path + ".tmp"
    with open(temporary, "wb") as handle:
        handle.write(data)
    os.replace(temporary, output_path)
    log(f"Packaged setup program: {output_path} ({len(data)} bytes, {len(files)} files)")
    return output_path
