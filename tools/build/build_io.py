

def safe_copy_file(src: str, dst: str) -> bool:
    """
    Safely copy a file, handling locked destination files gracefully.

    Strategy:
    1. If destination exists and is locked, rename it first
    2. Then copy the source file
    """
    import random

    dst_name = os.path.basename(dst)

    # If destination exists, check if locked and rename if necessary
    if os.path.exists(dst):
        if is_file_locked(dst):
            log(f"[SafeCopy] {dst_name} is locked, attempting rename...")
            try:
                trash_name = f"{dst}.old.{int(time.time())}.{random.randint(1000, 9999)}"
                os.rename(dst, trash_name)
                log(f"[SafeCopy] Renamed locked {dst_name} to {os.path.basename(trash_name)}")
                # Try to delete the renamed file, schedule for reboot if fails
                try:
                    os.remove(trash_name)
                except Exception:
                    try:
                        import ctypes

                        kernel32 = getattr(ctypes, "windll").kernel32
                        MOVEFILE_DELAY_UNTIL_REBOOT = 0x4
                        kernel32.MoveFileExW(trash_name, None, MOVEFILE_DELAY_UNTIL_REBOOT)
                        log(f"[SafeCopy] Scheduled old {dst_name} for deletion on next reboot")
                    except Exception:
                        pass
            except OSError as e:
                log(f"[SafeCopy] WARNING: Could not rename locked {dst_name}: {e}")
                return False
        else:
            # Not locked, try to delete normally
            try:
                os.remove(dst)
            except Exception as e:
                log(f"[SafeCopy] WARNING: Could not remove existing {dst_name}: {e}")
                return False

    # Copy the file
    try:
        shutil.copy2(src, dst)
        return True
    except Exception as e:
        log(f"[SafeCopy] ERROR: Failed to copy {os.path.basename(src)} to {dst_name}: {e}")
        return False


def safe_copy_file_if_changed(src: str, dst: str) -> tuple[bool, bool]:
    """Return (success, copied), preserving an identical destination in place."""
    try:
        if (
            os.path.isfile(dst)
            and os.path.getsize(src) == os.path.getsize(dst)
            and sha256_file(src) == sha256_file(dst)
        ):
            return True, False
    except OSError:
        pass
    return safe_copy_file(src, dst), True


def safe_remove_tree(path: str, max_retries: int = 3) -> bool:
    """Safely remove a directory tree, handling locked files."""
    if not os.path.exists(path):
        return True

    # First pass: try to delete individual files with safe_delete_file
    for root, dirs, files in os.walk(path):
        for file in files:
            filepath = os.path.join(root, file)
            safe_delete_file(filepath, max_retries=max_retries)

    # Second pass: try shutil.rmtree for remaining directories
    try:
        shutil.rmtree(path, ignore_errors=True)
    except Exception as e:
        log(f"[SafeDelete] rmtree failed for {path}: {e}")
        return False

    return not os.path.exists(path)


def prepare_command_with_response_file(
    command: List[str], response_file: str, max_command_length: int = 30000
) -> List[str]:
    """Move a long clang command's arguments into a response file."""
    if len(subprocess.list2cmdline(command)) <= max_command_length:
        return command
    if len(command) < 2:
        raise ValueError("A response-file command requires an executable and arguments")

    response_parent = os.path.dirname(response_file)
    if response_parent:
        os.makedirs(response_parent, exist_ok=True)
    with open(response_file, "w", encoding="utf-8", newline="\n") as rsp:
        for argument in command[1:]:
            escaped = argument.replace("\\", "/").replace('"', '\\"')
            rsp.write(f'"{escaped}"\n')
    log(f"Using response file for {os.path.basename(command[0])} ({len(command) - 1} arguments)")
    return [command[0], "@" + response_file]


def run_command(
    cmd: Union[List[str], str],
    env: Optional[Dict[str, str]] = None,
    cwd: Optional[str] = None,
    input_str: Optional[str] = None,
    fail_exit: bool = True,
    timeout: Optional[int] = None,
) -> str:
    cmd_str = subprocess.list2cmdline(cmd) if isinstance(cmd, list) else cmd
    log(f"Running: {cmd_str}", detail=True)
    try:
        input_bytes = input_str.encode("utf-8") if input_str is not None else None
        result = subprocess.run(
            cmd,
            capture_output=True,
            env=env,
            cwd=cwd,
            input=input_bytes,
            timeout=timeout,
        )
        stdout = result.stdout.decode("utf-8", errors="replace") if result.stdout else ""
        stderr = result.stderr.decode("utf-8", errors="replace") if result.stderr else ""
        log_captured_output("stdout", stdout, detail=True)
        log_captured_output("stderr", stderr, detail=True)
        if result.returncode != 0:
            log(f"ERROR: Command failed with code {result.returncode}")
            log_failure_output_tail("stdout", stdout)
            log_failure_output_tail("stderr", stderr)
            if fail_exit:
                sys.exit(1)
        return stdout
    except subprocess.TimeoutExpired as e:
        log(f"TIMEOUT: Command exceeded {timeout}s: {cmd_str}")
        stdout = e.stdout.decode("utf-8", errors="replace") if e.stdout else ""
        stderr = e.stderr.decode("utf-8", errors="replace") if e.stderr else ""
        log_captured_output("partial stdout", stdout, detail=True)
        log_captured_output("partial stderr", stderr, detail=True)
        log_failure_output_tail("partial stdout", stdout)
        log_failure_output_tail("partial stderr", stderr)
        if fail_exit:
            sys.exit(1)
        return ""
    except Exception as e:
        log(f"EXCEPTION: {e}")
        if fail_exit:
            sys.exit(1)
        return ""


def run_logged_subprocess(
    command,
    *,
    cwd=None,
    env=None,
    check=False,
    shell=False,
):
    """Stream a subprocess into the durable detail log and keep console output concise."""
    command_list = list(command) if not isinstance(command, str) else command
    command_text = subprocess.list2cmdline(command_list) if isinstance(command_list, list) else command_list
    log(f"Running: {command_text}", detail=True)
    process = subprocess.Popen(
        command_list,
        cwd=cwd,
        env=env,
        shell=shell,
        stdout=subprocess.PIPE,
        stderr=subprocess.STDOUT,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    output_lines = deque(maxlen=FAILURE_OUTPUT_TAIL_LINES)
    assert process.stdout is not None
    for raw_line in process.stdout:
        line = raw_line.rstrip("\r\n")
        output_lines.append(line)
        log(f"[subprocess] {line}", detail=True)
    return_code = process.wait()
    output = "\n".join(output_lines)
    if return_code != 0:
        log(f"ERROR: Command failed with code {return_code}: {command_text}")
        log_failure_output_tail("subprocess", output)
        if check:
            raise subprocess.CalledProcessError(return_code, command_list, output=output)
    return subprocess.CompletedProcess(command_list, return_code, stdout=output, stderr=None)


def is_windows_process_running(image_name: str) -> bool:
    if not IS_WINDOWS:
        return False

    try:
        result = subprocess.run(
            ["tasklist", "/FI", f"IMAGENAME eq {image_name}", "/FO", "CSV", "/NH"],
            capture_output=True,
            timeout=10,
            check=False,
        )
    except Exception as e:
        log(f"Warning: Failed to query {image_name}: {e}")
        return False

    if result.returncode != 0:
        return False

    stdout = result.stdout.decode("utf-8", errors="ignore") if result.stdout else ""
    for raw_line in stdout.splitlines():
        line = raw_line.strip()
        if not line or line.startswith("INFO:"):
            continue
        parts = line.strip('"').split('","')
        if parts and parts[0].lower() == image_name.lower():
            return True

    return False


def clear_stale_msys2_pacman_lock() -> None:
    if not IS_WINDOWS:
        return

    lock_path = os.path.join(MSYS2_DIR, "var", "lib", "pacman", "db.lck")
    if not os.path.exists(lock_path):
        return

    if is_windows_process_running("pacman.exe"):
        log(f"MSYS2 package manager lock is active: {lock_path}")
        return

    try:
        os.chmod(lock_path, stat.S_IWRITE | stat.S_IREAD)
        os.remove(lock_path)
        log(f"Removed stale MSYS2 pacman lock: {lock_path}")
    except OSError as e:
        log(f"Warning: Failed to remove stale MSYS2 pacman lock {lock_path}: {e}")


def bump_and_write_build_version():
    version_header_path = os.path.join(PROJECT_ROOT, "common", "build_version.h")
    # Store build number in build directory (not tracked by git)
    build_num_path = os.path.join(BUILD_DIR, "build_number.txt")

    os.makedirs(BUILD_DIR, exist_ok=True)
    os.makedirs(os.path.dirname(version_header_path), exist_ok=True)

    build_number = 0

    # Try to read local build number
    if os.path.exists(build_num_path):
        try:
            with open(build_num_path, "r") as f:
                build_number = int(f.read().strip())
        except Exception as e:
            log(f"Warning: Failed to read {build_num_path}: {e}")

    # Fallback/Seed: If no local counter, try to read from existing header to continuity
    # (Optional: user wanted to get rid of "weird git contraption", but seeding once is safe)
    if build_number == 0 and os.path.exists(version_header_path):
        try:
            with open(version_header_path, "r", encoding="utf-8") as f:
                txt = f.read()
            m = re.search(r"#define\s+BUILD_NUMBER\s+(\d+)", txt)
            if m:
                build_number = int(m.group(1))
        except Exception:
            pass

    build_number += 1

    # Save new build number
    try:
        with open(build_num_path, "w") as f:
            f.write(str(build_number))
    except Exception as e:
        log(f"Warning: Failed to write {build_num_path}: {e}")

    timestamp = datetime.datetime.now().strftime("%Y-%m-%d %H:%M:%S")
    version_str = f"0.1.{build_number}"

    contents = (
        "#pragma once\n\n"
        f"#define BUILD_NUMBER {build_number}\n"
        f'#define CAPTURE_VERSION "{version_str}"\n'
        f'#define BUILD_TIMESTAMP "{timestamp}"\n'
    )

    try:
        with open(version_header_path, "w", encoding="utf-8") as f:
            f.write(contents)
        log(f"Build version bumped: {version_str}")
    except Exception as e:
        log(f"ERROR: Failed to write {version_header_path}: {e}")
        sys.exit(1)

    return build_number  # Return for use by caller


def read_build_version_number(version_header_path: Optional[str] = None) -> int:
    """Read the parent build number without changing the shared build metadata."""
    if version_header_path is None:
        version_header_path = os.path.join(PROJECT_ROOT, "common", "build_version.h")
    try:
        with open(version_header_path, "r", encoding="utf-8") as version_header:
            contents = version_header.read()
    except OSError as error:
        raise RuntimeError(f"Unable to read {version_header_path}: {error}") from error

    match = re.search(r"#define\s+BUILD_NUMBER\s+(\d+)", contents)
    if not match:
        raise RuntimeError(f"No BUILD_NUMBER definition found in {version_header_path}")
    return int(match.group(1))


def read_failed_build_resume_state(
    manifest_path: Optional[str] = None, version_header_path: Optional[str] = None
) -> Tuple[int, bool]:
    """Return the failed build identity and whether its verification gate must resume."""
    if manifest_path is None:
        manifest_path = os.path.join(VERIFICATION_DIR, "latest_manifest.json")
    try:
        with open(manifest_path, "r", encoding="utf-8") as manifest_file:
            manifest = json.load(manifest_file)
    except (OSError, json.JSONDecodeError) as error:
        raise RuntimeError(f"Unable to read failed-build resume state from {manifest_path}: {error}") from error

    if manifest.get("success") is not False or not manifest.get("top_level"):
        raise RuntimeError("The latest top-level build did not fail; refusing to reuse its build identity")
    previous_args = manifest.get("args", [])
    if not isinstance(previous_args, list) or "--no-build" in previous_args:
        raise RuntimeError("The latest failed run was not a compilable product build")
    if "--sanitize-regression-child" in previous_args:
        raise RuntimeError("A sanitizer child cannot be resumed as a top-level product build")
    expected_build_script_hash = manifest.get("build_script_sha256")
    current_build_script_hash = sha256_file(os.path.abspath(__file__))
    if not isinstance(expected_build_script_hash, str) or expected_build_script_hash != current_build_script_hash:
        raise RuntimeError("build.py changed since the failed attempt; a clean build is required")

    build_number = manifest.get("build_number")
    if not isinstance(build_number, int) or build_number <= 0:
        raise RuntimeError("The latest failed build has no valid build number")
    header_build_number = read_build_version_number(version_header_path)
    if header_build_number != build_number:
        raise RuntimeError(
            f"Failed-build identity mismatch: manifest={build_number}, build_version.h={header_build_number}"
        )
    restore_verification_mode = manifest.get("mode") == "verify" or "--verify" in previous_args
    return build_number, restore_verification_mode


def read_failed_build_resume_version(
    manifest_path: Optional[str] = None, version_header_path: Optional[str] = None
) -> int:
    """Return the reusable identity of the immediately preceding failed top-level build."""
    build_number, _ = read_failed_build_resume_state(manifest_path, version_header_path)
    return build_number


def resolve_build_number_for_invocation(
    *, sanitize_regression_child: bool, resume_failed_build: bool, no_build: bool, tests_only: bool = False
) -> int:
    if sanitize_regression_child:
        return read_build_version_number()
    if resume_failed_build:
        return read_failed_build_resume_version()
    if no_build or tests_only:
        return read_build_version_number()
    return bump_and_write_build_version()


def get_mingw_compilers():
    """Get mingw-w64 compiler paths based on platform."""
    if IS_LINUX:
        x64_clang = shutil.which("x86_64-w64-mingw32-clang++")
        x64_gcc = shutil.which("x86_64-w64-mingw32-g++")
        x86_clang = shutil.which("i686-w64-mingw32-clang++")
        x86_gcc = shutil.which("i686-w64-mingw32-g++")

        x64_compiler = x64_clang or x64_gcc
        x86_compiler = x86_clang or x86_gcc

        if not x64_compiler:
            log("ERROR: No mingw-w64 x64 compiler found. Install: sudo apt install mingw-w64")
            sys.exit(1)

        x64_bin = os.path.dirname(x64_compiler)
        x86_bin = os.path.dirname(x86_compiler) if x86_compiler else ""

        x64_cc = x64_compiler.replace("clang++", "clang").replace("g++", "gcc")
        x86_cc = x86_compiler.replace("clang++", "clang").replace("g++", "gcc") if x86_compiler else None

        return {
            "x64": {"bin": x64_bin, "cxx": x64_compiler, "cc": x64_cc},
            "x86": {"bin": x86_bin, "cxx": x86_compiler, "cc": x86_cc},
        }
    else:
        return None


def has_linux_x86_compiler() -> bool:
    if not IS_LINUX:
        return True
    compilers = get_mingw_compilers()
    return bool(compilers and compilers["x86"]["cxx"])


def is_clang_compiler(compiler_exe: Optional[str]) -> bool:
    return bool(compiler_exe and "clang" in os.path.basename(compiler_exe).lower())


def get_linux_mingw_runtime_dirs(arch: str) -> List[str]:
    if not IS_LINUX:
        return []

    compiler = get_compiler_exe(arch)
    if not compiler:
        return []

    candidates: List[str] = [os.path.dirname(compiler)]
    runtime_names = ("libstdc++-6.dll", "libgcc_s_seh-1.dll", "libwinpthread-1.dll")
    for runtime_name in runtime_names:
        try:
            resolved = subprocess.check_output(
                [compiler, f"-print-file-name={runtime_name}"],
                encoding="utf-8",
                errors="ignore",
                stderr=subprocess.DEVNULL,
            ).strip()
        except Exception:
            resolved = ""
        if resolved and resolved != runtime_name:
            candidates.append(os.path.dirname(resolved))

    try:
        sysroot = subprocess.check_output(
            [compiler, "-print-sysroot"],
            encoding="utf-8",
            errors="ignore",
            stderr=subprocess.DEVNULL,
        ).strip()
    except Exception:
        sysroot = ""

    if sysroot:
        candidates.extend(
            [
                os.path.join(sysroot, "bin"),
                os.path.join(sysroot, "lib"),
                os.path.join(sysroot, "mingw", "bin"),
                os.path.join(sysroot, "mingw", "lib"),
            ]
        )

    unique_candidates: List[str] = []
    seen = set()
    for candidate in candidates:
        if not candidate or not os.path.isdir(candidate):
            continue
        normalized = os.path.normcase(os.path.abspath(candidate))
        if normalized in seen:
            continue
        seen.add(normalized)
        unique_candidates.append(candidate)
    return unique_candidates


def get_linux_vulkan_import_lib_path(arch: str) -> Optional[str]:
    if not IS_LINUX:
        return os.path.join(
            MSYS2_DIR,
            "clang64" if arch == "x64" else "mingw32",
            "lib",
            "libvulkan-1.dll.a",
        )

    compiler = get_compiler_exe(arch)
    if compiler:
        try:
            resolved = subprocess.check_output(
                [compiler, "-print-file-name=libvulkan-1.dll.a"],
                encoding="utf-8",
                errors="ignore",
                stderr=subprocess.DEVNULL,
            ).strip()
        except Exception:
            resolved = ""
        if resolved and resolved != "libvulkan-1.dll.a" and os.path.exists(resolved):
            return resolved

    if arch == "x64":
        fallback = os.path.join(get_linux_msys2_dir(), "clang64", "lib", "libvulkan-1.dll.a")
        if os.path.exists(fallback):
            return fallback

    return None


def get_compiler_exe(arch: str = "x64") -> Optional[str]:
    """Get the compiler executable for the given architecture."""
    if IS_LINUX:
        compilers = get_mingw_compilers()
        if compilers is None:
            log("ERROR: MinGW compiler discovery failed on Linux host")
            sys.exit(1)
        compiler = compilers[arch]["cxx"]
        if compiler:
            return compiler
        if arch == "x86":
            return None
        log(f"ERROR: MinGW compiler for {arch} not found on Linux host")
        sys.exit(1)
    else:
        if arch == "x64":
            return os.path.join(MSYS2_DIR, "clang64", "bin", "clang++.exe")
        else:
            # Cross-compile x86 from clang64 (mingw-w64-i686-clang was removed from MSYS2)
            return os.path.join(MSYS2_DIR, "clang64", "bin", "clang++.exe")


def get_windres_exe(arch: str = "x64") -> str:
    """Get the windres executable for the given architecture."""
    if IS_LINUX:
        if arch == "x64":
            return shutil.which("x86_64-w64-mingw32-windres") or "windres"
        else:
            return shutil.which("i686-w64-mingw32-windres") or "windres"
    else:
        return os.path.join(MSYS2_DIR, "clang64" if arch == "x64" else "mingw32", "bin", "windres.exe")


def setup_msys2(skip_updates: bool = False):
    if IS_LINUX:
        log("Running on Linux/WSL - skipping MSYS2 setup, using system mingw-w64")
        check_mingw_packages()
        download_msys2_packages_for_linux()
        return

    msys_bash = os.path.join(MSYS2_DIR, "usr", "bin", "bash.exe")
    if not os.path.exists(msys_bash):
        log("Downloading MSYS2...")
        os.makedirs(BUILD_DIR, exist_ok=True)
        msys2_url = resolve_msys2_url()
        tar_name = os.path.basename(msys2_url)
        tar_path = os.path.join(BUILD_DIR, tar_name)
        if not os.path.exists(tar_path):
            log(f"Downloading MSYS2 base archive: {tar_name}")
            temp_tar_path = tar_path + ".tmp"
            try:
                if os.path.exists(temp_tar_path):
                    os.remove(temp_tar_path)
                urllib.request.urlretrieve(msys2_url, temp_tar_path)
                os.replace(temp_tar_path, tar_path)
            finally:
                if os.path.exists(temp_tar_path):
                    os.remove(temp_tar_path)
        else:
            log(f"Using cached MSYS2 base archive: {tar_name}")

        verify_msys2_bootstrap_archive(tar_path, msys2_url)
        log("Extracting MSYS2...")
        with tarfile.open(tar_path) as f:
            safe_extract_tar(f, BUILD_DIR)

        msys_bash = os.path.join(MSYS2_DIR, "usr", "bin", "bash.exe")
        if not os.path.exists(msys_bash):
            raise RuntimeError(f"MSYS2 extraction finished, but {msys_bash} was not created")
        clear_stale_msys2_pacman_lock()
        run_command([msys_bash, "-lc", "pacman-key --init"])
        clear_stale_msys2_pacman_lock()
        run_command([msys_bash, "-lc", "pacman-key --populate msys2"])
        clear_stale_msys2_pacman_lock()
        run_command([msys_bash, "-lc", "pacman -Sy --noconfirm --disable-download-timeout"])
    else:
        log("MSYS2 found.")

    if skip_updates:
        log("MSYS2 package updates/install skipped (--skip-updates)")
        patch_amf_header()
        verify_msys2_ffmpeg_build_deps(MSYS2_DIR)
        return

    log("Updating MSYS2 base and installed packages...")
    for update_pass in range(2):
        log(f"MSYS2 full upgrade pass {update_pass + 1}/2")
        clear_stale_msys2_pacman_lock()
        run_command(
            [
                msys_bash,
                "-lc",
                "pacman -Syu --noconfirm --disable-download-timeout",
            ],
            input_str="\n",
            timeout=900,
        )

    log("Installing Packages...")
    msys_bash = os.path.join(MSYS2_DIR, "usr", "bin", "bash.exe")
    pkg_cmd = f"pacman -S --needed --noconfirm --disable-download-timeout {' '.join(PACKAGES)}"
    clear_stale_msys2_pacman_lock()
    run_command(
        [msys_bash, "-lc", pkg_cmd],
        input_str="\n",
        timeout=600,
    )
    patch_amf_header()
    verify_msys2_ffmpeg_build_deps(MSYS2_DIR)


def check_mingw_packages():
    """Check and install mingw-w64 packages on Linux/WSL."""
    if not IS_LINUX:
        return

    log("Checking for mingw-w64 compilers...")
    x64_compiler = shutil.which("x86_64-w64-mingw32-g++") or shutil.which("x86_64-w64-mingw32-clang++")

    if not x64_compiler:
        log("mingw-w64 not found. Installing via apt...")
        try:
            subprocess.run(["sudo", "apt", "update"], check=True)
            subprocess.run(["sudo", "apt", "install", "-y", "mingw-w64"], check=True)
            log("mingw-w64 installed successfully.")
        except subprocess.CalledProcessError as e:
            log(f"Failed to install mingw-w64: {e}")
            log("Please install manually: sudo apt install mingw-w64")
            sys.exit(1)
    else:
        log(f"Found mingw-w64 compiler: {x64_compiler}")
        if not (shutil.which("i686-w64-mingw32-g++") or shutil.which("i686-w64-mingw32-clang++")):
            log("Linux host missing mingw-w64 x86 compiler - x86 targets will be skipped")


def _find_nvngx_driverstore_paths() -> List[str]:
    """Find _nvngx.dll in the NVIDIA DriverStore or System32."""
    paths: List[str] = []
    # System32 — installed by running an NGX-enabled game
    sys32 = r"C:\Windows\System32\_nvngx.dll"
    if os.path.exists(sys32):
        paths.append(sys32)
    # DriverStore — driver staging copy
    ds_root = r"C:\Windows\System32\DriverStore\FileRepository"
    if os.path.isdir(ds_root):
        for entry in os.listdir(ds_root):
            if entry.startswith("nv_dispi"):
                candidate = os.path.join(ds_root, entry, "_nvngx.dll")
                if os.path.exists(candidate):
                    paths.append(candidate)
    return paths
