

def init_verification_context(args: List[str], build_number: int, verify_mode: bool, top_level: bool) -> None:
    global DETAIL_LOG_FILE, VERIFICATION_CONTEXT, VERIFICATION_FINAL_EXIT_CODE, VERIFICATION_ATEXIT_REGISTERED

    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    run_dir = os.path.join(VERIFICATION_DIR, f"{timestamp}_build_{build_number}")
    os.makedirs(run_dir, exist_ok=True)
    if not DETAIL_LOG_FILE:
        DETAIL_LOG_FILE = os.path.join(run_dir, "build.details.log")
    assert DETAIL_LOG_FILE is not None
    if os.path.abspath(DETAIL_LOG_FILE) != os.path.abspath(LOG_FILE) and not os.path.exists(DETAIL_LOG_FILE):
        try:
            if os.path.exists(LOG_FILE):
                shutil.copy2(LOG_FILE, DETAIL_LOG_FILE)
        except OSError:
            pass

    VERIFICATION_CONTEXT = {
        "run_dir": os.path.abspath(run_dir),
        "top_level": top_level,
        "mode": "verify" if verify_mode else "build",
        "build_number": build_number,
        "build_version": f"0.1.{build_number}",
        "build_script_sha256": sha256_file(os.path.abspath(__file__)),
        "command": [sys.executable, os.path.abspath(__file__), *args],
        "args": list(args),
        "start_time": datetime.datetime.now().isoformat(timespec="milliseconds"),
        "success": None,
        "exit_code": None,
        "steps": {},
        "coverage": {},
        "artifacts": {
            "live_build_log": os.path.abspath(LOG_FILE),
            "detailed_build_log": os.path.abspath(DETAIL_LOG_FILE),
        },
    }
    VERIFICATION_FINAL_EXIT_CODE = 1

    if not VERIFICATION_ATEXIT_REGISTERED:
        atexit.register(finalize_verification_on_exit)
        VERIFICATION_ATEXIT_REGISTERED = True


def finalize_verification_on_exit() -> None:
    global VERIFICATION_FINALIZED

    if VERIFICATION_FINALIZED or not VERIFICATION_CONTEXT:
        return
    VERIFICATION_FINALIZED = True

    success = VERIFICATION_FINAL_EXIT_CODE == 0
    VERIFICATION_CONTEXT["success"] = success
    VERIFICATION_CONTEXT["exit_code"] = VERIFICATION_FINAL_EXIT_CODE
    VERIFICATION_CONTEXT["end_time"] = datetime.datetime.now().isoformat(timespec="milliseconds")

    start_ts = VERIFICATION_CONTEXT.get("start_time")
    try:
        if not isinstance(start_ts, str):
            raise ValueError("verification start_time missing")
        start_dt = datetime.datetime.fromisoformat(start_ts)
        end_ts = VERIFICATION_CONTEXT.get("end_time")
        if not isinstance(end_ts, str):
            raise ValueError("verification end_time missing")
        end_dt = datetime.datetime.fromisoformat(end_ts)
        VERIFICATION_CONTEXT["duration_seconds"] = round((end_dt - start_dt).total_seconds(), 3)
    except Exception:
        pass

    for step in VERIFICATION_CONTEXT.get("steps", {}).values():
        if step.get("status") == "running":
            step["status"] = "passed" if success else "failed"

    run_dir = VERIFICATION_CONTEXT["run_dir"]
    manifest_path = os.path.join(run_dir, "verification_manifest.json")
    summary_path = os.path.join(run_dir, "verification_summary.txt")
    build_log_copy = os.path.join(run_dir, "build.log")

    try:
        if os.path.exists(LOG_FILE):
            shutil.copy2(LOG_FILE, build_log_copy)
            VERIFICATION_CONTEXT["artifacts"]["build_log"] = os.path.abspath(build_log_copy)
    except Exception:
        VERIFICATION_CONTEXT["artifacts"]["build_log"] = os.path.abspath(LOG_FILE)

    summary_lines = [
        f"mode={VERIFICATION_CONTEXT['mode']}",
        f"success={1 if success else 0}",
        f"exit_code={VERIFICATION_FINAL_EXIT_CODE}",
        f"build_version={VERIFICATION_CONTEXT['build_version']}",
        f"run_dir={run_dir}",
        f"live_build_log={VERIFICATION_CONTEXT['artifacts'].get('live_build_log', '')}",
        f"duration_seconds={VERIFICATION_CONTEXT.get('duration_seconds', '')}",
    ]
    if VERIFICATION_CONTEXT.get("artifacts", {}).get("build_log"):
        summary_lines.append(f"build_log_copy={VERIFICATION_CONTEXT['artifacts']['build_log']}")

    for step_name, step in VERIFICATION_CONTEXT.get("steps", {}).items():
        duration = step.get("duration_seconds")
        duration_suffix = f" ({duration:.3f}s)" if isinstance(duration, (int, float)) else ""
        summary_lines.append(f"step.{step_name}={step.get('status', 'unknown')}{duration_suffix}")

    for coverage_name, coverage_value in VERIFICATION_CONTEXT.get("coverage", {}).items():
        summary_lines.append(f"coverage.{coverage_name}={coverage_value}")

    for artifact_name, artifact_path in VERIFICATION_CONTEXT.get("artifacts", {}).items():
        summary_lines.append(f"artifact.{artifact_name}={artifact_path}")

    try:
        write_json_atomic(manifest_path, VERIFICATION_CONTEXT)
        write_text_atomic(summary_path, "\n".join(summary_lines) + "\n")

        if VERIFICATION_CONTEXT.get("top_level"):
            os.makedirs(VERIFICATION_DIR, exist_ok=True)
            # The latest_* pair is uploaded as a release asset; emit it with the
            # developer profile root redacted so release artifacts never leak
            # the maintainer's Windows user name.
            write_json_atomic(
                os.path.join(VERIFICATION_DIR, "latest_manifest.json"),
                sanitize_privacy_values(VERIFICATION_CONTEXT),
            )
            write_text_atomic(
                os.path.join(VERIFICATION_DIR, "latest_summary.txt"),
                sanitize_privacy_paths("\n".join(summary_lines) + "\n"),
            )
            write_text_atomic(os.path.join(VERIFICATION_DIR, "latest_run_dir.txt"), run_dir + "\n")
            if os.path.exists(build_log_copy):
                shutil.copy2(build_log_copy, os.path.join(VERIFICATION_DIR, "latest_build.log"))
            if DETAIL_LOG_FILE and os.path.exists(DETAIL_LOG_FILE):
                shutil.copy2(DETAIL_LOG_FILE, os.path.join(VERIFICATION_DIR, "latest_build.details.log"))
    except Exception:
        pass


def safe_extract_tar(archive: tarfile.TarFile, destination: str) -> None:
    """Safely extract tar archive, preventing path traversal."""
    dest_abs = os.path.normcase(os.path.abspath(destination))
    for member in archive.getmembers():
        member_abs = os.path.normcase(os.path.abspath(os.path.join(dest_abs, member.name)))
        if member_abs != dest_abs and not member_abs.startswith(dest_abs + os.sep):
            raise RuntimeError(f"Unsafe archive member path: {member.name}")
    archive.extractall(dest_abs, filter="data")


def is_virtual_environment() -> bool:
    return getattr(sys, "base_prefix", sys.prefix) != sys.prefix or hasattr(sys, "real_prefix")


def should_bootstrap_python_tools(
    default_quality_mode: bool, verify_flag: bool, lint_flag: bool, format_flag: bool
) -> bool:
    return default_quality_mode or verify_flag or lint_flag or format_flag


def _url_exists(url: str) -> bool:
    request = urllib.request.Request(url, method="HEAD", headers={"User-Agent": "Mozilla/5.0"})
    try:
        with urllib.request.urlopen(request, timeout=30):
            return True
    except Exception:
        return False


def download_verified_url(url: str, destination: str) -> None:
    temporary_path = destination + ".tmp"
    try:
        with urllib.request.urlopen(url, timeout=180) as response:
            with open(temporary_path, "wb") as output_file:
                shutil.copyfileobj(response, output_file)
        os.replace(temporary_path, destination)
    finally:
        if os.path.exists(temporary_path):
            os.remove(temporary_path)


def _bootstrap_gpg_context() -> tuple[str, Dict[str, str]]:
    gpg_candidates = [shutil.which("gpg.exe"), shutil.which("gpg")]
    if os.path.exists(os.path.join(MSYS2_DIR, "usr", "bin", "gpg.exe")):
        gpg_candidates.append(os.path.join(MSYS2_DIR, "usr", "bin", "gpg.exe"))
    gpg_exe = next((candidate for candidate in gpg_candidates if candidate), None)
    if not gpg_exe:
        raise RuntimeError(
            "A trusted host GPG executable is required to verify the MSYS2 bootstrap archive; "
            "install GnuPG before creating a fresh MSYS2 tree"
        )

    keyring_dir = os.path.join(BUILD_DIR, "msys2-bootstrap-gnupg")
    os.makedirs(keyring_dir, exist_ok=True)
    env = os.environ.copy()
    if os.path.normcase(os.path.abspath(gpg_exe)).startswith(os.path.normcase(os.path.abspath(MSYS2_DIR))):
        env["GNUPGHOME"] = to_unix(keyring_dir)
    else:
        env["GNUPGHOME"] = keyring_dir
    return gpg_exe, env


def ensure_msys2_bootstrap_key() -> tuple[str, Dict[str, str]]:
    gpg_exe, env = _bootstrap_gpg_context()

    def has_key() -> bool:
        result = subprocess.run(
            [gpg_exe, "--batch", "--with-colons", "--list-keys", MSYS2_BOOTSTRAP_PGP_KEY],
            env=env,
            capture_output=True,
            text=True,
        )
        return MSYS2_BOOTSTRAP_PGP_KEY.lower() in result.stdout.lower()

    if not has_key():
        imported = False
        for keyserver in MSYS2_BOOTSTRAP_KEY_SERVERS:
            log(f"Retrieving MSYS2 bootstrap signing key {MSYS2_BOOTSTRAP_PGP_KEY} from {keyserver}")
            result = subprocess.run(
                [
                    gpg_exe,
                    "--batch",
                    "--keyserver",
                    keyserver,
                    "--recv-keys",
                    MSYS2_BOOTSTRAP_PGP_KEY,
                ],
                env=env,
                capture_output=True,
                text=True,
            )
            if result.returncode == 0 and has_key():
                imported = True
                break
            if result.stderr:
                log(f"MSYS2 bootstrap key lookup failed: {result.stderr.strip()}")
        if not imported:
            raise RuntimeError(
                "Could not retrieve and fingerprint-verify the MSYS2 bootstrap signing key "
                f"{MSYS2_BOOTSTRAP_PGP_KEY}"
            )
    return gpg_exe, env


def verify_msys2_bootstrap_archive(archive_path: str, archive_url: str) -> None:
    signature_path = archive_path + ".sig"
    signature_url = archive_url + ".sig"
    if not os.path.exists(signature_path):
        log(f"Downloading MSYS2 bootstrap signature: {signature_url}")
        download_verified_url(signature_url, signature_path)
    gpg_exe, env = ensure_msys2_bootstrap_key()
    try:
        verify_detached_signature(gpg_exe, archive_path, signature_path, [MSYS2_BOOTSTRAP_PGP_KEY], env)
    except Exception:
        log("Cached MSYS2 bootstrap signature was not valid; downloading a fresh sidecar")
        download_verified_url(signature_url, signature_path)
        verify_detached_signature(gpg_exe, archive_path, signature_path, [MSYS2_BOOTSTRAP_PGP_KEY], env)
    log(f"Verified MSYS2 bootstrap archive signature with {MSYS2_BOOTSTRAP_PGP_KEY}")


def resolve_msys2_url() -> str:
    override = os.environ.get("CE_MSYS2_URL", "").strip()
    if override:
        log(f"Using MSYS2 base archive override: {override}")
        return override

    request = urllib.request.Request(MSYS2_DIST_URL, headers={"User-Agent": "Mozilla/5.0"})
    try:
        with urllib.request.urlopen(request, timeout=30) as response:
            html = response.read().decode("utf-8", errors="replace")
        matches = re.findall(r'href="(msys2-base-x86_64-(\d{8})\.tar\.xz)"', html)
        if matches:
            latest_name, _ = max(matches, key=lambda item: item[1])
            resolved_url = MSYS2_DIST_URL + latest_name
            log(f"Resolved latest MSYS2 base archive: {latest_name}")
            return resolved_url
    except Exception as error:
        log(f"Could not resolve the latest MSYS2 base archive: {error}")

    fallback_url = MSYS2_DIST_URL + MSYS2_DEFAULT_TARBALL
    if _url_exists(fallback_url):
        log(f"Using pinned MSYS2 bootstrap fallback: {MSYS2_DEFAULT_TARBALL}")
        return fallback_url
    raise RuntimeError(f"Could not resolve a downloadable MSYS2 base archive from {MSYS2_DIST_URL}")


def verify_msys2_ffmpeg_build_deps(msys2_dir: str) -> None:
    pkg_config_exe = (
        shutil.which("pkg-config") if IS_LINUX else os.path.join(msys2_dir, "clang64", "bin", "pkg-config.exe")
    )
    if not pkg_config_exe or (not IS_LINUX and not os.path.exists(pkg_config_exe)):
        raise RuntimeError("Missing pkg-config executable required for FFmpeg dependency probing")

    clang_bin = os.path.join(msys2_dir, "clang64", "bin")
    usr_bin = os.path.join(msys2_dir, "usr", "bin")
    clang_pkgconfig = os.path.join(msys2_dir, "clang64", "lib", "pkgconfig")
    probe_env = os.environ.copy()

    path_entries = []
    for candidate in (clang_bin, usr_bin):
        if os.path.isdir(candidate):
            path_entries.append(candidate)
    if path_entries:
        probe_env["PATH"] = os.pathsep.join(path_entries + [probe_env.get("PATH", "")])
    probe_env["PKG_CONFIG_PATH"] = os.pathsep.join([clang_pkgconfig, probe_env.get("PKG_CONFIG_PATH", "")]).rstrip(
        os.pathsep
    )

    required_pkg_configs = (
        [
            ("vpl", "mingw-w64-clang-x86_64-onevpl"),
            ("SvtAv1Enc", "mingw-w64-clang-x86_64-svt-av1"),
            ("opus", "mingw-w64-clang-x86_64-opus"),
        ]
        if IS_LINUX
        else []
    )
    required_headers = [
        (
            os.path.join("spirv", "unified1", "spirv.h"),
            "mingw-w64-clang-x86_64-spirv-headers",
        ),
    ]

    missing = []
    if not IS_LINUX:
        required_tools = [
            os.path.join(usr_bin, "makepkg"),
            os.path.join(usr_bin, "makepkg-mingw"),
            os.path.join(usr_bin, "bash.exe"),
            os.path.join(clang_bin, "cmake.exe"),
            os.path.join(clang_bin, "meson.exe"),
            os.path.join(clang_bin, "ninja.exe"),
            os.path.join(clang_bin, "llvm-objdump.exe"),
        ]
        missing.extend((tool, "MSYS2 source-build tooling") for tool in required_tools if not os.path.exists(tool))

        pacman_exe = os.path.join(usr_bin, "pacman.exe")
        required_toolchain_packages = [
            "mingw-w64-clang-x86_64-clang",
            "mingw-w64-clang-x86_64-llvm-tools",
            "mingw-w64-clang-x86_64-compiler-rt",
        ]
        for package_name in required_toolchain_packages:
            package_result = subprocess.run(
                [pacman_exe, "-Q", package_name],
                capture_output=True,
                text=True,
            )
            if package_result.returncode != 0:
                missing.append((package_name, "LLVM 22.1.8-compatible MSYS2 toolchain"))
                continue
            version_match = re.search(r"\s(\d+\.\d+\.\d+)-", package_result.stdout)
            if not version_match or version_match.group(1) != "22.1.8":
                installed = package_result.stdout.strip() or "unknown version"
                missing.append((installed, "LLVM 22.1.8-compatible MSYS2 toolchain"))

    for pkg_name, package_name in required_pkg_configs:
        result = subprocess.run(
            [pkg_config_exe, "--exists", pkg_name],
            env=probe_env,
            capture_output=True,
            text=True,
        )
        if result.returncode != 0:
            missing.append((pkg_name, package_name))

    for header_rel_path, package_name in required_headers:
        header_path = os.path.join(msys2_dir, "clang64", "include", header_rel_path)
        if not os.path.exists(header_path):
            missing.append((header_rel_path, package_name))

    if missing:
        missing_summary = ", ".join(f"{label} ({package_name})" for label, package_name in missing)
        raise RuntimeError(f"Missing MSYS2 FFmpeg build dependencies: {missing_summary}")


def patch_amf_header():
    """Patch AMF SDK header to fix extern C with C++ types.

    The AMF SDK's DisplayCapture.h has a function declaration that uses C++ types
    (amf:: namespace) inside an extern C block. This causes C compilers to fail
    because the C++ types cannot be parsed as C. This patch hides the entire
    function from C compilers by wrapping it with ifdef __cplusplus.
    """
    if IS_LINUX:
        return  # Not needed on Linux - uses prebuilt FFmpeg

    amf_header = os.path.join(MSYS2_DIR, "clang64", "include", "AMF", "components", "DisplayCapture.h")

    if not os.path.exists(amf_header):
        log("[AMF] Header not found - skipping patch")
        return

    with open(amf_header, "r") as f:
        content = f.read()

    # Check if already patched (look for the correct pattern)
    if "#ifdef __cplusplus" in content and 'extern "C"' in content and "AMFCreateComponentDisplayCapture" in content:
        # Check if properly structured (extern C inside ifdef)
        import re

        if re.search(r'#ifdef __cplusplus\s*\nextern "C"\s*\n\s*\{', content):
            log("[AMF] Header already patched - skipping")
            return

    import re

    # Find the extern "C" block with AMFCreateComponentDisplayCapture
    pattern = r'extern\s+"C"\s*\{([^}]*AMFCreateComponentDisplayCapture[^}]*)\}'
    match = re.search(pattern, content, re.DOTALL)

    if match:
        inner = match.group(1).strip()
        # Wrap entire block with __cplusplus (hiding C++ types from C)
        new_block = '#ifdef __cplusplus\nextern "C"\n{\n    ' + inner + "\n}\n#endif"
        patched = content[: match.start()] + new_block + content[match.end() :]

        with open(amf_header, "w") as f:
            f.write(patched)
        log("[AMF] Patched DisplayCapture.h - hid C++ function from C compilers")
    else:
        log("[AMF] Warning: Could not find extern C block to patch")


# =============================================================================
# Locked File Handling Utilities
# =============================================================================


def is_file_locked(filepath: str) -> bool:
    """Check if a file is locked by another process on Windows."""
    if not os.path.exists(filepath):
        return False

    try:
        # Try to open with exclusive access (deny all sharing)
        # If this fails, the file is locked
        import ctypes

        GENERIC_READ = 0x80000000
        GENERIC_WRITE = 0x40000000
        OPEN_EXISTING = 3
        FILE_ATTRIBUTE_NORMAL = 0x80

        kernel32 = getattr(ctypes, "windll").kernel32

        # Convert to wide string for CreateFileW
        filepath_w = ctypes.c_wchar_p(filepath)

        # Try to open with no sharing (0 = share none)
        handle = kernel32.CreateFileW(
            filepath_w,
            GENERIC_READ | GENERIC_WRITE,
            0,  # dwShareMode - 0 means exclusive access
            None,  # lpSecurityAttributes
            OPEN_EXISTING,
            FILE_ATTRIBUTE_NORMAL,
            None,  # hTemplateFile
        )

        if handle == -1:  # INVALID_HANDLE_VALUE
            return True  # File is locked

        # Success - file is not locked, close it
        kernel32.CloseHandle(handle)
        return False

    except Exception:
        # If we can't determine, assume it's locked to be safe
        return True


def find_process_locking_file(filepath: str) -> List[str]:
    """Try to find which process(es) have a file locked using handle.exe or Resource Monitor."""
    processes = []

    # Try using Resource Monitor (resmon) query via WMI
    try:
        import subprocess

        # Use handle.exe from Sysinternals if available
        result = subprocess.run(["handle.exe", filepath], capture_output=True, text=True, timeout=5)
        if result.returncode == 0:
            # Parse handle.exe output
            for line in result.stdout.split("\n"):
                if "pid:" in line.lower():
                    processes.append(line.strip())
    except (FileNotFoundError, subprocess.TimeoutExpired):
        pass

    return processes


def safe_delete_file(filepath: str, max_retries: int = 3, retry_delay: float = 0.5) -> bool:
    """
    Safely delete a file, handling locked files gracefully.

    Strategy:
    1. Try direct delete first
    2. If locked, try rename-to-trash approach
    3. Use MoveFileEx with MOVEFILE_DELAY_UNTIL_REBOOT as last resort
    """
    if not os.path.exists(filepath):
        return True

    filename = os.path.basename(filepath)

    # Strategy 1: Try direct delete with retries
    for attempt in range(max_retries):
        try:
            os.remove(filepath)
            return True
        except PermissionError:
            if attempt < max_retries - 1:
                time.sleep(retry_delay * (2**attempt))  # Exponential backoff
            continue
        except Exception as e:
            log(f"[SafeDelete] Error deleting {filename}: {e}")
            break

    # Strategy 2: Check if actually locked and try rename
    if is_file_locked(filepath):
        log(f"[SafeDelete] {filename} is locked by another process")

        # Try to identify the locking process
        locking = find_process_locking_file(filepath)
        if locking:
            log(f"[SafeDelete] Locking process info: {locking}")

        # Try rename-to-trash approach (this usually works even when locked)
        try:
            import random

            trash_name = f"{filepath}.old.{int(time.time())}.{random.randint(1000, 9999)}"
            os.rename(filepath, trash_name)
            log(f"[SafeDelete] Renamed locked {filename} to {os.path.basename(trash_name)}")

            # Now try to delete the renamed file (non-blocking)
            try:
                os.remove(trash_name)
                log("[SafeDelete] Deleted renamed file immediately")
            except Exception:
                # Schedule for deletion on reboot
                try:
                    import ctypes

                    kernel32 = getattr(ctypes, "windll").kernel32
                    MOVEFILE_DELAY_UNTIL_REBOOT = 0x4
                    kernel32.MoveFileExW(trash_name, None, MOVEFILE_DELAY_UNTIL_REBOOT)
                    log(f"[SafeDelete] Scheduled {filename} for deletion on next reboot")
                except Exception:
                    pass

            return True

        except OSError as e:
            log(f"[SafeDelete] Failed to rename locked {filename}: {e}")

    # Strategy 3: Schedule original file for deletion on reboot
    try:
        import ctypes

        kernel32 = getattr(ctypes, "windll").kernel32
        MOVEFILE_DELAY_UNTIL_REBOOT = 0x4

        # MoveFileEx with NULL destination schedules deletion
        result = kernel32.MoveFileExW(filepath, None, MOVEFILE_DELAY_UNTIL_REBOOT)
        if result:
            log(f"[SafeDelete] Scheduled {filename} for deletion on next reboot")
            return True
    except Exception as e:
        log(f"[SafeDelete] MoveFileEx failed: {e}")

    log(f"[SafeDelete] WARNING: Could not delete or rename {filename}")
    return False


def find_obsolete_process_loopback_helper_artifacts(search_roots=None) -> List[str]:
    """Return stale standalone process-loopback helper outputs below product build roots."""
    roots = search_roots if search_roots is not None else (BIN_DIR, OBJ_DIR)
    artifacts = set()
    for root in roots:
        if not os.path.isdir(root):
            continue
        pattern = os.path.join(root, "**", "process_loopback_helper*")
        for candidate in glob.glob(pattern, recursive=True):
            if os.path.isfile(candidate) or os.path.islink(candidate):
                artifacts.add(os.path.normpath(candidate))
    return sorted(artifacts, key=str.casefold)


def remove_obsolete_process_loopback_helper_artifacts(search_roots=None) -> None:
    """Remove obsolete helper outputs and fail if any locked/renamed artifact remains."""
    artifacts = find_obsolete_process_loopback_helper_artifacts(search_roots)
    failed = [artifact for artifact in artifacts if not safe_delete_file(artifact)]
    remaining = find_obsolete_process_loopback_helper_artifacts(search_roots)
    if failed or remaining:
        paths = sorted(set(failed + remaining), key=str.casefold)
        raise RuntimeError("Obsolete process-loopback helper artifact remains: " + ", ".join(paths))
    if artifacts:
        log(f"Removed {len(artifacts)} obsolete process-loopback helper artifact(s)")


def assert_no_obsolete_process_loopback_helper_artifacts(search_roots=None) -> None:
    """Fail product verification if a standalone helper output reappeared."""
    artifacts = find_obsolete_process_loopback_helper_artifacts(search_roots)
    if artifacts:
        raise RuntimeError("Obsolete process-loopback helper artifact present: " + ", ".join(artifacts))


def clear_stale_hook_pdb_cache() -> None:
    """Remove cached hook PDBs from the system symbol cache so cdb uses fresh ones."""
    if not IS_WINDOWS:
        return
    sym_cache_dir = r"C:\ProgramData\dbg\sym"
    if not os.path.isdir(sym_cache_dir):
        return
    for entry in os.listdir(sym_cache_dir):
        if entry.startswith("capture_hook_") and entry.endswith(".pdb"):
            pdb_cache_path = os.path.join(sym_cache_dir, entry)
            try:
                log(f"Removing stale PDB cache: {pdb_cache_path}")
                shutil.rmtree(pdb_cache_path, ignore_errors=True)
            except Exception as e:
                log(f"[Warning] Failed to remove stale PDB cache {pdb_cache_path}: {e}")
