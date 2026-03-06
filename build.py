import os
import sys
import glob
import shutil
import tarfile
import subprocess
import urllib.request
import time
import datetime
import hashlib
import platform
from concurrent.futures import ThreadPoolExecutor, as_completed
from multiprocessing import cpu_count
import re
import json

from typing import List, Dict, Optional, Union, Any

# --- Platform Detection ---
IS_WINDOWS = sys.platform == "win32"
IS_LINUX = sys.platform.startswith("linux")
IS_WSL = IS_LINUX and "microsoft" in platform.uname().release.lower()

# --- Optimization Flags ---
# x86-64-v3 requires AVX2 (Haswell 2013+), provides ~10-20% performance boost
# Used only for the host process (captureengine.exe) where CPU is controlled.
OPT_FLAGS_X64 = [
    "-O3",
    "-flto",
    "-ffast-math",
    "-march=x86-64-v3",
    "-mtune=generic",
    "-fvisibility=hidden",
    "-ffunction-sections",
    "-fdata-sections",
]

# Hook DLL flags: injected into arbitrary game processes — must not require AVX2
# and must not use -ffast-math (audio encoder correctness requires IEEE 754 semantics).
HOOK_OPT_FLAGS_X64 = [
    "-O3",
    "-flto",
    "-march=x86-64-v2",  # SSE4.2 + POPCNT minimum — safe for CPUs back to ~2008
    "-mtune=generic",
    "-fvisibility=hidden",
    "-ffunction-sections",
    "-fdata-sections",
    # Hook DLL performs aliased pointer access (vtable patching, SHM reinterpret_cast).
    # Without -fno-strict-aliasing the optimizer may miscompile these pointer casts.
    "-fno-strict-aliasing",
]

# x86 builds use generic optimization (no AVX on 32-bit)
OPT_FLAGS_X86 = [
    "-O3",
    "-flto",
    "-ffast-math",
    "-march=i686",
    "-mtune=generic",
    "-fvisibility=hidden",
    "-ffunction-sections",
    "-fdata-sections",
]

# x86 hook DLL: no -ffast-math (audio correctness)
# LTO is NOT enabled for x86: the mingw32 toolchain uses a GCC runtime that
# lacks the libc++ call_once symbols required by -flto=thin with lld.
HOOK_OPT_FLAGS_X86 = [
    "-O3",
    "-march=i686",
    "-mtune=generic",
    "-fvisibility=hidden",
    "-ffunction-sections",
    "-fdata-sections",
    "-fno-strict-aliasing",  # Same aliasing concerns as x64 hook DLL
]

# Linker optimization flags
# -g1 keeps minimal DWARF info (function names + file/line) for crash symbolication
# without meaningfully increasing binary size.  The separate --strip-debug step
# below removes it from the final shipped binary but keeps a .debug file for
# post-mortem analysis.
LD_OPT_FLAGS = [
    "-Wl,--gc-sections",
    "-Wl,--dynamicbase",  # ASLR
    "-Wl,--nxcompat",  # DEP/NX
    "-g1",  # Minimal debug info for crash symbolication
]

# --- Configuration ---
BUILD_DIR_NAME = "build"
COMPILE_COMMANDS: List[Dict[str, Any]] = []
CURRENT_BUILD_NUMBER = 0  # Set by bump_and_write_build_version()

PROJECT_ROOT = os.path.dirname(os.path.abspath(__file__))

BUILD_DIR = os.path.join(PROJECT_ROOT, BUILD_DIR_NAME)
MSYS2_URL = "https://repo.msys2.org/distrib/x86_64/msys2-base-x86_64-20240113.tar.xz"
MSYS2_DIR = os.path.join(BUILD_DIR, "msys64")
OBJ_DIR = os.path.join(BUILD_DIR, "obj")
BIN_DIR = os.path.join(BUILD_DIR, "bin")
INSTALLED_DIR = os.path.join(PROJECT_ROOT, "installed")
CAPTURE_BIN_DIR = os.path.join(INSTALLED_DIR, "captureengine")
TESTAPP_BIN_DIR = os.path.join(INSTALLED_DIR, "testapp")
BIN_DIR = CAPTURE_BIN_DIR  # output captureengine binaries to installed\captureengine
LOG_FILE = os.path.join(PROJECT_ROOT, "build.log")

# IMGUI_URL and IMGUI_DIR removed - Custom overlay renderer replaces ImGui

FFMPEG_DIR = os.path.join(PROJECT_ROOT, "external", "ffmpeg")

PACKAGES = [
    "mingw-w64-clang-x86_64-toolchain",
    # "mingw-w64-x86_64-toolchain", # GCC removed (User requested Zig)
    "mingw-w64-clang-x86_64-pkgconf",
    # ffmpeg & codecs removed (built from source)
    "mingw-w64-clang-x86_64-openssl",
    "mingw-w64-clang-x86_64-libxml2",
    "mingw-w64-clang-x86_64-shaderc",
    "mingw-w64-clang-x86_64-cmake",
    "mingw-w64-clang-x86_64-ninja",
    "mingw-w64-clang-x86_64-meson",
    "mingw-w64-clang-x86_64-nasm",
    "mingw-w64-clang-x86_64-vulkan-headers",
    "mingw-w64-clang-x86_64-vulkan-loader",
    "mingw-w64-i686-toolchain",
    "mingw-w64-i686-clang",
    "mingw-w64-i686-vulkan-headers",
    "mingw-w64-i686-vulkan-loader",
    "mingw-w64-clang-x86_64-cppwinrt",  # For Windows Graphics Capture
    "mingw-w64-clang-x86_64-gtest",
    "mingw-w64-clang-x86_64-amf-headers",
    "mingw-w64-clang-x86_64-onevpl",  # For QSV
    "mingw-w64-clang-x86_64-lld",  # For delay-load support (x64)
    "mingw-w64-i686-lld",  # For delay-load support (x86)
    "mingw-w64-clang-x86_64-clang-tools-extra",  # For clang-format
    "make",
    "ccache",
]


def log(msg: str) -> None:
    timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
    formatted = f"[{timestamp}] {msg}"
    print(formatted)
    try:
        with open(LOG_FILE, "a") as f:
            f.write(formatted + "\n")
    except Exception:
        pass


def safe_extract_tar(archive: tarfile.TarFile, destination: str) -> None:
    """Safely extract tar archive, preventing path traversal."""
    dest_abs = os.path.normcase(os.path.abspath(destination))
    for member in archive.getmembers():
        member_abs = os.path.normcase(os.path.abspath(os.path.join(dest_abs, member.name)))
        if member_abs != dest_abs and not member_abs.startswith(dest_abs + os.sep):
            raise RuntimeError(f"Unsafe archive member path: {member.name}")
    archive.extractall(dest_abs)


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


def run_command(
    cmd: Union[List[str], str],
    env: Optional[Dict[str, str]] = None,
    cwd: Optional[str] = None,
    input_str: Optional[str] = None,
    fail_exit: bool = True,
) -> str:
    cmd_str = " ".join(cmd) if isinstance(cmd, list) else cmd

    log(f"Running: {cmd_str}")
    try:
        result = subprocess.run(cmd, capture_output=True, text=True, env=env, cwd=cwd, input=input_str)
        if result.returncode != 0:
            log(f"ERROR: Command failed with code {result.returncode}")
            log(f"STDOUT: {result.stdout}")
            log(f"STDERR: {result.stderr}")
            if fail_exit:
                sys.exit(1)
        return result.stdout
    except Exception as e:
        log(f"EXCEPTION: {e}")
        if fail_exit:
            sys.exit(1)
        return ""


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
    version_str = f"1.1.0-dev+build.{build_number}"

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
        x86_bin = os.path.dirname(x86_compiler) if x86_compiler else x64_bin

        x64_cc = x64_compiler.replace("clang++", "clang").replace("g++", "gcc")
        x86_cc = x86_compiler.replace("clang++", "clang").replace("g++", "gcc") if x86_compiler else None

        return {
            "x64": {"bin": x64_bin, "cxx": x64_compiler, "cc": x64_cc},
            "x86": {"bin": x86_bin, "cxx": x86_compiler or x64_compiler, "cc": x86_cc},
        }
    else:
        return None


def get_compiler_exe(arch="x64"):
    """Get the compiler executable for the given architecture."""
    if IS_LINUX:
        compilers = get_mingw_compilers()
        if compilers is None:
            log("ERROR: MinGW compiler discovery failed on Linux host")
            sys.exit(1)
        return compilers[arch]["cxx"]
    else:
        if arch == "x64":
            return os.path.join(MSYS2_DIR, "clang64", "bin", "clang++.exe")
        else:
            return os.path.join(MSYS2_DIR, "mingw32", "bin", "clang++.exe")


def get_windres_exe(arch="x64"):
    """Get the windres executable for the given architecture."""
    if IS_LINUX:
        if arch == "x64":
            return shutil.which("x86_64-w64-mingw32-windres") or "windres"
        else:
            return shutil.which("i686-w64-mingw32-windres") or "windres"
    else:
        return os.path.join(MSYS2_DIR, "clang64" if arch == "x64" else "mingw32", "bin", "windres.exe")


def setup_msys2():
    if IS_LINUX:
        log("Running on Linux/WSL - skipping MSYS2 setup, using system mingw-w64")
        check_mingw_packages()
        download_msys2_packages_for_linux()
        return

    if not os.path.exists(MSYS2_DIR):
        log("Downloading MSYS2...")
        os.makedirs(BUILD_DIR, exist_ok=True)
        tar_path = os.path.join(BUILD_DIR, "msys2.tar.xz")
        if not os.path.exists(tar_path):
            urllib.request.urlretrieve(MSYS2_URL, tar_path)

        log("Extracting MSYS2...")
        with tarfile.open(tar_path) as f:
            safe_extract_tar(f, BUILD_DIR)

        msys_bash = os.path.join(MSYS2_DIR, "usr", "bin", "bash.exe")
        run_command([msys_bash, "-lc", "pacman-key --init"])
        run_command([msys_bash, "-lc", "pacman-key --populate msys2"])
        run_command([msys_bash, "-lc", "pacman -Sy --noconfirm"])
    else:
        log("MSYS2 found.")

    log("Installing Packages...")
    msys_bash = os.path.join(MSYS2_DIR, "usr", "bin", "bash.exe")
    pkg_cmd = f"pacman -S --needed --noconfirm {' '.join(PACKAGES)}"
    run_command([msys_bash, "-lc", pkg_cmd], input_str="\n")
    patch_amf_header()


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


def check_python_lsp_tools():
    """Check and install Python LSP/lint/format tools for better IDE support."""
    # Ensure user's local bin is in PATH for installed tools
    user_local_bin = os.path.expanduser("~/.local/bin")
    if user_local_bin not in os.environ.get("PATH", ""):
        os.environ["PATH"] = user_local_bin + os.pathsep + os.environ.get("PATH", "")

    tools = ["pyright", "flake8", "black"]

    for tool in tools:
        try:
            subprocess.run(
                [sys.executable, "-m", tool, "--version"],
                capture_output=True,
                check=True,
            )
        except (subprocess.CalledProcessError, FileNotFoundError):
            log(f"{tool} not found. Installing via pip...")
            try:
                # On Linux with externally-managed Python, use --break-system-packages
                cmd = [sys.executable, "-m", "pip", "install", tool]
                if IS_LINUX:
                    cmd.append("--break-system-packages")
                subprocess.run(cmd, check=True)
                log(f"{tool} installed successfully.")
            except subprocess.CalledProcessError as e:
                log(f"Warning: Failed to install {tool}: {e}")
                log(f"  Install manually: pip install {tool} --break-system-packages")


def get_system_ffmpeg_flags():
    """Get FFmpeg compiler flags - now uses MSYS2 FFmpeg on Linux."""
    # FFmpeg is downloaded from MSYS2 repo, not system
    return [], []


# MSYS2 packages to download for Linux builds (Windows-specific libs not in Arch repos)
LINUX_MSYS2_PACKAGES = [
    "mingw-w64-clang-x86_64-vulkan-headers",
    "mingw-w64-clang-x86_64-vulkan-loader",
    "mingw-w64-clang-x86_64-gtest",
    "mingw-w64-clang-x86_64-ffmpeg",
    "mingw-w64-clang-x86_64-cppwinrt",
    "mingw-w64-clang-x86_64-headers",
    # FFmpeg dependencies (required by external/ffmpeg/bin DLLs)
    "mingw-w64-clang-x86_64-libxml2",
    "mingw-w64-clang-x86_64-bzip2",
    "mingw-w64-clang-x86_64-libmodplug",
    "mingw-w64-clang-x86_64-libgme",
    "mingw-w64-clang-x86_64-libva",
    "mingw-w64-clang-x86_64-libvpl",
    "mingw-w64-clang-x86_64-libwinpthread",
]

MSYS2_REPO_URL = "https://repo.msys2.org/mingw/clang64"


def download_msys2_packages_for_linux():
    """Download minimal MSYS2 packages for Linux cross-compilation.

    Only downloads Windows-specific libraries/headers not available in Arch repos.
    Toolchain (clang, mingw-w64) should be installed via system pacman.

    OPTIMIZATION: Uses marker file to skip extraction if already done.
    """
    if not IS_LINUX:
        return

    msys_linux_dir = os.path.join(BUILD_DIR, "msys64_linux")
    clang64_dir = os.path.join(msys_linux_dir, "clang64")
    marker_file = os.path.join(msys_linux_dir, ".packages_installed")

    # Check if packages already extracted (marker file + clang64 exists with content)
    if os.path.exists(marker_file) and os.path.isdir(clang64_dir):
        include_dir = os.path.join(clang64_dir, "include")
        if os.path.isdir(include_dir) and os.listdir(include_dir):
            log("MSYS2 packages already installed - skipping setup")
            return msys_linux_dir

    log("Setting up MSYS2 packages for Linux cross-compilation...")
    os.makedirs(msys_linux_dir, exist_ok=True)
    os.makedirs(os.path.join(clang64_dir, "include"), exist_ok=True)
    os.makedirs(os.path.join(clang64_dir, "lib"), exist_ok=True)

    for pkg in LINUX_MSYS2_PACKAGES:
        try:
            # Find package file in repo (with version)
            log(f"Checking for {pkg} in MSYS2 repo...")
            req = urllib.request.Request(MSYS2_REPO_URL, headers={"User-Agent": "Mozilla/5.0"})
            with urllib.request.urlopen(req, timeout=30) as response:
                html = response.read().decode()

            # Find the actual package filename (with version)
            import re

            pattern = rf'href="({pkg}[^"]*-any\.pkg\.tar\.zst)"'
            match = re.search(pattern, html)
            if not match:
                log(f"Warning: Could not find package {pkg}")
                continue

            pkg_file = match.group(1)
            pkg_url = f"{MSYS2_REPO_URL}/{pkg_file}"
            pkg_path = os.path.join(msys_linux_dir, pkg_file)

            # Download if not exists
            if not os.path.exists(pkg_path):
                log(f"Downloading {pkg_file}...")
                urllib.request.urlretrieve(pkg_url, pkg_path)

            # Extract package
            log(f"Extracting {pkg_file}...")
            import tarfile

            with tarfile.open(pkg_path, "r") as tar:
                safe_extract_tar(tar, msys_linux_dir)

            log(f"Package {pkg} ready")

        except Exception as e:
            log(f"Warning: Failed to download {pkg}: {e}")
            log("Build may fail - consider installing pre-built MSYS2 from Windows")

    # Create marker file to skip on future builds
    with open(marker_file, "w") as f:
        f.write(datetime.datetime.now().isoformat())

    log("MSYS2 packages setup complete for Linux")
    return msys_linux_dir


def wsl_path_to_windows(path):
    r"""Convert WSL path (/mnt/c/...) to Windows path (C:\...)"""
    if IS_LINUX and path.startswith("/mnt/"):
        # /mnt/c/path -> C:\path
        parts = path.split("/")
        if len(parts) >= 3:
            drive = parts[2].upper()  # 'c' -> 'C'
            rest = "\\".join(parts[3:])
            return f"{drive}:\\{rest}"
    return path


def get_linux_msys2_dir():
    """Get the MSYS2 directory for Linux builds."""
    if not IS_LINUX:
        return MSYS2_DIR

    # Prefer existing MSYS2 from Windows if available
    if os.path.exists(MSYS2_DIR):
        return MSYS2_DIR

    # Otherwise use downloaded minimal setup
    return os.path.join(BUILD_DIR, "msys64_linux")


# ============================================================================
# ImGui Setup - REMOVED: No longer using ImGui
# Custom overlay renderer (custom_overlay) replaces ImGui
# ============================================================================

# --- FFmpeg Configuration ---
FFMPEG_URL = "https://git.ffmpeg.org/ffmpeg.git"
FFNVCODEC_URL = "https://git.videolan.org/git/ffmpeg/nv-codec-headers.git"


def to_unix(p):
    """Convert Windows path to MSYS2 Unix path."""
    p = p.replace("\\", "/")
    if len(p) >= 2 and p[1] == ":":
        drive = p[0].lower()
        return "/" + drive + p[2:]
    return p


class FFmpegBuilder:
    def __init__(self, root_dir, msys_dir, install_dir, license_mode="gpl"):
        self.root = root_dir
        self.msys = msys_dir
        self.install_dir = install_dir
        self.license_mode = license_mode  # 'gpl' or 'lgpl'

        self.build_root = os.path.join(self.root, "ffmpeg_build")
        self.repos_dir = os.path.join(self.build_root, "repos")
        self.working_dir = os.path.join(self.build_root, "working")

        # Output dirs
        self.prefix = to_unix(install_dir)
        self.win_prefix = install_dir
        self.license_mode = "lgpl"  # Changed to LGPL per user request

    def setup_dirs(self):
        for d in [self.build_root, self.repos_dir, self.working_dir, self.install_dir]:
            os.makedirs(d, exist_ok=True)

    def get_msys_env(self):
        env = os.environ.copy()

        # Add MSYS2 tools to path
        msys_bin = [
            os.path.join(self.msys, "clang64", "bin"),
            os.path.join(self.msys, "usr", "bin"),
            os.path.join(self.install_dir, "bin"),
        ]
        env["PATH"] = os.pathsep.join(msys_bin + [env["PATH"]])

        # Compiler flags
        pkg_config = self.get_tool_path("pkg-config")

        # Add MSYS2 paths for system libs (vpl, etc.)
        msys_inc = to_unix(os.path.join(self.msys, "clang64", "include"))
        msys_lib = to_unix(os.path.join(self.msys, "clang64", "lib"))

        env["CC"] = "clang"
        env["CXX"] = "clang++"
        env["CFLAGS"] = f"-O3 -ffunction-sections -fdata-sections -I{self.prefix}/include -I{msys_inc}"
        env["CXXFLAGS"] = f"-O3 -ffunction-sections -fdata-sections -I{self.prefix}/include -I{msys_inc}"
        env["LDFLAGS"] = f"-Wl,--gc-sections -L{self.prefix}/lib -L{msys_lib}"
        env["PKG_CONFIG"] = f"{pkg_config} --static"
        env["PKG_CONFIG_PATH"] = f"{self.prefix}/lib/pkgconfig"
        env["MSYSTEM"] = "CLANG64"  # Ensure we are treated as MinGW-Clang

        return env

    def get_tool_path(self, tool_name):
        """Find tool in MSYS2 directories."""
        # Check clang64/bin first, then usr/bin, then ucrt64/bin
        search_dirs = [
            os.path.join(self.msys, "clang64", "bin"),
            os.path.join(self.msys, "usr", "bin"),
            os.path.join(self.msys, "ucrt64", "bin"),
        ]

        exe = tool_name + ".exe"
        for d in search_dirs:
            p = os.path.join(d, exe)
            if os.path.exists(p):
                return p

        return tool_name  # Fallback to path lookup

    def run(self, cmd, cwd=None, env=None, check=True):
        # Always pass a list to subprocess.run to avoid shell=True injection risk.
        cmd_list = cmd if isinstance(cmd, list) else cmd.split()
        cmd_str = " ".join(cmd_list)
        log(f"[FFmpeg] EXEC: {cmd_str}")
        try:
            if env is None:
                env = os.environ.copy()
            if env and "PATH" not in env:
                env["PATH"] = os.environ["PATH"]

            subprocess.run(cmd_list, cwd=cwd, env=env, check=check, shell=False)
        except subprocess.CalledProcessError as e:
            log(f"[FFmpeg] FAILED: {cmd_str}")
            raise e

    def git_clone(self, url, name, update=True):
        """Clone or update a git repository. Returns (path, updated) tuple."""
        dest = os.path.join(self.repos_dir, name)
        git_exe = self.get_tool_path("git")
        env = self.get_msys_env()

        if not os.path.exists(dest):
            log(f"[FFmpeg] Cloning {name}...")
            self.run([git_exe, "clone", "--depth", "1", url, dest], env=env)
            return dest, True  # New clone = always needs build

        if not update:
            log(f"[FFmpeg] Using existing {name} (--skip-updates)")
            return dest, False

        # Get current commit before update
        old_commit = subprocess.check_output([git_exe, "rev-parse", "HEAD"], cwd=dest, env=env).decode().strip()

        # Fetch and reset to latest
        log(f"[FFmpeg] Checking for updates to {name}...")
        try:
            self.run(
                [git_exe, "fetch", "--depth", "1", "origin"],
                cwd=dest,
                env=env,
                check=False,
            )
            self.run(
                [git_exe, "reset", "--hard", "origin/HEAD"],
                cwd=dest,
                env=env,
                check=False,
            )
        except Exception as e:
            log(f"[FFmpeg] Warning: Could not update {name}: {e}")
            return dest, False

        # Get new commit
        new_commit = subprocess.check_output([git_exe, "rev-parse", "HEAD"], cwd=dest, env=env).decode().strip()

        if old_commit != new_commit:
            log(f"[FFmpeg] Updated {name}: {old_commit[:8]} -> {new_commit[:8]}")
            return dest, True
        else:
            log(f"[FFmpeg] {name} is up to date ({new_commit[:8]})")
            return dest, False

    def rmtree_onerror(self, func, path, exc_info):
        import stat
        import time

        if not os.access(path, os.W_OK):
            try:
                os.chmod(path, stat.S_IWRITE)
                func(path)
                return
            except Exception:
                pass

        ex = exc_info[1]
        if isinstance(ex, PermissionError) or isinstance(ex, OSError):
            log(f"[FFmpeg] Locked file: {path}. Retrying...")
            for i in range(5):
                try:
                    time.sleep(1.0)
                    if not os.access(path, os.W_OK):
                        os.chmod(path, stat.S_IWRITE)
                    func(path)
                    return
                except Exception:
                    pass
            log(f"[FFmpeg] FAILED to remove: {path}")
            raise

    def build_dependencies(self, update=True):
        log("[FFmpeg] Building Dependencies...")
        # 1. FFNVCodec
        nv_dir, _ = self.git_clone(FFNVCODEC_URL, "ffnvcodec", update=update)
        make_exe = self.get_tool_path("make")
        self.run(
            f'{make_exe} PREFIX="{self.prefix}" install',
            cwd=nv_dir,
            env=self.get_msys_env(),
        )

    def build_ffmpeg(self, update=True):
        """Build FFmpeg. Returns True if build was performed."""
        log("[FFmpeg] Building FFmpeg...")
        src_dir, updated = self.git_clone(FFMPEG_URL, "ffmpeg", update=update)
        build_dir = os.path.join(self.working_dir, "ffmpeg")

        if os.path.exists(build_dir):
            shutil.rmtree(build_dir, onerror=self.rmtree_onerror)
        shutil.copytree(src_dir, build_dir)

        env = self.get_msys_env()
        make_exe = self.get_tool_path("make")
        bash_exe = self.get_tool_path("bash")

        # Define msys_lib for extra-ldflags
        msys_lib = to_unix(os.path.join(self.msys, "clang64", "lib"))

        conf = [
            bash_exe,
            "./configure",
            f'--prefix="{self.prefix}"',
            "--target-os=mingw32",
            "--enable-shared",
            "--disable-static",  # SHARED BUILD
            # '--pkg-config-flags="--static"',
            "--arch=x86_64",
            # Linking fixes
            # We explicitly link dependent C++ libraries to ensure they are available to avcodec.dll
            # libvpl (for QSV) often needs -lvpl -lstdc++ and system libs
            '--extra-libs="-lc++ -lvpl -lstdc++ -lole32 -lgdi32 -luuid"',
            # Toolchain
            '--extra-libs="-lc++"',
            # Toolchain - Use MSYS2 Clang
            "--cc=clang",
            "--cxx=clang++",
            "--ar=llvm-ar",
            "--nm=llvm-nm",
            "--ranlib=llvm-ranlib",
            # Optimization
            '--extra-cflags="-O3 -ffast-math -flto"',
            '--extra-cxxflags="-O3 -ffast-math -flto"',
            '--extra-ldflags="-flto -O3"',
            f'--extra-ldflags="-L{msys_lib}"',
            # Licensing
            "--disable-gpl",  # NO GPL
            "--enable-version3",
            "--enable-nonfree",  # NVENC requires nonfree, but nonfree + lgpl is compatible?
            # Wait, NVENC headers are MIT. But --enable-nvenc in ffmpeg might trigger nonfree?
            # Actually, "The resulting binary will be nonfree".
            # If so, it's not LGPL. It's proprietary.
            # User wants MIT release. "MIT + proprietary generic binary" is allowed.
            # Key is: Don't link GPL code.
            # Components
            "--disable-doc",
            "--disable-programs",
            "--enable-ffmpeg",
            "--enable-ffprobe",
            "--disable-zlib",
            "--disable-bzlib",
            "--disable-lzma",
            "--disable-alsa",  # Linux audio not available on Windows
            # Hardware
            "--enable-d3d11va",
            "--enable-dxva2",
            "--enable-nvenc",
            "--enable-nvdec",
            "--enable-vulkan",
            "--enable-amf",
            "--enable-libvpl",  # QSV
            "--enable-mediafoundation",
            # Tuning
            "--disable-encoders",
            "--disable-decoders",
            "--disable-muxers",
            "--disable-demuxers",
            "--disable-parsers",
            "--disable-bsfs",
            "--disable-protocols",
            "--enable-protocol=file",
            "--enable-muxer=mp4,matroska,mov,flv,ts",
            "--enable-demuxer=concat,matroska,mov,mp4",
            # SW Encoders (Audio)
            "--enable-encoder=aac,opus,flac,alac",
            "--enable-decoder=aac,opus,flac,alac",
            "--enable-parser=aac,opus,flac",
            # HW Encoders
            "--enable-encoder=h264_nvenc,hevc_nvenc,av1_nvenc",
            "--enable-encoder=h264_amf,hevc_amf,av1_amf",
            "--enable-encoder=h264_qsv,hevc_qsv,av1_qsv,vp9_qsv",
            "--enable-encoder=h264_mf,hevc_mf",  # MediaFoundation
            # HW Decoders
            "--enable-decoder=h264,hevc,av1,vp9,mjpeg",
            "--enable-decoder=h264_qsv,hevc_qsv,av1_qsv,vp9_qsv",
            "--enable-decoder=h264_cuvid,hevc_cuvid,vp9_cuvid,av1_cuvid",
            # Filters
            "--enable-filter=scale,scale_qsv,vpp_qsv",
            "--enable-hwaccel=h264_nvdec,hevc_nvdec,av1_nvdec",
            "--enable-hwaccel=h264_d3d11va,hevc_d3d11va,av1_d3d11va",
        ]

        self.run(" ".join(conf), cwd=build_dir, env=env)
        self.run(f"{make_exe} -j16", cwd=build_dir, env=env)
        self.run(f"{make_exe} install", cwd=build_dir, env=env)


def compile_custom_ffmpeg(skip_updates=False):
    """Build FFmpeg from git master. Check for updates and rebuild if needed.

    Args:
        skip_updates: If True, don't check for git updates, use existing repo as-is.
    """
    if IS_LINUX:
        log("Running on Linux/WSL - using MSYS2 FFmpeg (downloaded from repo)")
        return  # FFmpeg is downloaded as part of MSYS2 packages

    # SEMI-HARDCODED: Skip FFmpeg setup if DLLs already exist in bin/ffmpeg
    ffmpeg_bin_dst = os.path.join(BIN_DIR, "ffmpeg")
    # Check for any avcodec DLL (version number varies)
    avcodec_dlls = glob.glob(os.path.join(ffmpeg_bin_dst, "avcodec-*.dll"))
    if avcodec_dlls:
        log("FFmpeg DLLs already exist in target ffmpeg dir - skipping whole FFmpeg setup to avoid permission locks.")
        return

    # Use internal builder
    builder = FFmpegBuilder(root_dir=PROJECT_ROOT, msys_dir=MSYS2_DIR, install_dir=FFMPEG_DIR)
    builder.setup_dirs()

    # Check if FFmpeg repo exists and get current commit for tracking
    ffmpeg_repo = os.path.join(builder.repos_dir, "ffmpeg")
    commit_file = os.path.join(builder.build_root, "last_built_commit.txt")

    # Determine if rebuild is needed
    needs_rebuild = False

    if not os.path.exists(os.path.join(FFMPEG_DIR, "lib", "libavcodec.dll.a")):
        # No built FFmpeg - definitely need to build
        needs_rebuild = True
        log("FFmpeg not built yet - building...")
    elif not os.path.exists(ffmpeg_repo):
        # Repo doesn't exist - need to clone and build
        needs_rebuild = True
        log("FFmpeg repo not found - cloning and building...")
    else:
        # Check for updates
        update = not skip_updates
        _, updated = builder.git_clone(FFMPEG_URL, "ffmpeg", update=update)

        if updated:
            needs_rebuild = True
            log("FFmpeg source updated - rebuilding...")
        else:
            # Check if last build matches current commit
            git_exe = builder.get_tool_path("git")
            current_commit = (
                subprocess.check_output(
                    [git_exe, "rev-parse", "HEAD"],
                    cwd=ffmpeg_repo,
                    env=builder.get_msys_env(),
                )
                .decode()
                .strip()
            )

            last_built = ""
            if os.path.exists(commit_file):
                with open(commit_file, "r") as f:
                    last_built = f.read().strip()

            if current_commit != last_built:
                needs_rebuild = True
                log(
                    f"FFmpeg commit changed ({last_built[:8] if last_built else 'none'}"
                    f" -> {current_commit[:8]}) - rebuilding..."
                )
            else:
                log(f"FFmpeg up to date ({current_commit[:8]}) - skipping build.")

    if not needs_rebuild:
        # Even if we don't rebuild, let's ensure DLLs are in bin/ffmpeg
        log("FFmpeg up to date, ensuring DLLs are in target ffmpeg dir...")
    else:
        log("Building Custom FFmpeg (this may take a while)...")
        try:
            builder.build_dependencies(update=not skip_updates)
            builder.build_ffmpeg(update=not skip_updates)

            # Save current commit hash for future comparison
            git_exe = builder.get_tool_path("git")
            current_commit = (
                subprocess.check_output(
                    [git_exe, "rev-parse", "HEAD"],
                    cwd=ffmpeg_repo,
                    env=builder.get_msys_env(),
                )
                .decode()
                .strip()
            )
            with open(commit_file, "w") as f:
                f.write(current_commit)
            log(f"Built FFmpeg commit: {current_commit[:8]}")
        except Exception as e:
            log(f"FFmpeg Build Failed: {e}")
            sys.exit(1)

    # Post-build (or if already built): Copy DLLs to bin/ffmpeg
    try:
        log("Copying FFmpeg DLLs...")
        ffmpeg_bin_src = os.path.join(FFMPEG_DIR, "bin")
        ffmpeg_bin_dst = os.path.join(BIN_DIR, "ffmpeg")
        os.makedirs(ffmpeg_bin_dst, exist_ok=True)

        for f in glob.glob(os.path.join(ffmpeg_bin_src, "*.dll")):
            shutil.copy(f, ffmpeg_bin_dst)
            log(f"Copied {os.path.basename(f)} to ffmpeg dir")

        # Copy MSYS2 runtime dependencies that FFmpeg DLLs need
        msys_bin = os.path.join(MSYS2_DIR, "clang64", "bin")
        runtime_deps = [
            "libiconv-2.dll",
            "libva_win32.dll",
            "libva.dll",
            "libvpl-2.dll",
            "libc++.dll",
            "libunwind.dll",  # libc++ often needs this
        ]
        for dep in runtime_deps:
            src = os.path.join(msys_bin, dep)
            if os.path.exists(src):
                shutil.copy(src, ffmpeg_bin_dst)
                log(f"Copied runtime dep {dep} to bin/ffmpeg")

        log("Custom FFmpeg Setup Complete.")
    except Exception as e:
        log(f"FFmpeg Setup Failed: {e}")
        sys.exit(1)


def get_env():
    if IS_LINUX:
        compilers = get_mingw_compilers()
        if compilers is None:
            log("ERROR: MinGW compiler discovery failed on Linux host")
            sys.exit(1)
        env = os.environ.copy()
        env["PATH"] = compilers["x64"]["bin"] + os.pathsep + env.get("PATH", "")
        env["CC"] = compilers["x64"]["cc"]
        env["CXX"] = compilers["x64"]["cxx"]
        env["PKG_CONFIG_PATH"] = ""
        env["DISABLE_CCACHE"] = "1"
        return env, compilers["x64"]["bin"]

    clang_bin = os.path.join(MSYS2_DIR, "clang64", "bin")
    usr_bin = os.path.join(MSYS2_DIR, "usr", "bin")
    env = os.environ.copy()
    env["PATH"] = clang_bin + os.pathsep + usr_bin + os.pathsep + env.get("PATH", "")
    env["PKG_CONFIG_PATH"] = os.path.join(MSYS2_DIR, "clang64", "lib", "pkgconfig")
    env["CCACHE_DIR"] = os.path.join(MSYS2_DIR, ".ccache")
    env["DISABLE_CCACHE"] = "1"
    return env, clang_bin


def get_env_x86():
    if IS_LINUX:
        compilers = get_mingw_compilers()
        if compilers is None:
            log("ERROR: MinGW compiler discovery failed on Linux host")
            sys.exit(1)
        env = os.environ.copy()
        x86_bin = compilers["x86"]["bin"]
        env["PATH"] = x86_bin + os.pathsep + env.get("PATH", "")
        env["CC"] = compilers["x86"]["cc"] or compilers["x64"]["cc"]
        env["CXX"] = compilers["x86"]["cxx"]
        env["PKG_CONFIG_PATH"] = ""
        env["DISABLE_CCACHE"] = "1"
        return env, x86_bin

    clang_bin = os.path.join(MSYS2_DIR, "mingw32", "bin")
    usr_bin = os.path.join(MSYS2_DIR, "usr", "bin")
    env = os.environ.copy()
    env["PATH"] = clang_bin + os.pathsep + usr_bin + os.pathsep + env.get("PATH", "")
    env["PKG_CONFIG_PATH"] = os.path.join(MSYS2_DIR, "mingw32", "lib", "pkgconfig")
    env["CCACHE_DIR"] = os.path.join(MSYS2_DIR, ".ccache")
    env["DISABLE_CCACHE"] = "1"
    return env, clang_bin


def ensure_dirs():
    os.makedirs(OBJ_DIR, exist_ok=True)
    os.makedirs(CAPTURE_BIN_DIR, exist_ok=True)
    os.makedirs(TESTAPP_BIN_DIR, exist_ok=True)
    os.makedirs(os.path.join(OBJ_DIR, "common"), exist_ok=True)
    os.makedirs(os.path.join(OBJ_DIR, "mediaengine"), exist_ok=True)
    os.makedirs(os.path.join(OBJ_DIR, "hook"), exist_ok=True)
    os.makedirs(os.path.join(OBJ_DIR, "captureengine"), exist_ok=True)
    os.makedirs(os.path.join(OBJ_DIR, "tests"), exist_ok=True)
    os.makedirs(os.path.join(OBJ_DIR, "external"), exist_ok=True)


def parse_dep_file(dep_path: str) -> List[str]:
    """Parse a GCC/Clang .d file."""
    deps = []
    try:
        with open(dep_path, "r") as f:
            content = f.read().replace("\\\n", " ").replace("\r", " ").strip()
            # Content is like "target: dep1 dep2 ...".
            # On Windows, drive letters also contain ':' ("C:/..."), so split on
            # the first ':' that is followed by whitespace.
            split_match = re.search(r":\s", content)
            if split_match:
                dep_part = content[split_match.end() :].strip()
                if dep_part:
                    # Preserve escaped spaces in dependency paths.
                    placeholder = "__CE_ESC_SPACE__"
                    dep_part = dep_part.replace("\\ ", placeholder)
                    files = dep_part.split()
                    deps = [f.strip().replace(placeholder, " ") for f in files if f.strip()]
    except Exception:
        pass
    return deps


def compute_build_signature(src: str, clang_exe: str, compile_flags: List[str]) -> str:
    """Create a stable signature from source contents + compiler/flags."""
    with open(src, "rb") as f:
        src_hash = hashlib.md5(f.read()).hexdigest()[:16]
    tool_fingerprint = "\n".join([os.path.abspath(clang_exe)] + compile_flags)
    tool_hash = hashlib.md5(tool_fingerprint.encode("utf-8")).hexdigest()[:16]
    return f"{src_hash}:{tool_hash}"


def should_recompile(
    src: str,
    obj: str,
    dep_file: str,
    env: Dict[str, str],
    clang_exe: str,
    compile_flags: List[str],
) -> bool:
    # Check for force rebuild flag
    if env.get("FORCE_REBUILD") == "1":
        return True
    if not os.path.exists(obj):
        return True

    # Check source timestamp
    try:
        src_mtime = os.path.getmtime(src)
        obj_mtime = os.path.getmtime(obj)
        if src_mtime > obj_mtime:
            return True
    except OSError:
        return True  # Error accessing files, safer to recompile

    # Also check if source content or compile settings changed (hash-based).
    # This catches stale objects when flags/compiler change across incremental runs.
    hash_file = obj + ".hash"
    try:
        signature = compute_build_signature(src, clang_exe, compile_flags)
        if os.path.exists(hash_file):
            with open(hash_file, "r") as f:
                stored_hash = f.read().strip()
            if signature != stored_hash:
                return True  # Content and/or flags changed, recompile
        else:
            # No hash file, need to create one (first compile or old build)
            return True
    except Exception:
        pass  # Fall back to timestamp check only

    # Check dependencies
    if os.path.exists(dep_file):
        deps = parse_dep_file(dep_file)
        if not deps:
            # Invalid/empty dep file can miss header changes and cause ABI skew.
            return True
        obj_mtime = os.path.getmtime(obj)
        for dep in deps:
            try:
                if os.path.exists(dep) and os.path.getmtime(dep) > obj_mtime:
                    return True
            except OSError:
                return True  # Error accessing dependency, safer to recompile
    else:
        # If object exists but dep file is missing, recompile to generate dep file
        return True

    return False


def normalize_compile_command_arg(arg: str) -> str:
    """Normalize compile_commands argument paths for clangd compatibility."""
    if IS_WINDOWS:
        return arg.replace("\\", "/")
    return arg


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
    if clang_exe:
        detected = run_command([os.path.normpath(clang_exe), "--print-resource-dir"], env=env, fail_exit=False).strip()
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
        with open(hash_file, "w") as f:
            f.write(compute_build_signature(src, clang_exe, compile_flags))
    except Exception:
        pass  # Non-critical, ignore errors

    return True


def parallel_compile(env, clang_exe, cflags, src_obj_pairs):
    """Compile multiple source files in parallel."""
    requested_workers = env.get("CE_BUILD_JOBS", "").strip()
    if requested_workers:
        try:
            num_workers = int(requested_workers)
        except ValueError:
            log(f"Warning: invalid CE_BUILD_JOBS={requested_workers!r}; using auto worker count")
            num_workers = min(cpu_count(), 8)
    else:
        # Keep memory pressure bounded during LTO and dual-arch builds.
        num_workers = min(cpu_count(), 8)
    num_workers = max(1, min(num_workers, len(src_obj_pairs) or 1))
    compiled = 0
    skipped = 0

    def compile_one(args):
        src, obj = args
        os.makedirs(os.path.dirname(obj), exist_ok=True)
        return compile_object(env, clang_exe, cflags, src, obj), obj

    with ThreadPoolExecutor(max_workers=num_workers) as executor:
        futures = {executor.submit(compile_one, pair): pair for pair in src_obj_pairs}
        for future in as_completed(futures):
            was_compiled, obj = future.result()
            if was_compiled:
                compiled += 1
            else:
                skipped += 1

    return compiled, skipped


def compile_tests(env, clang_exe, cflags, common_objs, pkg_config, obj_dir):
    log(f"Compiling Tests (parallel, {cpu_count()} threads)...")
    src_files = glob.glob(os.path.join(PROJECT_ROOT, "tests", "*.cpp"))
    if not src_files:
        log("No test files found.")
        return

    tests_dir = os.path.join(PROJECT_ROOT, "tests")
    os.makedirs(tests_dir, exist_ok=True)
    test_exe = os.path.join(tests_dir, "unit_tests.exe")

    # 1. Get FFmpeg flags
    if IS_LINUX:
        # Use system FFmpeg on Linux
        ffmpeg_cflags, ffmpeg_libs = get_system_ffmpeg_flags()
        ffmpeg_flags = ffmpeg_cflags + ffmpeg_libs
    else:
        # Use local FFmpeg on Windows
        env_ffmpeg = env.copy()
        env_ffmpeg["PKG_CONFIG_PATH"] = (
            os.path.join(FFMPEG_DIR, "lib", "pkgconfig") + os.pathsep + env_ffmpeg.get("PKG_CONFIG_PATH", "")
        )

        pkgs = ["libavcodec", "libavformat", "libavutil", "libswresample", "libswscale"]
        pkg_cmd = [
            pkg_config,
            "--cflags",
            "--libs",
        ] + pkgs  # Removed --static for shared linking
        ffmpeg_flags_raw = run_command(pkg_cmd, env=env_ffmpeg).strip().split()
        ffmpeg_flags = [f for f in ffmpeg_flags_raw if f not in ["-ldl", "-lshaderc_shared"]]

    # Link against gtest, common, hook, mediaengine, and ffmpeg
    # Add VPL for QSV symbols, ole32/gdi32/uuid as VPL deps
    ldflags_test = [
        "-static-libgcc",
        "-static-libstdc++",
        "-Wl,--allow-multiple-definition",
        "-lgtest",
        "-lgtest_main",
        "-lwinmm",
        "-lshlwapi",
        "-ld3d11",
        "-ld3d12",
        "-ldxgi",
        "-ld3dcompiler",
        "-lgdi32",
        "-luser32",
        "-ldwmapi",
        "-ldbghelp",
        "-lavrt",
        "-lpdh",
        "-lshcore",
        "-lole32",
        "-lmfplat",
        "-lmfuuid",
        "-lbcrypt",
        "-lsecur32",
        "-lws2_32",
        "-lmmdevapi",
        "-lvpl",
        "-luuid",  # VPL for QSV
        "-lshaderc_combined",
        "-lglslang",
        "-lSPIRV-Tools",
        "-lSPIRV-Tools-opt",
        "-lSPIRV-Tools-link",
        "-lMachineIndependent",
        "-lGenericCodeGen",
        "-lOSDependent",
        "-lSPIRV",
        "-ljxl_cms",
        "-ljxl_threads",
        "-lhwy",
        "-llcms2",
        "-ltasn1",
        "-lnettle",
        "-lhogweed",
        "-lgmp",
        "-lpangocairo-1.0",
        "-lpangowin32-1.0",
        "-lpangoft2-1.0",
        "-lpango-1.0",
        "-lharfbuzz",
        "-lfreetype",
        "-lgraphite2",
        "-lfribidi",
        "-lthai",
        "-ldatrie",
        "-lintl",
        "-lfontconfig",
        "-lexpat",
        "-lcairo-gobject",
        "-lpixman-1",
        "-lffi",
        "-lpcre2-8",
        "-lgmodule-2.0",
        "-lssl",
        "-lcrypto",
        "-lsharpyuv",
        "-lcrypt32",
        "-lncrypt",
        "-lntdll",
        "-luserenv",
        "-lwinmm",
        "-liphlpapi",
        "-lgdiplus",
        "-lshlwapi",
        "-lrpcrt4",
        "-ldwrite",
        "-ldnsapi",
        "-lmsimg32",
        "-lbrotlienc",
        "-lbrotlidec",
        "-lbrotlicommon",
        "-lz",
        "-llzma",
        "-lbz2",
        "-liconv",
        "-lunistring",
        "-lzstd",
        "-lidn2",
    ] + ffmpeg_flags
    if any(flag.startswith("-fsanitize=") for flag in cflags):
        ldflags_test.append("-fsanitize=address,undefined")

    # 2. Compile MediaEngine objects for tests
    me_src = glob.glob(os.path.join(PROJECT_ROOT, "mediaengine", "*.cpp"))
    me_objs = []
    src_obj_pairs = []
    # We need to compile MediaEngine with MEDIAENGINE_EXPORTS or similar if needed,
    # but for static linking in tests, we just need the symbols.
    # Note: AudioEncoder.cpp might rely on specific defines.
    me_cflags = cflags + ffmpeg_flags + ["-DMEDIAENGINE_EXPORTS"]

    for src in me_src:
        rel_path = os.path.relpath(src, PROJECT_ROOT)
        obj = os.path.join(obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
        src_obj_pairs.append((src, obj))
        me_objs.append(obj)

    parallel_compile(env, clang_exe, me_cflags, src_obj_pairs)

    # 3. Compile Tests
    test_cflags = cflags + [
        "-I" + os.path.join(PROJECT_ROOT, "mediaengine"),
        "-I" + os.path.join(PROJECT_ROOT, "hook", "wrappers"),
        "-I" + os.path.join(PROJECT_ROOT, "hook", "common"),
    ]  # Ensure we can include audio_encoder.h and hook headers for stubs
    test_objs = []
    src_obj_pairs = []
    for src in src_files:
        rel_path = os.path.relpath(src, PROJECT_ROOT)
        obj = os.path.join(obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
        src_obj_pairs.append((src, obj))
        test_objs.append(obj)

    parallel_compile(env, clang_exe, test_cflags, src_obj_pairs)

    # 4. Compile hook/common for tests
    hook_common_src = glob.glob(os.path.join(PROJECT_ROOT, "hook", "common", "*.cpp"))
    hook_common_objs = []
    src_obj_pairs = []
    for src in hook_common_src:
        rel_path = os.path.relpath(src, PROJECT_ROOT)
        obj = os.path.join(obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
        src_obj_pairs.append((src, obj))
        hook_common_objs.append(obj)
    parallel_compile(env, clang_exe, cflags, src_obj_pairs)

    log("Linking Unit Tests...")
    # Order: Tests -> Common -> MediaEngine -> HookCommon -> Libs
    cmd = [clang_exe] + test_objs + common_objs + me_objs + hook_common_objs + ldflags_test + ["-o", test_exe]
    run_command(cmd, env=env)
    copy_test_runtime_dlls(tests_dir)
    return test_exe


def copy_test_runtime_dlls(tests_dir):
    """Copy libgtest.dll and FFmpeg DLLs next to unit_tests.exe so it can be run directly."""
    import shutil

    msys_bin = os.path.join(PROJECT_ROOT, "build", "msys64", "clang64", "bin")
    ffmpeg_bin = os.path.join(PROJECT_ROOT, "installed", "captureengine", "ffmpeg")
    copied = []
    for dll_dir in [msys_bin, ffmpeg_bin]:
        if not os.path.isdir(dll_dir):
            continue
        for dll in os.listdir(dll_dir):
            if not dll.lower().endswith(".dll"):
                continue
            src = os.path.join(dll_dir, dll)
            dst = os.path.join(tests_dir, dll)
            if not os.path.exists(dst) or os.path.getmtime(src) > os.path.getmtime(dst):
                shutil.copy2(src, dst)
                copied.append(dll)
    if copied:
        log(f"Copied {len(copied)} runtime DLLs to tests/ for direct execution")


def run_tests(env, test_exe):
    log("=== Running Unit Tests ===")
    if not os.path.exists(test_exe):
        log("Error: Test executable not found.")
        return False

    # Ensure required DLLs are on PATH (libgtest.dll, FFmpeg DLLs)
    msys_bin = os.path.join(PROJECT_ROOT, "build", "msys64", "clang64", "bin")
    ffmpeg_dir = os.path.join(PROJECT_ROOT, "installed", "captureengine", "ffmpeg")
    test_env = dict(env)
    test_env["PATH"] = msys_bin + os.pathsep + ffmpeg_dir + os.pathsep + test_env.get("PATH", "")

    cmd = [test_exe]
    result = subprocess.run(cmd, env=test_env)
    if result.returncode != 0:
        log(f"=== Unit Tests FAILED (exit code {result.returncode}) ===")
        return False
    else:
        log("=== Unit Tests Passed ===")
        return True


def run_integration_tests(env, full_matrix=False):
    mode = "Full Matrix" if full_matrix else "Smoke"
    log(f"=== Running Integration Tests ({mode}) ===")
    script = os.path.join(PROJECT_ROOT, "testapp", "run_tests.py")
    if not os.path.exists(script):
        log(f"Error: {script} not found.")
        return

    logs_dir = os.path.join(PROJECT_ROOT, "installed", "captureengine", "logs")
    os.makedirs(logs_dir, exist_ok=True)

    base_cmd = [
        sys.executable,
        script,
        "--duration",
        "5",
        "--tests",
        "1",
        "--min-frames",
        "60",
    ]

    if full_matrix:
        targets = [
            ("full_matrix", ["--api", "all", "--arch", "both"]),
        ]
    else:
        targets = [
            ("smoke_dx9", ["--api", "dx9", "--arch", "both"]),
        ]

    for label, args in targets:
        result_json = os.path.join(logs_dir, f"integration_{label}.json")
        cmd = base_cmd + args + ["--results-json", result_json]
        log(f"Executing: {' '.join(cmd)}")
        result = subprocess.run(cmd, cwd=os.path.dirname(script), env=env)
        if result.returncode != 0:
            log(f"ERROR: Integration test target failed: {label}")
            sys.exit(1)

    log("=== Integration Tests Passed ===")


def run_lint(env):
    log("=== Running Linting ===")
    checks_ok = True

    # 1. C++ Linting (clang-format)
    clang_format = None
    if IS_LINUX:
        clang_format = shutil.which("clang-format")
    else:
        clang_format = os.path.join(MSYS2_DIR, "clang64", "bin", "clang-format.exe")

    if clang_format and (IS_LINUX or os.path.exists(clang_format)):
        log("Running clang-format...")

        files = []
        dirs_to_lint = [
            "common",
            "hook",
            "captureengine",
            "mediaengine",
            "testapp",
            "tests",
        ]

        for d in dirs_to_lint:
            root_path = os.path.join(PROJECT_ROOT, d)
            for root, _, filenames in os.walk(root_path):
                for f in filenames:
                    if f.endswith((".cpp", ".h", ".hpp", ".c")):
                        path = os.path.join(root, f)
                        if "external" in path or "imgui" in path:
                            continue
                        files.append(path)

        if files:
            chunk_size = 50
            issues_found = 0

            for i in range(0, len(files), chunk_size):
                chunk = files[i : i + chunk_size]
                cmd = [clang_format, "--dry-run", "-Werror"] + chunk
                res = subprocess.run(cmd, capture_output=True, text=True, env=env)
                if res.returncode != 0:
                    issues_found += 1

            if issues_found > 0:
                log(f"WARNING: C++ Style issues found in {issues_found} batches.")
                log("Run 'python build.py --format' to fix them automatically.")
                checks_ok = False
            else:
                log("C++ Style: OK")
    else:
        log("Error: clang-format not found.")
        checks_ok = False

    # 2. Python Linting (flake8)
    # Check if flake8 is installed in host python
    try:
        subprocess.run(
            [sys.executable, "-m", "flake8", "--version"],
            capture_output=True,
            check=True,
        )
        has_flake8 = True
    except Exception:
        has_flake8 = False

    if has_flake8:
        log("Running flake8...")
        # Lint build scripts and test scripts
        # We need to specify paths explicitly to avoid traversing build/ directories if exclude fails
        py_targets = ["build.py", "testapp"]

        cmd = [sys.executable, "-m", "flake8"] + py_targets
        res = subprocess.run(cmd, capture_output=True, text=True)

        if res.returncode != 0:
            log("Python Style Issues:")
            log(res.stdout)
            log("Python Style: FAILED")
            checks_ok = False
        else:
            log("Python Style: OK")
    else:
        log("Error: flake8 not installed. (Run 'pip install flake8')")
        checks_ok = False

    # 3. Python type/LSP check (pyright)
    try:
        subprocess.run(
            [sys.executable, "-m", "pyright", "--version"],
            capture_output=True,
            check=True,
        )
        has_pyright = True
    except Exception:
        has_pyright = False

    if has_pyright:
        log("Running pyright...")
        cmd = [
            sys.executable,
            "-m",
            "pyright",
            "-p",
            os.path.join(PROJECT_ROOT, "pyrightconfig.json"),
        ]
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            log("Python Type Issues:")
            if res.stdout:
                log(res.stdout)
            if res.stderr:
                log(res.stderr)
            log("Python Types: FAILED")
            checks_ok = False
        else:
            log("Python Types: OK")
    else:
        log("Error: pyright not installed. (Run 'pip install pyright')")
        checks_ok = False

    return checks_ok


def run_format(env):
    log("=== Running Auto-Format ===")
    format_ok = True

    # 1. C++ Format (clang-format -i)
    clang_format = None
    if IS_LINUX:
        clang_format = shutil.which("clang-format")
    else:
        clang_format = os.path.join(MSYS2_DIR, "clang64", "bin", "clang-format.exe")

    if clang_format and (IS_LINUX or os.path.exists(clang_format)):
        log("Formatting C++ files...")
        files = []
        dirs_to_lint = [
            "common",
            "hook",
            "captureengine",
            "mediaengine",
            "testapp",
            "tests",
        ]
        for d in dirs_to_lint:
            root_path = os.path.join(PROJECT_ROOT, d)
            for root, _, filenames in os.walk(root_path):
                for f in filenames:
                    if f.endswith((".cpp", ".h", ".hpp", ".c")):
                        path = os.path.join(root, f)
                        if "external" in path or "imgui" in path:
                            continue
                        files.append(path)

        if files:
            chunk_size = 50
            for i in range(0, len(files), chunk_size):
                chunk = files[i : i + chunk_size]
                cmd = [clang_format, "-i"] + chunk
                subprocess.run(cmd, env=env, check=True)
            log("C++ files formatted.")
    else:
        log("Error: clang-format not found.")
        format_ok = False

    # 2. Python format (black)
    try:
        subprocess.run(
            [sys.executable, "-m", "black", "--version"],
            capture_output=True,
            check=True,
        )
        has_black = True
    except Exception:
        has_black = False

    if has_black:
        log("Formatting Python files...")
        py_targets = ["build.py", "testapp"]
        cmd = [sys.executable, "-m", "black", "--line-length", "120"] + py_targets
        res = subprocess.run(cmd, capture_output=True, text=True)
        if res.returncode != 0:
            log("Python format failed:")
            if res.stdout:
                log(res.stdout)
            if res.stderr:
                log(res.stderr)
            format_ok = False
        else:
            log("Python files formatted.")
    else:
        log("Error: black not found. (Run 'pip install black')")
        format_ok = False

    return format_ok


def compile_testapps(env, x86_env, clang_exe, cflags):
    """Compile test applications using Clang (and x86 if available)"""
    log("Compiling Test Applications...")

    testapp_src_dir = os.path.join(PROJECT_ROOT, "testapp")
    testapp_bin_dir = TESTAPP_BIN_DIR
    os.makedirs(testapp_bin_dir, exist_ok=True)

    # Optional: also build 32-bit variants if 32-bit toolchain is available.
    x86_bin_dir = os.path.join(testapp_bin_dir, "x86")
    os.makedirs(x86_bin_dir, exist_ok=True)

    # Determine x86 compiler availability
    if IS_LINUX:
        clang_exe_x86 = get_compiler_exe("x86")
        have_x86 = clang_exe_x86 is not None
    else:
        clang_exe_x86 = os.path.join(MSYS2_DIR, "mingw32", "bin", "clang++.exe")
        have_x86 = os.path.exists(clang_exe_x86)

    if env.get("CE_SANITIZE") == "1":
        have_x86 = False
        log("Sanitizer mode: skipping x86 test applications (ASan runtime unavailable)")

    cflags_x86 = [f for f in cflags if f != "-flto"]

    tasks = []

    ccache_exe = shutil.which("ccache", path=env.get("PATH", ""))
    if env.get("DISABLE_CCACHE"):
        ccache_exe = None

    def add_task(desc, cmd, cwd=None, task_env=env):
        tasks.append((desc, cmd, cwd, task_env))

    def make_cmd(compiler, flags, source, linker_flags, output):
        cmd_base = [compiler] + flags + [source] + linker_flags + ["-o", output]
        if ccache_exe:
            return [ccache_exe, os.path.basename(compiler)] + flags + [source] + linker_flags + ["-o", output]
        return cmd_base

    # Get vulkan lib path
    msys2_dir = get_linux_msys2_dir() if IS_LINUX else MSYS2_DIR
    vulkan_lib = os.path.join(msys2_dir, "clang64", "lib", "libvulkan-1.dll.a")
    vulkan_lib_x86 = os.path.join(msys2_dir, "mingw32", "lib", "libvulkan-1.dll.a")

    # DX12 Test App
    dx12_src = os.path.join(testapp_src_dir, "dx12_test.cpp")
    dx12_exe = os.path.join(testapp_bin_dir, "dx12_test.exe")
    if os.path.exists(dx12_src):
        dx12_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
            "-ld3d12",
            "-ldxgi",
            "-ld3dcompiler",
            "-lgdi32",
            "-luser32",
            "-lshcore",
            "-lavrt",
        ]
        add_task(
            "dx12_test.exe",
            make_cmd(clang_exe, cflags, dx12_src, dx12_ldflags, dx12_exe),
        )

        if have_x86:
            dx12_exe_x86 = os.path.join(x86_bin_dir, "dx12_test.exe")
            add_task(
                "dx12_test.exe (x86)",
                make_cmd(clang_exe_x86, cflags_x86, dx12_src, dx12_ldflags, dx12_exe_x86),
            )

    # DX11 Test App
    dx11_src = os.path.join(testapp_src_dir, "dx11_test.cpp")
    dx11_exe = os.path.join(testapp_bin_dir, "dx11_test.exe")
    if os.path.exists(dx11_src):
        dx11_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
            "-ld3d11",
            "-ldxgi",
            "-lgdi32",
            "-luser32",
            "-lshcore",
            "-lavrt",
        ]
        add_task(
            "dx11_test.exe",
            make_cmd(clang_exe, cflags, dx11_src, dx11_ldflags, dx11_exe),
        )

        if have_x86:
            dx11_exe_x86 = os.path.join(x86_bin_dir, "dx11_test.exe")
            add_task(
                "dx11_test.exe (x86)",
                make_cmd(clang_exe_x86, cflags_x86, dx11_src, dx11_ldflags, dx11_exe_x86),
            )

    # DX9 Test App
    dx9_src = os.path.join(testapp_src_dir, "dx9_test.cpp")
    dx9_exe = os.path.join(testapp_bin_dir, "dx9_test.exe")
    if os.path.exists(dx9_src):
        dx9_ldflags = ["-static", "-Wl,--subsystem,windows", "-ld3d9", "-lgdi32", "-luser32", "-lavrt"]
        add_task("dx9_test.exe", make_cmd(clang_exe, cflags, dx9_src, dx9_ldflags, dx9_exe))

        if have_x86:
            dx9_exe_x86 = os.path.join(x86_bin_dir, "dx9_test.exe")
            add_task(
                "dx9_test.exe (x86)",
                make_cmd(clang_exe_x86, cflags_x86, dx9_src, dx9_ldflags, dx9_exe_x86),
            )

    # DX10 Test App
    dx10_src = os.path.join(testapp_src_dir, "dx10_test.cpp")
    dx10_exe = os.path.join(testapp_bin_dir, "dx10_test.exe")
    if os.path.exists(dx10_src):
        dx10_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
            "-ld3d10",
            "-ldxgi",
            "-ld3dcompiler",
            "-lgdi32",
            "-luser32",
            "-lshcore",
            "-lavrt",
        ]
        add_task(
            "dx10_test.exe",
            make_cmd(clang_exe, cflags, dx10_src, dx10_ldflags, dx10_exe),
        )

        if have_x86:
            dx10_exe_x86 = os.path.join(x86_bin_dir, "dx10_test.exe")
            add_task(
                "dx10_test.exe (x86)",
                make_cmd(clang_exe_x86, cflags_x86, dx10_src, dx10_ldflags, dx10_exe_x86),
            )

    # Vulkan Test App
    vulkan_src = os.path.join(testapp_src_dir, "vulkan_test.cpp")
    vulkan_exe = os.path.join(testapp_bin_dir, "vulkan_test.exe")
    if os.path.exists(vulkan_src):
        vulkan_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
            vulkan_lib,
            "-lgdi32",
            "-luser32",
            "-lshcore",
        ]
        add_task(
            "vulkan_test.exe",
            make_cmd(clang_exe, cflags, vulkan_src, vulkan_ldflags, vulkan_exe),
        )

        if have_x86:
            vulkan_exe_x86 = os.path.join(x86_bin_dir, "vulkan_test.exe")
            vulkan_ldflags_x86 = [
                "-static",
                "-Wl,--subsystem,windows",
                vulkan_lib_x86,
                "-lgdi32",
                "-luser32",
                "-lshcore",
            ]
            add_task(
                "vulkan_test.exe (x86)",
                make_cmd(
                    clang_exe_x86,
                    cflags_x86,
                    vulkan_src,
                    vulkan_ldflags_x86,
                    vulkan_exe_x86,
                ),
            )

    # OpenGL Test App
    opengl_src = os.path.join(testapp_src_dir, "opengl_test.cpp")
    opengl_exe = os.path.join(testapp_bin_dir, "opengl_test.exe")
    if os.path.exists(opengl_src):
        opengl_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
            "-lopengl32",
            "-lglu32",
            "-lgdi32",
            "-luser32",
            "-lshcore",
            "-lavrt",
        ]

        add_task(
            "opengl_test.exe",
            make_cmd(clang_exe, cflags, opengl_src, opengl_ldflags, opengl_exe),
        )

        if have_x86:
            opengl_exe_x86 = os.path.join(x86_bin_dir, "opengl_test.exe")
            cmd = [clang_exe_x86] + cflags_x86 + [opengl_src] + opengl_ldflags + ["-o", opengl_exe_x86]
            add_task("opengl_test.exe (x86)", cmd)

    # Execute all tasks in parallel
    if not tasks:
        return

    # Record tasks for compile_commands.json
    for desc, cmd, cwd, tenv in tasks:
        # Find the source file in the command arguments
        src_file = None
        for arg in cmd:
            if arg.endswith(".cpp"):
                src_file = arg
                break

        if src_file:
            COMPILE_COMMANDS.append(
                {
                    "directory": PROJECT_ROOT,
                    "arguments": cmd,
                    "file": src_file,
                }
            )

    def compile_app(t):
        desc, cmd, cwd, tenv = t
        log(f"Compiling {desc}...")
        try:
            subprocess.run(cmd, env=tenv, cwd=cwd, check=True, capture_output=True, text=True)
            log(f"Built: {desc}")
        except subprocess.CalledProcessError as e:
            log(f"ERROR compiling {desc}:")
            log(e.stdout)
            log(e.stderr)
            raise e

    log(f"Compiling {len(tasks)} Test Apps in parallel...")
    errors = []
    with ThreadPoolExecutor(max_workers=cpu_count()) as executor:
        futures = [executor.submit(compile_app, t) for t in tasks]
        for future in as_completed(futures):
            try:
                future.result()
            except Exception as e:
                errors.append(e)

    if errors:
        log(f"Warning: {len(errors)} test app(s) failed to compile")


def compile_vulkan_layer(env, clang_exe, cflags, arch):
    """Compile VK_LAYER_CE_overlay - Vulkan implicit layer for overlay and capture"""
    log(f"Compiling Vulkan Layer ({arch})...")

    layer_dir = os.path.join(PROJECT_ROOT, "hook", "vulkan_layer")
    bin_dir = CAPTURE_BIN_DIR
    obj_dir = os.path.join(PROJECT_ROOT, "build", "obj", arch, "vulkan_layer")
    os.makedirs(obj_dir, exist_ok=True)

    # Layer source files
    layer_sources = [
        os.path.join(layer_dir, "layer_main.cpp"),
        os.path.join(layer_dir, "vulkan_layer.cpp"),
        os.path.join(layer_dir, "layer_ipc.cpp"),
        os.path.join(layer_dir, "layer_overlay.cpp"),
        os.path.join(layer_dir, "layer_capture.cpp"),
        os.path.join(layer_dir, "layer_bridge.cpp"),
        os.path.join(layer_dir, "layer_hooks.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "fg_detection.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "ipc_client.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "system_metrics.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "performance_metrics.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "perf_logger.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "input_manager.cpp"),
        # CustomOverlay system for full overlay rendering
        os.path.join(PROJECT_ROOT, "hook", "common", "custom_overlay.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "custom_overlay_vk.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "custom_font.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "overlay_adapter.cpp"),
        os.path.join(PROJECT_ROOT, "hook", "common", "cached_overlay_renderer.cpp"),
    ]

    # Compile layer sources
    layer_cflags = cflags + [
        "-I" + layer_dir,
        "-I" + os.path.join(PROJECT_ROOT, "common"),
        "-I" + os.path.join(PROJECT_ROOT, "hook", "common"),
        "-DVK_NO_PROTOTYPES",
        "-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES",
        "-DVK_USE_PLATFORM_WIN32_KHR",
        "-DVK_LAYER_CE_OVERLAY",
    ]

    # Add Vulkan headers include path (from MSYS2 on Linux)
    if IS_LINUX:
        vulkan_include = os.path.join(get_linux_msys2_dir(), "clang64", "include")
        if os.path.exists(vulkan_include):
            layer_cflags.append("-I" + vulkan_include)

    layer_objs = []

    # Compile all sources in parallel
    src_obj_pairs = []

    # helper to add sources
    def add_sources(sources, dest_obj_dir):
        for src in sources:
            if not os.path.exists(src):
                log(f"Warning: Layer source not found: {src}")
                continue
            basename = os.path.splitext(os.path.basename(src))[0]
            obj = os.path.join(dest_obj_dir, basename + ".o")
            src_obj_pairs.append((src, obj))
            layer_objs.append(obj)

    add_sources(layer_sources, obj_dir)

    if not src_obj_pairs:
        log("Error: No layer sources found.")
        return

    # Run parallel compilation
    compiled, skipped = parallel_compile(env, clang_exe, layer_cflags, src_obj_pairs)
    if compiled > 0:
        log(f"Vulkan Layer ({arch}): compiled {compiled}, skipped {skipped}")

    # Link layer DLL
    if arch == "x64":
        layer_dll_name = "VK_LAYER_CE_overlay.dll"
    else:
        layer_dll_name = "VK_LAYER_CE_overlay_x86.dll"

    # Use MSYS2 Vulkan import library
    if arch == "x64":
        vulkan_lib = os.path.join(get_linux_msys2_dir(), "clang64", "lib", "libvulkan-1.dll.a")
    else:
        vulkan_lib = os.path.join(get_linux_msys2_dir(), "mingw32", "lib", "libvulkan-1.dll.a")

    layer_dll = os.path.join(bin_dir, layer_dll_name)

    ldflags = [
        "-shared",
        "-static",
        vulkan_lib,
        "-lgdi32",
        "-luser32",
        "-lpdh",
        "-ldxgi",
        "-lshcore",
        "-lwinmm",
        "-lversion",
        "-o",
        layer_dll,
    ]

    ldflags.extend(LD_OPT_FLAGS)  # Strip debug sections, reduce binary size
    if arch == "x86":
        ldflags.append("-Wl,--kill-at")

        # Re-add -static for proper linking
        if "-static" not in ldflags:
            ldflags.insert(0, "-static")

        # Detect if we're using Clang or GCC
        # Clang on mingw32 defaults to libc++ which may not be available
        # GCC uses libstdc++ by default and doesn't support -stdlib flag
        is_clang = "clang" in os.path.basename(clang_exe).lower()

        if is_clang:
            # x86 mingw32 clang++ defaults to libc++ but mingw32 only has libstdc++
            # Rebuild objects with -stdlib=libstdc++ to use available stdlib
            log("[INFO] Rebuilding x86 Vulkan layer with -stdlib=libstdc++ (Clang detected)")

            # Rebuild all layer objects with -stdlib=libstdc++
            x86_layer_cflags = layer_cflags + ["-stdlib=libstdc++"]
            rebuilt_objs = []
            for src in layer_sources:
                if not os.path.exists(src):
                    continue
                basename = os.path.splitext(os.path.basename(src))[0]
                obj = os.path.join(obj_dir, basename + "_stdc++.o")
                cmd = [clang_exe] + x86_layer_cflags + ["-c", src, "-o", obj]
                run_command(cmd, env=env)
                rebuilt_objs.append(obj)

            # Update layer_objs with rebuilt objects
            layer_objs[:] = rebuilt_objs
        else:
            # GCC uses libstdc++ by default - no need for -stdlib flag
            log("[INFO] x86 Vulkan layer using GCC with default libstdc++")

    # Use ccache for linking too if available
    ccache_exe = shutil.which("ccache", path=env["PATH"])
    if env.get("DISABLE_CCACHE"):
        ccache_exe = None

    if ccache_exe:
        cmd = [ccache_exe, os.path.basename(clang_exe)] + layer_objs + ldflags
    else:
        cmd = [clang_exe] + layer_objs + ldflags

    # Robust handling for locked DLLs (DataExchangeHost, explorer, etc.)
    if os.path.exists(layer_dll):
        if not safe_delete_file(layer_dll):
            # Even if we can't delete, we can still build if we renamed it
            # Check if the file still exists with original name
            if os.path.exists(layer_dll):
                log(f"[Warning] {os.path.basename(layer_dll)} is still locked, build may fail")
                # Check if locked and log helpful info
                if is_file_locked(layer_dll):
                    log("[Info] File is actively locked by another process")
                    locking = find_process_locking_file(layer_dll)
                    if locking:
                        log(f"[Info] Locking process: {locking}")

    try:
        run_command(cmd, env=env)
        log(f"Built: {layer_dll}")

        # Generate layer manifest JSON dynamically
        # This ensures the path is always correct and current
        import json

        manifest_name = "VK_LAYER_CE_overlay.json" if arch == "x64" else "VK_LAYER_CE_overlay_x86.json"
        manifest_path = os.path.join(bin_dir, manifest_name)

        # Keep manifest portable/private by using a DLL name relative to the
        # manifest location instead of an absolute machine-local path.
        # Prefix with .\ so Vulkan loader resolves relative to manifest path.
        # Bare DLL names are resolved via process DLL search paths and can fail.
        manifest_dll_name = f".\\{os.path.basename(layer_dll)}"

        # Use different layer names for x64 vs x86 to avoid "wrong bit-type" conflicts
        # Both 32-bit and 64-bit apps read from HKCU (no WOW64 redirection for HKCU)
        layer_name = "VK_LAYER_CE_overlay" if arch == "x64" else "VK_LAYER_CE_overlay_x86"

        manifest = {
            "file_format_version": "1.2.0",
            "layer": {
                "name": layer_name,
                "type": "GLOBAL",
                "library_path": manifest_dll_name,
                "api_version": "1.3.0",
                "implementation_version": "1",
                "description": "CaptureEngine Overlay and Recording Layer",
                "functions": {
                    "vkGetInstanceProcAddr": "vkGetInstanceProcAddr",
                    "vkGetDeviceProcAddr": "vkGetDeviceProcAddr",
                    "vkNegotiateLoaderLayerInterfaceVersion": "vkNegotiateLoaderLayerInterfaceVersion",
                },
                "disable_environment": {"DISABLE_CE_VULKAN_LAYER": "1"},
            },
        }

        with open(manifest_path, "w") as f:
            json.dump(manifest, f, indent=4)

        log(f"Generated Manifest: {manifest_path}")

        # CLEANUP: Ensure this layer is NOT registered globally in the registry.
        # It should ONLY be registered ephemerally by captureengine.exe at runtime.
        if IS_WINDOWS:
            try:
                import winreg

                key_path = r"Software\Khronos\Vulkan\ImplicitLayers"
                try:
                    key = winreg.OpenKey(
                        winreg.HKEY_CURRENT_USER,
                        key_path,
                        0,
                        winreg.KEY_SET_VALUE | winreg.KEY_READ,
                    )
                    # Check if value exists and delete it
                    try:
                        winreg.DeleteValue(key, manifest_path)
                        log(f"Cleaned legacy registry key for: {manifest_name}")
                    except FileNotFoundError:
                        pass  # Key doesn't exist, good.
                    winreg.CloseKey(key)
                except FileNotFoundError:
                    pass  # Interface key doesn't exist, good.
            except Exception as e:
                log(f"Warning: Failed to clean registry keys: {e}")

    except Exception as e:
        log(f"Error linking layer: {e}")


def compile_project(env, clang_bin, skip_updates=False, should_run_tests=False):
    ensure_dirs()

    compile_custom_ffmpeg(skip_updates=skip_updates)
    clang_exe = get_compiler_exe("x64")
    pkg_config = shutil.which("pkg-config") if IS_LINUX else os.path.join(clang_bin, "pkg-config.exe")

    cflags = (
        [
            "-std=c++20",
        ]
        + OPT_FLAGS_X64
        + [
            "-Wall",
            "-Wextra",
            "-Wshadow",
            "-Wformat=2",
            "-Wundef",
            "-Wno-unused-parameter",
            "-D_WIN32_WINNT=0x0A00",
            "-I" + os.path.join(PROJECT_ROOT, "common"),
        ]
        + (["-DCE_PRODUCTION_BUILD=1"] if env.get("CE_PRODUCTION_BUILD") == "1" else [])
    )

    # Add MSYS2 include path on Linux for Windows headers (cppwinrt, etc.)
    if IS_LINUX:
        msys2_dir = get_linux_msys2_dir()
        msys2_include = os.path.join(msys2_dir, "clang64", "include")
        if os.path.exists(msys2_include):
            cflags.append("-I" + msys2_include)

    # --- Architecture Loop ---
    arch_targets = ["x64", "x86"]
    if env.get("CE_SANITIZE") == "1":
        # MSYS2 currently ships ASan runtime only for x64 clang target.
        # Building x86 sanitizer binaries fails at link time.
        log("Sanitizer mode: skipping x86 targets (ASan runtime unavailable)")
        arch_targets = ["x64"]

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
            for _flag in ("FORCE_REBUILD",):
                if _flag in env:
                    curr_env[_flag] = env[_flag]

        curr_clang_exe = get_compiler_exe(arch)
        curr_pkg_config = shutil.which("pkg-config") if IS_LINUX else os.path.join(curr_clang_bin, "pkg-config.exe")

        if arch == "x64":
            curr_cflags = (
                [
                    "-std=c++20",
                ]
                + OPT_FLAGS_X64
                + [
                    "-Wall",
                    "-Wextra",
                    "-Wshadow",
                    "-Wformat=2",
                    "-Wundef",
                    "-Wno-unused-parameter",
                    "-Wno-microsoft-exception-spec",
                    "-D_WIN32_WINNT=0x0A00",
                    "-I" + os.path.join(PROJECT_ROOT, "common"),
                ]
            )
        else:  # x86
            curr_cflags = (
                [
                    "-std=c++20",
                ]
                + OPT_FLAGS_X86
                + [
                    "-m32",
                    "-mstackrealign",
                    "-Wall",
                    "-Wextra",
                    "-Wshadow",
                    "-Wformat=2",
                    "-Wundef",
                    "-Wno-unused-parameter",
                    "-Wno-microsoft-exception-spec",
                    "-D_WIN32_WINNT=0x0A00",
                    "-I" + os.path.join(PROJECT_ROOT, "common"),
                ]
            )
        # Add MSYS2 include path on Linux for Windows headers
        if IS_LINUX:
            msys2_dir = get_linux_msys2_dir()
            msys2_include = os.path.join(msys2_dir, "clang64", "include")
            if os.path.exists(msys2_include):
                curr_cflags.append("-I" + msys2_include)

        if arch == "x64":
            if not IS_LINUX:
                mingw_lib = os.path.join(MSYS2_DIR, "clang64", "lib")

        if arch == "x86":
            curr_cflags.append("-m32")
            curr_cflags.append("-mstackrealign")
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
        common_objs = []
        src_obj_pairs = []
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

        # Get vulkan lib path (use MSYS2 import library for cross-compilation)
        vulkan_lib = os.path.join(
            get_linux_msys2_dir(),
            "clang64" if arch == "x64" else "mingw32",
            "lib",
            "libvulkan-1.dll.a",
        )

        # Use delay-load for graphics DLLs so the hook can load even in games that don't have them
        # This prevents crash during DLL load when injecting into games that don't use D3D12/D3D11/etc
        ldflags_hook = [
            "-shared",
            "-static",
        ]

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
                "-lwinmm",
                "-luser32",
                "-lgdi32",
                "-lopengl32",
                vulkan_lib,
                "-lversion",
                "-ldxgi",
                "-ld3d12",
                "-ld3dcompiler",
                "-lpdh",
                "-lpsapi",
                "-lavrt",
                "-ldbghelp",
            ]
        )

        ldflags_hook.extend(LD_OPT_FLAGS)

        # LLD linker - use on Windows MSYS2, fallback to default on Linux
        if not IS_LINUX:
            ldflags_hook.extend(["-fuse-ld=lld", "-Wl,--exclude-all-symbols"])

        # Hook DLL must use conservative arch flags (injected into game processes
        # with unknown CPU support). Replace curr_cflags march/ffast-math flags.
        if arch == "x64":
            hook_base_cflags = (
                ["-std=c++20"]
                + HOOK_OPT_FLAGS_X64
                + [
                    "-Wall",
                    "-Wextra",
                    "-Wshadow",
                    "-Wformat=2",
                    "-Wundef",
                    "-Wno-unused-parameter",
                    "-Wno-microsoft-exception-spec",
                    "-D_WIN32_WINNT=0x0A00",
                    "-I" + os.path.join(PROJECT_ROOT, "common"),
                ]
            )
        else:
            hook_base_cflags = (
                ["-std=c++20"]
                + HOOK_OPT_FLAGS_X86
                + [
                    "-m32",
                    "-mstackrealign",
                    "-Wall",
                    "-Wextra",
                    "-Wshadow",
                    "-Wformat=2",
                    "-Wundef",
                    "-Wno-unused-parameter",
                    "-Wno-microsoft-exception-spec",
                    "-D_WIN32_WINNT=0x0A00",
                    "-I" + os.path.join(PROJECT_ROOT, "common"),
                ]
            )

        hk_cflags = (
            hook_base_cflags
            + ["-DVK_NO_PROTOTYPES", "-DBUILDING_CAPTURE_HOOK"]
            + [  # Vulkan hooks now in layer
                "-I" + os.path.join(PROJECT_ROOT, "common"),
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
                hk_cflags.append("-I" + vulkan_include)

        hk_objs = []
        src_obj_pairs = []
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

        cmd = [curr_clang_exe] + hk_objs + common_objs + ldflags_hook + ["-o", hk_dll]
        # cmd = [curr_clang_exe] + hk_objs + ldflags_hook + ["-o", hk_dll]
        run_command(cmd, env=curr_env)

        # Verify the built binary contains the correct version
        if os.path.exists(hk_dll):
            try:
                # Use strings to extract version from binary
                import subprocess as sp

                strings_exe = os.path.join(MSYS2_DIR, "clang64", "bin", "strings.exe")
                if not os.path.exists(strings_exe):
                    strings_exe = "strings"  # fallback
                result = sp.run([strings_exe, hk_dll], capture_output=True, text=True, timeout=10)
                version_match = re.search(r"1\.1\.0-dev\+build\.(\d+)", result.stdout)
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
                    log(f"[WARNING] Could not find version string in {os.path.basename(hk_dll)}")
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
                    # Use downloaded MSYS2 FFmpeg on Linux
                    msys2_dir = get_linux_msys2_dir()
                    # MSYS2 FFmpeg headers are in a versioned subdirectory
                    ffmpeg_include = os.path.join(msys2_dir, "clang64", "include", "ffmpeg7.1")
                    if not os.path.exists(ffmpeg_include):
                        # Try to find the actual FFmpeg include directory
                        include_dir = os.path.join(msys2_dir, "clang64", "include")
                        for d in os.listdir(include_dir):
                            if d.startswith("ffmpeg") and os.path.isdir(os.path.join(include_dir, d)):
                                ffmpeg_include = os.path.join(include_dir, d)
                                break
                    ffmpeg_lib = os.path.join(msys2_dir, "clang64", "lib")

                    # Use pkg-config from the MSYS2 FFmpeg if available, otherwise use include/lib paths directly
                    pkg_config_path = os.path.join(ffmpeg_lib, "pkgconfig")
                    env_ffmpeg = curr_env.copy()
                    env_ffmpeg["PKG_CONFIG_PATH"] = pkg_config_path + os.pathsep + env_ffmpeg.get("PKG_CONFIG_PATH", "")

                    try:
                        pkgs = [
                            "libavcodec",
                            "libavformat",
                            "libavutil",
                            "libswresample",
                            "libswscale",
                        ]
                        pkg_cmd = [curr_pkg_config, "--cflags", "--libs"] + pkgs
                        ffmpeg_flags_raw = run_command(pkg_cmd, env=env_ffmpeg).strip().split()

                        # Convert MSYS2-relative paths to absolute paths
                        # pkg-config returns paths like "-I/clang64/include" which are relative to MSYS2 root
                        ffmpeg_flags = []
                        for f in ffmpeg_flags_raw:
                            if f in ["-ldl", "-lshaderc_shared"]:
                                continue
                            if f.startswith("-I/") and not os.path.exists(f[2:]):
                                # Convert -I/clang64/include to -I/msys2_dir/clang64/include
                                rel_path = f[2:]  # Remove "-I"
                                abs_path = os.path.join(msys2_dir, rel_path.lstrip("/"))
                                if os.path.exists(abs_path):
                                    f = "-I" + abs_path
                            elif f.startswith("-L/") and not os.path.exists(f[2:]):
                                # Convert -L/clang64/lib to -L/msys2_dir/clang64/lib
                                rel_path = f[2:]  # Remove "-L"
                                abs_path = os.path.join(msys2_dir, rel_path.lstrip("/"))
                                if os.path.exists(abs_path):
                                    f = "-L" + abs_path
                            ffmpeg_flags.append(f)
                    except Exception as e:
                        log(f"pkg-config failed, using manual FFmpeg paths: {e}")
                        # Fallback to manual paths
                        ffmpeg_flags = [
                            "-I" + ffmpeg_include,
                            "-L" + ffmpeg_lib,
                            "-lavcodec",
                            "-lavformat",
                            "-lavutil",
                            "-lswresample",
                            "-lswscale",
                        ]
                else:
                    # Use local FFmpeg on Windows
                    env_ffmpeg = curr_env.copy()
                    env_ffmpeg["PKG_CONFIG_PATH"] = (
                        os.path.join(FFMPEG_DIR, "lib", "pkgconfig")
                        + os.pathsep
                        + env_ffmpeg.get("PKG_CONFIG_PATH", "")
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
                    ffmpeg_flags = [f for f in ffmpeg_flags_raw if f not in ["-ldl", "-lshaderc_shared"]]

                # For shared build, linking usually requires -Lpath -lavcodec.
                # We need to make sure the DLLs are findable at runtime.
                # Since they are in bin/ffmpeg, we might need to SetDllDirectory in app code or move them to bin/.
                # User asked for bin/ffmpeg. MediaEngine needs to handle it or we use DelayLoad.
                # DelayLoad is complex with MinGW.
                # Let's just link normally. User must assume bin/ffmpeg is in PATH or simple copy for dev.
                # Actually, standard Windows search path includes current dir.
                # If they are in bin/ffmpeg, they are NOT in current dir of captureengine.exe (bin).
                # We might need a manifest or SetDllDirectory.
                # For now, just link.

                me_dll = os.path.join(BIN_DIR, "mediaengine.dll")
                me_lib = os.path.join(BIN_DIR, "libmediaengine.dll.a")

                me_ldflags = [
                    "-shared",
                    "-static",
                    "-static-libgcc",
                    "-static-libstdc++",
                    "-Wl,--gc-sections",
                    "-s",
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
                if curr_env.get("CE_DISABLE_LTO") != "1":
                    me_ldflags.append("-flto")
                if any(flag.startswith("-fsanitize=") for flag in curr_cflags):
                    me_ldflags.append("-fsanitize=address,undefined")
                me_ldflags.append(f"-Wl,--out-implib,{me_lib}")

                me_cflags = curr_cflags + ["-DMEDIAENGINE_EXPORTS"] + ffmpeg_flags
                me_objs = []
                src_obj_pairs = []
                for src in me_src:
                    rel_path = os.path.relpath(src, PROJECT_ROOT)
                    obj = os.path.join(curr_obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
                    src_obj_pairs.append((src, obj))
                    me_objs.append(obj)
                parallel_compile(curr_env, curr_clang_exe, me_cflags, src_obj_pairs)

                log("Linking MediaEngine x64...")
                # Use absolute paths to our custom FFmpeg import libs to avoid system FFmpeg
                ffmpeg_lib_dir = os.path.join(FFMPEG_DIR, "lib")
                ffmpeg_import_libs = [
                    os.path.join(ffmpeg_lib_dir, "libavformat.dll.a"),
                    os.path.join(ffmpeg_lib_dir, "libavcodec.dll.a"),
                    os.path.join(ffmpeg_lib_dir, "libswresample.dll.a"),
                    os.path.join(ffmpeg_lib_dir, "libswscale.dll.a"),
                    os.path.join(ffmpeg_lib_dir, "libavutil.dll.a"),
                ]
                temp_me_dll = os.path.join(curr_obj_dir, "mediaengine.tmp.dll")
                safe_delete_file(temp_me_dll)
                cmd = [curr_clang_exe] + me_objs + common_objs + me_ldflags + ffmpeg_import_libs + ["-o", temp_me_dll]
                run_command(cmd, env=curr_env)
                if not safe_copy_file(temp_me_dll, me_dll):
                    log("ERROR: Failed to place mediaengine.dll (destination may be locked)")
                    sys.exit(1)
                safe_delete_file(temp_me_dll)
                # generate_hash(me_dll) # MediaEngine doesn't need hash check for injection
                # Note: mediaengine.dll is output directly to BIN_DIR (main folder)
                # It acts as a bridge to FFmpeg DLLs in ffmpeg/ subfolder

                # Copy FFmpeg DLLs to bin/ffmpeg/ for runtime (Linux)
                if IS_LINUX:
                    log("Copying FFmpeg DLLs to bin/ffmpeg/...")
                    # Use external/ffmpeg/bin if it exists (from Windows build), otherwise MSYS2
                    if os.path.exists(os.path.join(FFMPEG_DIR, "bin")):
                        ffmpeg_bin_src = os.path.join(FFMPEG_DIR, "bin")
                    else:
                        msys2_dir = get_linux_msys2_dir()
                        ffmpeg_bin_src = os.path.join(msys2_dir, "clang64", "bin")

                    ffmpeg_bin_dst = os.path.join(BIN_DIR, "ffmpeg")
                    os.makedirs(ffmpeg_bin_dst, exist_ok=True)

                    # Copy ALL DLLs from FFmpeg bin folder (includes dependencies)
                    for dll in glob.glob(os.path.join(ffmpeg_bin_src, "*.dll")):
                        shutil.copy(dll, ffmpeg_bin_dst)
                        log(f"Copied {os.path.basename(dll)} to bin/ffmpeg/")

                    # Copy MSYS2 runtime dependencies that FFmpeg DLLs need
                    msys2_dir = get_linux_msys2_dir()
                    msys_bin = os.path.join(msys2_dir, "clang64", "bin")
                    runtime_deps = [
                        "libbz2-1.dll",
                        "libxml2-16.dll",
                        "libmodplug-1.dll",
                        "libgme.dll",
                        "libiconv-2.dll",
                        "libc++.dll",
                        "libva.dll",
                        "libva_win32.dll",
                        "libvpl-2.dll",
                        "libwinpthread-1.dll",
                        "libgcc_s_seh-1.dll",
                        "libstdc++-6.dll",
                    ]
                    for dep in runtime_deps:
                        # Try MSYS2 first, then system mingw
                        src = os.path.join(msys_bin, dep)
                        if not os.path.exists(src):
                            # Try system mingw bin directory
                            if arch == "x64":
                                src = os.path.join("/usr", "x86_64-w64-mingw32", "bin", dep)
                            else:
                                src = os.path.join("/usr", "i686-w64-mingw32", "bin", dep)
                        if os.path.exists(src):
                            safe_copy_file(src, os.path.join(ffmpeg_bin_dst, dep))
                            log(f"Copied runtime dep {dep}")

    # Compile and run tests (using x64 objects) if requested
    if should_run_tests:
        x64_common_objs = [
            os.path.join(OBJ_DIR, "x64", os.path.relpath(s, PROJECT_ROOT).replace(".cpp", ".o"))
            for s in glob.glob(os.path.join(PROJECT_ROOT, "common", "*.cpp"))
        ]
        # We use x64 obj dir for tests
        test_exe = compile_tests(
            env,
            clang_exe,
            cflags,
            x64_common_objs,
            pkg_config,
            os.path.join(OBJ_DIR, "x64"),
        )
        if test_exe:
            if not run_tests(env, test_exe):
                sys.exit(1)

    # 5. CaptureEngine (x64 only for now)
    log("Compiling CaptureEngine x64...")
    ce_src = glob.glob(os.path.join(PROJECT_ROOT, "captureengine", "*.cpp"))
    if ce_src:
        ce_exe = os.path.join(BIN_DIR, "captureengine.exe")
        me_lib = os.path.join(BIN_DIR, "libmediaengine.dll.a")
        ce_obj_dir = os.path.join(OBJ_DIR, "x64")
        ce_objs = []
        src_obj_pairs = []
        for src in ce_src:
            if "screen_capture.cpp" in src:
                continue
            rel_path = os.path.relpath(src, PROJECT_ROOT)
            obj = os.path.join(ce_obj_dir, os.path.splitext(rel_path)[0] + ".o").replace("\\", "/")
            src_obj_pairs.append((src, obj))
            ce_objs.append(obj)
        parallel_compile(env, get_compiler_exe("x64"), cflags, src_obj_pairs)

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
        ce_ldflags = [
            "-mwindows",
            "-static",
            "-static-libgcc",
            "-static-libstdc++",
            "-Wl,--gc-sections",
            "-s",
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
            "-ldbghelp",
            "-lwbemuuid",
            "-lbcrypt",
            "-lwintrust",
            "-lpdh",
            "-lntdll",
        ]
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
        # x64 common objects
        x64_common_objs = [
            os.path.join(OBJ_DIR, "x64", os.path.relpath(s, PROJECT_ROOT).replace(".cpp", ".o"))
            for s in glob.glob(os.path.join(PROJECT_ROOT, "common", "*.cpp"))
        ]
        temp_ce_exe = os.path.join(ce_obj_dir, "captureengine.tmp.exe")
        safe_delete_file(temp_ce_exe)
        cmd = [get_compiler_exe("x64")] + ce_objs + x64_common_objs + ce_ldflags + ["-o", temp_ce_exe]
        run_command(cmd, env=env)
        if not safe_copy_file(temp_ce_exe, ce_exe):
            log("ERROR: Failed to place captureengine.exe (destination may be locked)")
            sys.exit(1)
        safe_delete_file(temp_ce_exe)

    # 6. Compile Test Applications (DX9/10/11/12, Vulkan, OpenGL; x64/x86)
    x86_env_for_tests = None
    if get_env_x86:
        x86_env_for_tests, _ = get_env_x86()
    compile_testapps(env, x86_env_for_tests, get_compiler_exe("x64"), cflags)

    # 7. Compile Vulkan Layer (VK_LAYER_CE_overlay)
    compile_vulkan_layer(env, get_compiler_exe("x64"), cflags, "x64")
    # x86 layer using mingw32 toolchain (disabled for sanitizer builds)
    if get_env_x86 and env.get("CE_SANITIZE") != "1":
        x86_env, x86_clang_bin = get_env_x86()
        x86_clang = get_compiler_exe("x86")
        # Check if x86 compiler exists (on Linux it might not)
        if IS_LINUX or os.path.exists(x86_clang):
            x86_cflags = [
                "-std=c++20",
                "-O3",
                "-m32",
                "-Wall",
                "-Wextra",
                "-Wshadow",
                "-Wformat=2",
                "-Wundef",
                "-Wno-unused-parameter",
                "-D_WIN32_WINNT=0x0A00",
            ]
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
        log("Copied licenses/ directory to installed/captureengine/")

    # Keep runtime DLLs in tests/ current so unit_tests.exe can run directly
    tests_dir = os.path.join(PROJECT_ROOT, "tests")
    if os.path.exists(os.path.join(tests_dir, "unit_tests.exe")):
        copy_test_runtime_dlls(tests_dir)

    log("Build Complete.")


def backup_sources(script_dir):
    timestamp = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    backup_root = os.path.join(script_dir, "bak", timestamp)

    log(f"Creating source backup in {backup_root}...")

    dirs_to_backup = ["hook", "captureengine", "mediaengine", "common", "tests"]
    files_to_backup = [
        "build_and_run.py",
        "CMakeLists.txt",
    ]  # Add strict files if needed

    for d in dirs_to_backup:
        src_path = os.path.join(script_dir, d)
        if os.path.exists(src_path):
            dst_path = os.path.join(backup_root, d)
            # Ignore binary/obj directories inside if any (though unlikely in source dirs)
            shutil.copytree(
                src_path,
                dst_path,
                ignore=shutil.ignore_patterns("*.obj", "*.o", "*.tmp"),
            )

    # Also backup root files
    os.makedirs(backup_root, exist_ok=True)
    for f in files_to_backup:
        src = os.path.join(script_dir, f)
        if os.path.exists(src):
            shutil.copy2(src, backup_root)


def ensure_debug_logging():
    """Ensure debug_logging=true in bin/config.ini."""
    config_path = os.path.join(BIN_DIR, "config.ini")
    if not os.path.exists(config_path):
        log("config.ini missing, skipping debug_logging check.")
        return

    try:
        with open(config_path, "r") as f:
            lines = f.readlines()

        changed = False
        new_lines = []
        for line in lines:
            if line.strip().startswith("debug_logging="):
                if "true" not in line:
                    new_lines.append("debug_logging=true\n")
                    changed = True
                else:
                    new_lines.append(line)
            else:
                new_lines.append(line)

        if changed:
            with open(config_path, "w") as f:
                f.writelines(new_lines)
            log("Forced debug_logging=true in config.ini for testing.")
    except Exception as e:
        log(f"Warning: Failed to update config.ini: {e}")


def run_sanitizer_regression_pass(skip_updates: bool, ccache_flag: bool) -> None:
    """Run a second validation pass with ASan/UBSan + unit tests."""
    cmd = [
        sys.executable,
        os.path.abspath(__file__),
        "--run-tests",
        "--sanitize",
        "--incremental",
        "--sanitize-regression-child",
    ]
    if skip_updates:
        cmd.append("--skip-updates")
    if ccache_flag:
        cmd.append("--ccache")

    log("=== Running sanitizer regression cadence pass ===")
    result = subprocess.run(cmd, cwd=PROJECT_ROOT)
    if result.returncode != 0:
        log(f"ERROR: Sanitizer regression pass failed (exit code {result.returncode})")
        sys.exit(result.returncode)
    log("=== Sanitizer regression cadence pass: OK ===")


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)

    # 0. Backup Sources - DISABLED per user request
    # backup_sources(script_dir)

    if os.path.exists(LOG_FILE):
        try:
            os.remove(LOG_FILE)
        except Exception:
            pass

    log("=== Starting Build ===")

    # Parse flags early for force rebuild
    force_rebuild = "--force-rebuild" in sys.argv
    if force_rebuild:
        log("FORCE REBUILD: Cleaning all object files...")
        if os.path.exists(OBJ_DIR):
            try:
                shutil.rmtree(OBJ_DIR)
                log("Object directory cleaned.")
            except Exception as e:
                log(f"Warning: Could not clean {OBJ_DIR}: {e}")

    current_build_number = bump_and_write_build_version()
    # Store for use by compile_project
    global CURRENT_BUILD_NUMBER
    CURRENT_BUILD_NUMBER = current_build_number

    # Always clean object files to avoid struct layout mismatches
    # if os.path.exists(OBJ_DIR):
    #     log("Cleaning object files for fresh build...")
    #     try:
    #         shutil.rmtree(OBJ_DIR)
    #     except Exception as e:
    #         log(f"Warning: Could not clean {OBJ_DIR}: {e}")

    # Clean log files in logs/ subfolder
    logs_dir = os.path.join(BIN_DIR, "logs")
    if os.path.exists(logs_dir):
        log("Cleaning log files...")
        for log_file in glob.glob(os.path.join(logs_dir, "*.log")):
            try:
                os.remove(log_file)
            except Exception as e:
                log(f"Warning: Could not delete {log_file}: {e}")
        for csv_file in glob.glob(os.path.join(logs_dir, "*.csv")):
            try:
                os.remove(csv_file)
            except Exception as e:
                log(f"Warning: Could not delete {csv_file}: {e}")
        # Clean crash dumps and crash handler traces
        for dmp_file in glob.glob(os.path.join(logs_dir, "*.dmp")):
            try:
                os.remove(dmp_file)
            except Exception as e:
                log(f"Warning: Could not delete {dmp_file}: {e}")
        crash_trace_file = os.path.join(logs_dir, "crash_handler_trace.txt")
        if os.path.exists(crash_trace_file):
            try:
                os.remove(crash_trace_file)
            except Exception as e:
                log(f"Warning: Could not delete {crash_trace_file}: {e}")

    # Clean legacy log files from root/bin
    legacy_logs = [
        os.path.join(BIN_DIR, "Layer"),
        os.path.join(PROJECT_ROOT, "Layer"),
    ]
    for f in legacy_logs:
        if os.path.exists(f):
            try:
                os.remove(f)
                log(f"Removed legacy log: {f}")
            except Exception:
                pass

    setup_msys2()
    check_python_lsp_tools()
    env, clang_bin = get_env()

    # Parse flags
    args = sys.argv[1:]
    default_quality_mode = len(args) == 0
    skip_updates = "--skip-updates" in sys.argv
    run_tests_flag = "--run-tests" in sys.argv
    run_integration_flag = "--run-integration-tests" in sys.argv
    full_integration_flag = "--full-integration" in sys.argv
    lint_flag = "--lint" in sys.argv
    format_flag = "--format" in sys.argv
    ccache_flag = "--ccache" in sys.argv
    sanitize_flag = "--sanitize" in sys.argv
    sanitize_regression_flag = "--sanitize-regression" in sys.argv
    sanitize_regression_child = "--sanitize-regression-child" in sys.argv
    # --production: build signed production binaries (requires CE_PRODUCTION_BUILD=1)
    # Dev builds do NOT pass this flag; signature verification is a warning only.
    production_flag = "--production" in sys.argv or "CE_PRODUCTION_BUILD" in os.environ
    # --force is now DEFAULT behavior for reliability (disable with --incremental)
    incremental_flag = "--incremental" in sys.argv
    force_flag = not incremental_flag  # Force rebuild by default

    if default_quality_mode:
        log("Default quality mode: running auto-repair, lint/LSP checks, and " "unit/regression tests")
        run_tests_flag = True
        lint_flag = True
        format_flag = True
        sanitize_regression_flag = True

    if full_integration_flag:
        run_integration_flag = True
    if run_integration_flag:
        run_tests_flag = True
    if sanitize_regression_child:
        sanitize_regression_flag = False

    # Store flags in env for access in compile functions
    env["FORCE_REBUILD"] = "1" if force_flag else "0"
    if production_flag:
        env["CE_PRODUCTION_BUILD"] = "1"
        log("PRODUCTION BUILD: DLL signature verification will be enforced")
    else:
        log("DEV BUILD: DLL signature verification is advisory only")

    if incremental_flag:
        log("Incremental build (--incremental) - may use cached objects")
    else:
        log("Force rebuild (default) - ensuring clean build for reliability")

    if ccache_flag:
        log("Enabling ccache for faster builds (--ccache)")
        log(
            "WARNING: ccache may occasionally serve stale objects."
            " Use --ccache only for development, not release builds."
        )
        # Clear any existing disable flag
        if "DISABLE_CCACHE" in env:
            del env["DISABLE_CCACHE"]

    if sanitize_flag:
        log("Sanitizer build enabled (--sanitize) - adding ASan + UBSan flags")
        sanitize_compile = [
            "-fsanitize=address,undefined",
            "-fno-omit-frame-pointer",
            "-g",
        ]
        sanitize_link = ["-fsanitize=address,undefined"]

        def _strip_lto(flags: List[str]) -> List[str]:
            return [flag for flag in flags if not flag.startswith("-flto")]

        # ASan/UBSan + LTO is unstable with current MinGW LLVM/LLD and can crash
        # the linker with COMDAT key errors. Disable LTO for sanitizer builds.
        OPT_FLAGS_X64[:] = _strip_lto(OPT_FLAGS_X64)
        HOOK_OPT_FLAGS_X64[:] = _strip_lto(HOOK_OPT_FLAGS_X64)
        OPT_FLAGS_X86[:] = _strip_lto(OPT_FLAGS_X86)
        HOOK_OPT_FLAGS_X86[:] = _strip_lto(HOOK_OPT_FLAGS_X86)
        LD_OPT_FLAGS[:] = _strip_lto(LD_OPT_FLAGS)
        env["CE_SANITIZE"] = "1"
        env["CE_DISABLE_LTO"] = "1"

        OPT_FLAGS_X64.extend(sanitize_compile)
        OPT_FLAGS_X86.extend(sanitize_compile)
        LD_OPT_FLAGS.extend(sanitize_link)

    if skip_updates:
        log("FFmpeg updates disabled (--skip-updates)")
    if run_tests_flag:
        log("Unit/regression test suite enabled")
    if sanitize_regression_flag:
        log("Sanitizer regression cadence enabled")
    if run_integration_flag:
        if full_integration_flag:
            log("Integration mode: full matrix (--full-integration)")
        else:
            log("Integration mode: smoke (default)")

    if format_flag:
        format_ok = run_format(env)
        if not format_ok:
            log("Auto-format completed with issues.")
        if len(args) == 1 and "--format" in args and not lint_flag and not run_tests_flag and not run_integration_flag:
            return

    if lint_flag:
        lint_ok = run_lint(env)
        if not lint_ok:
            log("Lint/LSP checks reported issues.")
        if len(args) == 1 and "--lint" in args and not format_flag and not run_tests_flag and not run_integration_flag:
            return

    if sanitize_regression_flag and not sanitize_regression_child:
        if sanitize_flag:
            log("Sanitizer regression cadence requested in sanitizer mode; skipping nested pass")
        else:
            # Run sanitizer validation first so final installed artifacts remain
            # non-sanitized unless --sanitize was explicitly requested.
            run_sanitizer_regression_pass(skip_updates=skip_updates, ccache_flag=ccache_flag)

    compile_project(env, clang_bin, skip_updates=skip_updates, should_run_tests=run_tests_flag)

    # Write compile_commands.json
    try:
        # Deduplicate and sort compile commands for better LSP performance/determinism
        seen_files = set()
        unique_commands = []

        # Sort by file path to keep output stable
        sorted_commands = sorted(COMPILE_COMMANDS, key=lambda x: x["file"])

        for cmd in sorted_commands:
            # Prefer x64 commands over x86 for LSP if both exist for the same file
            # (assuming x64 is usually the primary dev target)
            is_x86 = "mingw32" in cmd["arguments"][0] or "-m32" in cmd["arguments"]

            if cmd["file"] not in seen_files:
                unique_commands.append(cmd)
                seen_files.add(cmd["file"])
            elif not is_x86:
                # Replace x86 entry with x64 entry if we encounter it
                for i, existing in enumerate(unique_commands):
                    if existing["file"] == cmd["file"]:
                        unique_commands[i] = cmd
                        break

        compile_commands_path = os.path.join(PROJECT_ROOT, "compile_commands.json")
        write_json_atomic(compile_commands_path, unique_commands)
        log(f"Generated compile_commands.json ({len(unique_commands)} entries)")
        log("LSP: normalized compile_commands paths for clangd")

        # Auto-detect clang resource-dir and update .clangd so LSP survives toolchain updates.
        _clangd_path = os.path.join(PROJECT_ROOT, ".clangd")
        if os.path.exists(_clangd_path):
            _clang_hint = ""
            if unique_commands and unique_commands[0].get("arguments"):
                _clang_hint = unique_commands[0]["arguments"][0]
            if not _clang_hint:
                _clang_hint = os.path.join(clang_bin, "clang++.exe" if IS_WINDOWS else "clang++")

            _detected_resource_dir = detect_clang_resource_dir(env, _clang_hint)
            if _detected_resource_dir:
                with open(_clangd_path, "r", encoding="utf-8") as _f:
                    _clangd_content = _f.read()
                import re as _re

                _normalized_resource_dir = _detected_resource_dir.replace("\\", "/")
                _project_root_norm = PROJECT_ROOT.replace("\\", "/").rstrip("/")
                if _normalized_resource_dir.lower().startswith(_project_root_norm.lower() + "/"):
                    _normalized_resource_dir = _normalized_resource_dir[len(_project_root_norm) + 1 :]

                _updated, _resource_subs = _re.subn(
                    r"(-resource-dir=)(?:[^\"\n]*/)?clang64/lib/clang/[^\"\n]+",
                    rf"\g<1>{_normalized_resource_dir}",
                    _clangd_content,
                )
                _updated = _re.sub(
                    r"(^\s*Compiler:\s*).*$",
                    r"\g<1>build/msys64/clang64/bin/clang++.exe",
                    _updated,
                    flags=_re.MULTILINE,
                )
                _updated = _re.sub(
                    r'(^\s*-\s*")[^"\n]*clang64/include/c\+\+/v1(".*$)',
                    r"\1build/msys64/clang64/include/c++/v1\2",
                    _updated,
                    flags=_re.MULTILINE,
                )
                _updated = _re.sub(
                    r'(^\s*-\s*")[^"\n]*clang64/include(?!/c\+\+/v1)(".*$)',
                    r"\1build/msys64/clang64/include\2",
                    _updated,
                    flags=_re.MULTILINE,
                )
                if _updated != _clangd_content:
                    with open(_clangd_path, "w", encoding="utf-8") as _f:
                        _f.write(_updated)
                    log(f"LSP: Updated .clangd compiler/resource/include paths ({_normalized_resource_dir})")
                else:
                    log(f"LSP: .clangd compiler/resource/include paths already correct ({_normalized_resource_dir})")
                if _resource_subs == 0:
                    log("LSP: WARNING: .clangd resource-dir pattern not found; please verify .clangd format")
            else:
                log("LSP: WARNING: Could not detect clang resource-dir; .clangd was not updated")
    except Exception as e:
        log(f"Error writing compile_commands.json: {e}")

    if run_integration_flag:
        ensure_debug_logging()
        run_integration_tests(env, full_matrix=full_integration_flag)


if __name__ == "__main__":
    main()
