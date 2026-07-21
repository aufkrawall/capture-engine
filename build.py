# MIT License
#
# Copyright (c) 2026 aufkrawall
#
# Permission is hereby granted, free of charge, to any person obtaining a copy
# of this software and associated documentation files (the "Software"), to deal
# in the Software without restriction, including without limitation the rights
# to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
# copies of the Software, and to permit persons to whom the Software is
# furnished to do so, subject to the following conditions:
#
# The above copyright notice and this permission notice shall be included in
# all copies or substantial portions of the Software.
#
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
# IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
# FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
# AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
# LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
# SOFTWARE.

import os
import sys
import glob
import shutil
import tarfile
import subprocess
import atexit
import urllib.error
import urllib.request
import time
import datetime
import hashlib
import platform
import shlex
import site
import stat
import threading
from collections import Counter, deque
from functools import lru_cache
from concurrent.futures import ThreadPoolExecutor, as_completed
from multiprocessing import cpu_count
import re
import json

from typing import List, Dict, Optional, Union, Any

from ffmpeg_dependencies import (
    SourceDependencyBuilder,
    dependency_manifest_fingerprint,
    dependency_prefix,
    is_path_within,
    load_dependency_manifest,
    manifest_runtime_dlls,
    parse_pe_import_names,
    verify_detached_signature,
    verify_pe_import_closure,
)
from ffmpeg_patch_utils import normalize_custom_patch_targets

# --- Platform Detection ---
IS_WINDOWS = sys.platform == "win32"
IS_LINUX = sys.platform.startswith("linux")
IS_WSL = IS_LINUX and "microsoft" in platform.uname().release.lower()

# --- Optimization Flags ---
# These flags are safe for both host and injected binaries and provide basic
# hardening without changing program behavior. _FORTIFY_SOURCE requires the
# optimization level that all current build profiles already enable.
COMMON_HARDENING_FLAGS = [
    "-fstack-protector-strong",
    "-D_FORTIFY_SOURCE=2",
]
CPP_STD_FLAGS = ["-std=c++20"]
COMMON_WARNING_FLAGS = [
    "-Wall",
    "-Wextra",
    "-Wshadow",
    "-Wformat=2",
    "-Wundef",
    "-Wno-unused-parameter",
]
# CFG is reliable for the native x64 Clang/LLD toolchain. GCC does not support
# Clang's -mguard=cf instrumentation, while the Windows clang64 -> mingw32 x86
# cross-link currently emits the CFG image bit without a usable target table.
# Keep compiler-specific selection centralized below so unsupported MinGW GCC
# flags never leak into Linux cross-compilation.
CFG_COMPILE_FLAG = "-mguard=cf"
CFG_LINK_FLAG = "-Wl,--guard-cf"
COMMON_WINDOWS_COMPILE_FLAGS = ["-D_WIN32_WINNT=0x0A00", "-DNOMINMAX"]
if IS_WINDOWS:
    # Emit native CodeView info so clang/lld can write PDBs that CDB, WinDbg,
    # and Visual Studio understand without switching away from the clang toolchain.
    COMMON_DEBUG_INFO_FLAGS = ["-gcodeview"]
else:
    COMMON_DEBUG_INFO_FLAGS = ["-g1"]  # Minimal DWARF info for crash symbolication with low size impact

# First-party x64 code must run on the architectural x86-64 baseline. Codec
# libraries retain their own measured runtime dispatch for newer instruction sets.
OPT_FLAGS_X64 = [
    "-O3",
    "-flto",
    "-fcf-protection=full",  # CET where supported; Windows CFG is -mguard=cf/--guard-cf.
    "-march=x86-64",
    "-mtune=generic",
    "-fvisibility=hidden",
    "-ffunction-sections",
    "-fdata-sections",
] + COMMON_HARDENING_FLAGS

# Hook DLL flags: injected into arbitrary game processes — must not require AVX2
# and must not use -ffast-math (audio encoder correctness requires IEEE 754 semantics).
HOOK_OPT_FLAGS_X64 = [
    "-O3",
    "-flto",
    "-fcf-protection=full",  # Control Flow Integrity
    "-march=x86-64",
    "-mtune=generic",
    "-fvisibility=hidden",
    "-ffunction-sections",
    "-fdata-sections",
    # Hook DLL performs aliased pointer access (vtable patching, SHM reinterpret_cast).
    # Without -fno-strict-aliasing the optimizer may miscompile these pointer casts.
    "-fno-strict-aliasing",
] + COMMON_HARDENING_FLAGS

# x86 builds use generic optimization (no AVX on 32-bit)
OPT_FLAGS_X86 = [
    "-O3",
    "-flto",
    "-march=i686",
    "-mtune=generic",
    "-fvisibility=hidden",
    "-ffunction-sections",
    "-fdata-sections",
] + COMMON_HARDENING_FLAGS

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
] + COMMON_HARDENING_FLAGS

# Unit tests and the single-translation-unit test apps retain the optimized
# hardening baseline but do not use LTO. Product binaries keep their existing
# full-LTO policy; using it for validation-only binaries adds substantial link
# work without improving the shipped artifacts or cross-TU coverage of a
# one-source test app.
UNIT_TEST_OPT_FLAGS_X64 = [flag for flag in OPT_FLAGS_X64 if not flag.startswith("-flto")]
TESTAPP_OPT_FLAGS_X64 = list(UNIT_TEST_OPT_FLAGS_X64)
TESTAPP_OPT_FLAGS_X86 = [
    "-O3",
    "-march=i686",
    "-mtune=generic",
    "-fvisibility=hidden",
    "-ffunction-sections",
    "-fdata-sections",
] + COMMON_HARDENING_FLAGS
TESTAPP_X86_CFG_LINK_FLAGS = ["-Wl,--no-guard-cf"]

# Linker optimization flags
# Keep debug info enabled for crash dumps and post-mortem analysis. On Windows
# this pairs with sidecar PDB emission; on non-Windows hosts we keep minimal
# DWARF info in the PE outputs.
LD_OPT_FLAGS = [
    "-Wl,--gc-sections",
    "-Wl,--dynamicbase",  # ASLR
    "-Wl,--nxcompat",  # DEP/NX
] + COMMON_DEBUG_INFO_FLAGS

# x64-only linker flags. x86 intentionally does not receive CFG until the
# cross-linker can emit a valid target table; high-entropy VA is x64-only too.
LD_OPT_FLAGS_X64 = [
    CFG_LINK_FLAG,
    "-Wl,--high-entropy-va",  # High-entropy 64-bit ASLR
]

# Floating-point state that affects timestamps, mixing, resampling, or HDR color
# conversion must not vary with contraction/reassociation decisions. Clang's
# aggregate strict model has no GCC spelling, so use its explicit GCC
# equivalents when Linux selects the system MinGW GCC toolchain.
STRICT_FP_MEDIA_SOURCES = {
    "app_audio_capture.cpp",
    "audio_capture.cpp",
    "audio_encoder.cpp",
    "audio_latency_probe.cpp",
    "audio_resampler.cpp",
    "mediaengine.cpp",
    "process_loopback_capture.cpp",
    "process_loopback_worker.cpp",
    "video_encoder.cpp",
}
CLANG_STRICT_FP_FLAGS = ["-ffp-model=strict"]
GCC_STRICT_FP_FLAGS = ["-fno-fast-math", "-ffp-contract=off", "-frounding-math", "-fsignaling-nans"]

# --- Configuration ---
BUILD_DIR_NAME = "build"
COMPILE_COMMANDS: List[Dict[str, Any]] = []
CURRENT_BUILD_NUMBER = 0  # Set by bump_and_write_build_version()

PROJECT_ROOT = os.path.dirname(os.path.abspath(__file__))

BUILD_DIR = os.path.join(PROJECT_ROOT, BUILD_DIR_NAME)
MSYS2_DIST_URL = "https://repo.msys2.org/distrib/x86_64/"
MSYS2_DEFAULT_TARBALL = "msys2-base-x86_64-20260611.tar.xz"
MSYS2_BOOTSTRAP_PGP_KEY = "E0AA0F031DBD80FFBA57B06D5A62D0CAB6264964"
MSYS2_BOOTSTRAP_KEY_SERVERS = (
    "hkps://keys.openpgp.org",
    "hkps://keyserver.ubuntu.com",
)

# FG SDK download URLs (for test app DLLs and headers)
FFX_SDK_URL = (
    "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v2.2.0/"
    "FidelityFX-Samples-v2.2.0-prebuilt.zip"
)
FFX_SDK_ZIP_NAME = "FidelityFX-Samples-v2.2.0-prebuilt.zip"
FFX_SDK_SOURCE_URL = "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/archive/refs/tags/v2.2.0.zip"
FFX_SDK_SOURCE_ZIP_NAME = "FidelityFX-SDK-v2.2.0-source.zip"
# FidelityFX SDK 2.2 deliberately has no Vulkan backend. The Vulkan FG switch app is therefore
# pinned independently to AMD's signed 1.1.4 release (FSR 3.1.4 SR/FG) and never includes 2.2
# headers in the same translation unit.
FFX_VK_SDK_URL = (
    "https://github.com/GPUOpen-LibrariesAndSDKs/FidelityFX-SDK/releases/download/v1.1.4/" "FidelityFX-SDK-v1.1.4.zip"
)
FFX_VK_SDK_ZIP_NAME = "FidelityFX-SDK-v1.1.4.zip"
FFX_VK_SDK_SHA256 = "0216556bfb0e243cec30004a2a98d38f4e3f7406cb7938e3c1b85c758e95d952"
STREAMLINE_SDK_URL = "https://github.com/NVIDIA-RTX/Streamline/releases/download/v2.11.1/streamline-sdk-v2.11.1.zip"
STREAMLINE_SDK_ZIP_NAME = "streamline-sdk-v2.11.1.zip"
FG_SDK_CACHE_DIR = os.path.join(BUILD_DIR, "fg_sdk_cache")
FG_SDK_INCLUDE_DIR = os.path.join(BUILD_DIR, "fg_sdk_include")
MSYS2_DIR = os.path.join(BUILD_DIR, "msys64")
OBJ_DIR = os.path.join(BUILD_DIR, "obj")
BIN_DIR = os.path.join(BUILD_DIR, "bin")
INSTALLED_DIR = os.path.join(PROJECT_ROOT, "installed")
CAPTURE_BIN_DIR = os.path.join(INSTALLED_DIR, "captureengine")
TESTAPP_BIN_DIR = os.path.join(INSTALLED_DIR, "testapp")
BIN_DIR = CAPTURE_BIN_DIR  # output captureengine binaries to installed\captureengine
DEFAULT_LOG_FILE = os.path.join(PROJECT_ROOT, "build.log")
LOG_FILE = DEFAULT_LOG_FILE
DETAIL_LOG_FILE: Optional[str] = None
VERIFICATION_DIR = os.path.join(BUILD_DIR, "verification")
VERBOSE_COMMANDS = False
CONCISE_OUTPUT = False
LOG_LOCK = threading.Lock()
FAILURE_OUTPUT_TAIL_LINES = 80
VERIFICATION_CONTEXT: Optional[Dict[str, Any]] = None
VERIFICATION_FINAL_EXIT_CODE = 0
VERIFICATION_ATEXIT_REGISTERED = False
VERIFICATION_FINALIZED = False
WORKSPACE_TEMP_DIR = os.path.join(BUILD_DIR, "tmp")


def sha256_file(path: str) -> str:
    digest = hashlib.sha256()
    with open(path, "rb") as src:
        for chunk in iter(lambda: src.read(1024 * 1024), b""):
            digest.update(chunk)
    return digest.hexdigest()


def pe_has_authenticode_certificate(path: str) -> bool:
    """Return whether a PE image carries a non-empty Authenticode certificate table."""
    try:
        with open(path, "rb") as src:
            image = src.read()
        if len(image) < 0x40 or image[:2] != b"MZ":
            return False
        pe_offset = int.from_bytes(image[0x3C:0x40], "little")
        if pe_offset + 26 > len(image) or image[pe_offset : pe_offset + 4] != b"PE\0\0":
            return False
        optional_offset = pe_offset + 24
        magic = int.from_bytes(image[optional_offset : optional_offset + 2], "little")
        data_directory_offset = optional_offset + (112 if magic == 0x20B else 96 if magic == 0x10B else 0)
        if data_directory_offset == optional_offset or data_directory_offset + 40 > len(image):
            return False
        # IMAGE_DIRECTORY_ENTRY_SECURITY is directory index 4 and uses a file offset, not an RVA.
        certificate_offset = int.from_bytes(image[data_directory_offset + 32 : data_directory_offset + 36], "little")
        certificate_size = int.from_bytes(image[data_directory_offset + 36 : data_directory_offset + 40], "little")
        return certificate_size >= 8 and certificate_offset + certificate_size <= len(image)
    except OSError:
        return False


def append_linux_msys2_include(flags: List[str]) -> None:
    if not IS_LINUX:
        return
    msys2_dir = get_linux_msys2_dir()
    msys2_include = os.path.join(msys2_dir, "clang64", "include")
    if os.path.exists(msys2_include):
        # Treat MSYS2's fallback headers as "after" includes on Linux so the
        # host MinGW toolchain keeps using its own CRT/Windows SDK headers.
        # This avoids mixing Debian MinGW's CRT with MSYS2 clang64's UCRT headers
        # while still allowing missing packages like cppwinrt/vulkan to resolve.
        flags.extend(["-idirafter", msys2_include])


def pdb_path_for_binary(binary_path: str) -> str:
    return os.path.splitext(os.path.abspath(binary_path))[0].replace("\\", "/") + ".pdb"


def _get_linux_cross_linker_info(compiler: str) -> str:
    """Return a diagnostic string about the expected cross-linker on Linux."""
    if not IS_LINUX:
        return ""
    try:
        result = subprocess.run(
            [compiler, "-print-prog-name=ld"],
            capture_output=True, text=True, timeout=15,
        )
        if result.returncode == 0:
            linker_path = result.stdout.strip()
            if os.path.isfile(linker_path):
                return linker_path
            else:
                return f"not_found({linker_path})"
        return ""
    except Exception:
        return ""


def append_windows_pdb_linker_flag(ldflags: List[str], binary_path: str) -> None:
    if not IS_WINDOWS:
        return
    pdb_flag = f"-Wl,--pdb={pdb_path_for_binary(binary_path)}"
    if pdb_flag not in ldflags:
        ldflags.append(pdb_flag)


def compiler_supports_windows_cfg(compiler_exe: Optional[str]) -> bool:
    """Return whether the selected compiler supports the project's CFG flags."""
    return is_clang_compiler(compiler_exe)


def get_x64_linker_flags(compiler_exe: Optional[str]) -> List[str]:
    """Select x64 mitigation flags without passing Clang CFG options to GCC."""
    if compiler_supports_windows_cfg(compiler_exe):
        return list(LD_OPT_FLAGS_X64)
    return [flag for flag in LD_OPT_FLAGS_X64 if flag != CFG_LINK_FLAG]


def get_x86_testapp_cfg_link_flags(compiler_exe: Optional[str]) -> List[str]:
    """Disable x86 CFG only on linkers that understand the CFG option family."""
    return list(TESTAPP_X86_CFG_LINK_FLAGS) if compiler_supports_windows_cfg(compiler_exe) else []


def get_strict_fp_flags(compiler_exe: Optional[str]) -> List[str]:
    """Return equivalent strict floating-point flags for Clang or GCC."""
    return list(CLANG_STRICT_FP_FLAGS if is_clang_compiler(compiler_exe) else GCC_STRICT_FP_FLAGS)


def get_llvm_readobj_exe() -> str:
    """Resolve a host-native llvm-readobj instead of a target Windows binary."""
    if not IS_LINUX:
        return os.path.join(MSYS2_DIR, "clang64", "bin", "llvm-readobj.exe")

    for executable in ["llvm-readobj"] + [f"llvm-readobj-{version}" for version in range(22, 13, -1)]:
        resolved = shutil.which(executable)
        if resolved:
            return resolved
    raise RuntimeError("llvm-readobj is required for Linux PE verification; install the 'llvm' package")


def get_vulkan_fg_shader_tools() -> tuple[str, str]:
    """Resolve shader tools that execute on the current build host."""
    if IS_LINUX:
        glslang = shutil.which("glslangValidator")
        spirv_val = shutil.which("spirv-val")
        if not glslang or not spirv_val:
            raise RuntimeError(
                "Vulkan FG shader build on Linux requires host glslangValidator and spirv-val; "
                "install the 'glslang-tools' and 'spirv-tools' packages"
            )
        return glslang, spirv_val

    glslang = os.path.join(MSYS2_DIR, "clang64", "bin", "glslangValidator.exe")
    spirv_val = os.path.join(MSYS2_DIR, "clang64", "bin", "spirv-val.exe")
    if not os.path.isfile(glslang) or not os.path.isfile(spirv_val):
        raise RuntimeError("Vulkan FG shader build requires bundled glslangValidator.exe and spirv-val.exe")
    return glslang, spirv_val


def make_cpp_cflags(
    opt_flags: List[str],
    *,
    compiler_exe: Optional[str] = None,
    arch_flags: Optional[List[str]] = None,
    extra_flags: Optional[List[str]] = None,
    suppress_microsoft_exception_spec: bool = False,
    production_build: bool = False,
    enable_cfg: bool = True,
) -> List[str]:
    flags = CPP_STD_FLAGS + opt_flags + COMMON_DEBUG_INFO_FLAGS + (arch_flags or []) + COMMON_WARNING_FLAGS
    if suppress_microsoft_exception_spec and (compiler_exe is None or is_clang_compiler(compiler_exe)):
        flags.append("-Wno-microsoft-exception-spec")
    flags += COMMON_WINDOWS_COMPILE_FLAGS
    if enable_cfg and (compiler_exe is None or compiler_supports_windows_cfg(compiler_exe)):
        flags.append(CFG_COMPILE_FLAG)
    flags.append("-I" + os.path.join(PROJECT_ROOT, "common"))
    if production_build:
        flags.append("-DCE_PRODUCTION_BUILD=1")
    if extra_flags:
        flags.extend(extra_flags)
    append_linux_msys2_include(flags)
    return flags


# IMGUI_URL and IMGUI_DIR removed - Custom overlay renderer replaces ImGui

FFMPEG_DIR = os.path.join(PROJECT_ROOT, "external", "ffmpeg")

PACKAGES = [
    "mingw-w64-clang-x86_64-toolchain",
    # Source-package builds use makepkg-mingw, but their build tools remain
    # precompiled MSYS2 tooling rather than shipped runtime dependencies.
    "base-devel",
    "mingw-w64-clang-x86_64-autotools",
    "mingw-w64-clang-x86_64-gettext-tools",
    "mingw-w64-clang-x86_64-gperf",
    "mingw-w64-clang-x86_64-doxygen",
    "mingw-w64-clang-x86_64-llvm-tools",
    "mingw-w64-clang-x86_64-compiler-rt",
    "mingw-w64-clang-x86_64-python",
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
    "mingw-w64-clang-x86_64-spirv-headers",
    "mingw-w64-i686-toolchain",
    "mingw-w64-i686-vulkan-headers",
    "mingw-w64-i686-vulkan-loader",
    "mingw-w64-clang-x86_64-cppwinrt",  # For Windows Graphics Capture
    "mingw-w64-clang-x86_64-gtest",
    "mingw-w64-clang-x86_64-amf-headers",
    "mingw-w64-clang-x86_64-lld",  # For delay-load support (x64 + x86 cross-compile)
    "mingw-w64-clang-x86_64-clang-tools-extra",  # For clang-format
    "make",
    "ccache",
]


def log(msg: str, *, detail: bool = False) -> None:
    timestamp = time.strftime("%Y-%m-%d %H:%M:%S")
    formatted = f"[{timestamp}] {msg}"
    if not (CONCISE_OUTPUT and detail and not VERBOSE_COMMANDS):
        print(formatted)
    with LOG_LOCK:
        if DETAIL_LOG_FILE:
            try:
                with open(DETAIL_LOG_FILE, "a", encoding="utf-8") as detail_log:
                    detail_log.write(formatted + "\n")
            except Exception:
                pass
        if not detail or not DETAIL_LOG_FILE or os.path.abspath(DETAIL_LOG_FILE) == os.path.abspath(LOG_FILE):
            try:
                with open(LOG_FILE, "a", encoding="utf-8") as summary_log:
                    summary_log.write(formatted + "\n")
            except Exception:
                pass


def log_captured_output(label: str, output: str, *, detail: bool) -> None:
    """Log captured subprocess output line-by-line without losing stream identity."""
    if not output:
        return
    for line in output.splitlines():
        log(f"[{label}] {line}", detail=detail)


def log_failure_output_tail(label: str, output: str, max_lines: int = FAILURE_OUTPUT_TAIL_LINES) -> None:
    """Expose a bounded diagnostic tail while the complete output remains in the detail log."""
    if not output:
        return
    lines = output.splitlines()
    omitted = max(0, len(lines) - max_lines)
    if omitted:
        log(f"[{label}] ... {omitted} earlier line(s) are in the detailed log")
    for line in lines[-max_lines:]:
        log(f"[{label}] {line}")


def get_workspace_temp_dir() -> str:
    """Return a repo-local temp directory used by build subprocesses."""
    os.makedirs(WORKSPACE_TEMP_DIR, exist_ok=True)
    return WORKSPACE_TEMP_DIR


def apply_workspace_temp_environment(env: Optional[Dict[str, str]] = None) -> Dict[str, str]:
    """Route temp-file usage into the workspace instead of the profile Temp tree."""
    temp_dir = get_workspace_temp_dir()
    if env is None:
        os.environ["TMP"] = temp_dir
        os.environ["TEMP"] = temp_dir
        os.environ["TMPDIR"] = temp_dir
        return dict(os.environ)

    target_env = env
    target_env["TMP"] = temp_dir
    target_env["TEMP"] = temp_dir
    target_env["TMPDIR"] = temp_dir
    return target_env


def sanitize_temp_component(value: str) -> str:
    cleaned = re.sub(r"[^A-Za-z0-9_.-]+", "_", value.strip())
    cleaned = cleaned.strip("._-")
    return cleaned or "task"


def make_task_temp_environment(base_env: Dict[str, str], temp_key: str) -> Dict[str, str]:
    task_env = base_env.copy()
    task_temp_dir = os.path.join(get_workspace_temp_dir(), "testapps", sanitize_temp_component(temp_key))
    os.makedirs(task_temp_dir, exist_ok=True)
    task_env["TMP"] = task_temp_dir
    task_env["TEMP"] = task_temp_dir
    task_env["TMPDIR"] = task_temp_dir
    return task_env


def write_text_atomic(path: str, text: str) -> None:
    tmp_path = f"{path}.tmp.{os.getpid()}"
    os.makedirs(os.path.dirname(path), exist_ok=True)
    try:
        with open(tmp_path, "w", encoding="utf-8") as f:
            f.write(text)
        os.replace(tmp_path, path)
    finally:
        if os.path.exists(tmp_path):
            try:
                os.remove(tmp_path)
            except OSError:
                pass


def write_text_atomic_if_changed(path: str, text: str) -> bool:
    """Atomically update a text file only when its contents changed."""
    try:
        with open(path, "r", encoding="utf-8") as existing:
            if existing.read() == text:
                return False
    except OSError:
        pass
    write_text_atomic(path, text)
    return True


def verification_artifact_path(filename: str) -> Optional[str]:
    if not VERIFICATION_CONTEXT:
        return None
    return os.path.join(VERIFICATION_CONTEXT["run_dir"], filename)


def record_verification_artifact(name: str, path: Optional[str]) -> None:
    if not VERIFICATION_CONTEXT or not path:
        return
    VERIFICATION_CONTEXT.setdefault("artifacts", {})[name] = os.path.abspath(path)


def write_verification_artifact(name: str, filename: str, text: str) -> Optional[str]:
    path = verification_artifact_path(filename)
    if not path:
        return None
    write_text_atomic(path, text)
    record_verification_artifact(name, path)
    return path


def record_verification_coverage(name: str, value: Any) -> None:
    if not VERIFICATION_CONTEXT:
        return
    VERIFICATION_CONTEXT.setdefault("coverage", {})[name] = value


def record_verification_step(
    name: str,
    status: str,
    *,
    duration_seconds: Optional[float] = None,
    details: Optional[Dict[str, Any]] = None,
) -> None:
    if not VERIFICATION_CONTEXT:
        return
    step: Dict[str, Any] = {"status": status}
    if duration_seconds is not None:
        step["duration_seconds"] = round(duration_seconds, 3)
    if details:
        step["details"] = details
    VERIFICATION_CONTEXT.setdefault("steps", {})[name] = step


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
            write_json_atomic(
                os.path.join(VERIFICATION_DIR, "latest_manifest.json"),
                VERIFICATION_CONTEXT,
            )
            write_text_atomic(
                os.path.join(VERIFICATION_DIR, "latest_summary.txt"),
                "\n".join(summary_lines) + "\n",
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


def read_failed_build_resume_version(
    manifest_path: Optional[str] = None, version_header_path: Optional[str] = None
) -> int:
    """Return the reusable identity of the immediately preceding failed top-level build."""
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


def setup_fg_sdk_dependencies(skip_updates: bool = False, *, include_runtime_dlls: bool) -> None:
    """Download and extract FG SDK headers plus optional Windows runtime DLLs.

    Downloads FidelityFX-SDK and Streamline-SDK archives to a cache dir,
    then extracts the headers needed to cross-compile the FG test apps. Native
    Windows builds also extract the runtime DLLs into the test app directory.
    Respects --skip-updates to avoid re-downloading.
    """
    testapp_dir = TESTAPP_BIN_DIR
    if include_runtime_dlls:
        os.makedirs(testapp_dir, exist_ok=True)
    os.makedirs(FG_SDK_CACHE_DIR, exist_ok=True)
    os.makedirs(FG_SDK_INCLUDE_DIR, exist_ok=True)

    streamline_include_dir = os.path.join(FG_SDK_INCLUDE_DIR, "streamline")
    fidelityfx_include_dir = os.path.join(FG_SDK_INCLUDE_DIR, "fidelityfx")
    fidelityfx_vk_include_dir = os.path.join(FG_SDK_INCLUDE_DIR, "fidelityfx_vk_v1_1_4")
    streamline_header_probe = os.path.join(streamline_include_dir, "include", "sl.h")
    ffx_header_probe = os.path.join(
        fidelityfx_include_dir, "Kits", "FidelityFX", "framegeneration", "include", "ffx_framegeneration.h"
    )
    # Upscaler kit header probe: forces a header re-extract on installs that predate the FSR
    # super-resolution integration (the FG header alone used to satisfy the probe).
    ffx_upscale_header_probe = os.path.join(
        fidelityfx_include_dir, "Kits", "FidelityFX", "upscalers", "include", "ffx_upscale.h"
    )
    ffx_vk_header_probe = os.path.join(fidelityfx_vk_include_dir, "ffx_api", "vk", "ffx_api_vk.h")
    ffx_vk_dll = os.path.join(testapp_dir, "amd_fidelityfx_vk.dll")
    ffx_vk_license = os.path.join(testapp_dir, "FidelityFX-SDK-v1.1.4-LICENSE.txt")

    # -- Required DLLs by test app --
    # FSR FG + FSR upscaler (super resolution): core + companion AMD runtime DLLs
    fsr_dlls = [
        "amd_fidelityfx_framegeneration_dx12.dll",
        "amd_fidelityfx_loader_dx12.dll",
        "amd_fidelityfx_upscaler_dx12.dll",
        "amd_acs_x64.dll",
        "amd_ags_x64.dll",
    ]
    # Streamline interposer loads companion .dlls + NGX DLLs at load time;
    # we must extract ALL .dll files from the zip's x64 bin dir.
    sl_known_dlls = [
        "sl.interposer.dll",
        "sl.common.dll",
        "sl.dlss_g.dll",
        "sl.dlss.dll",
        "sl.reflex.dll",
        "nvngx_dlssg.dll",
    ]

    # _nvngx.dll comes from the NVIDIA driver (DriverStore), not from Streamline SDK zip.
    # Search dynamically in case the DriverStore path changes with driver updates.
    _nvngx_sys_paths = _find_nvngx_driverstore_paths() if include_runtime_dlls else []

    missing_fsr = (
        [d for d in fsr_dlls if not os.path.exists(os.path.join(testapp_dir, d))]
        if include_runtime_dlls
        else []
    )
    missing_sl = (
        [d for d in sl_known_dlls if not os.path.exists(os.path.join(testapp_dir, d))]
        if include_runtime_dlls
        else []
    )
    missing_nvngx = include_runtime_dlls and not os.path.exists(os.path.join(testapp_dir, "_nvngx.dll"))
    missing_headers = (
        not os.path.exists(streamline_header_probe)
        or not os.path.exists(ffx_header_probe)
        or not os.path.exists(ffx_upscale_header_probe)
    )
    missing_vk_headers = not os.path.exists(ffx_vk_header_probe)
    missing_vk_runtime = include_runtime_dlls and (
        not os.path.exists(ffx_vk_dll)
        or not os.path.exists(ffx_vk_license)
        or not pe_has_authenticode_certificate(ffx_vk_dll)
    )

    if (
        not missing_fsr
        and not missing_sl
        and not missing_nvngx
        and not missing_headers
        and not missing_vk_headers
        and not missing_vk_runtime
    ):
        log("FG SDK dependencies already present - skipping download")
        return
    log(
        f"FSR FG DLLs missing: {len(missing_fsr)}, Streamline DLLs missing: {len(missing_sl)}, "
        f"headers missing: {1 if missing_headers else 0}, "
        f"Vulkan FFX headers missing: {1 if missing_vk_headers else 0}, "
        f"Vulkan FFX runtime missing: {1 if missing_vk_runtime else 0}"
    )

    def _ensure_zip(url: str, zip_name: str, expected_sha256: Optional[str] = None) -> str:
        zip_path = os.path.join(FG_SDK_CACHE_DIR, zip_name)
        if os.path.exists(zip_path) and expected_sha256:
            actual_sha256 = sha256_file(zip_path)
            if actual_sha256.lower() != expected_sha256.lower():
                log(f"Cached {zip_name} failed SHA-256 verification; replacing it")
                os.remove(zip_path)
        if not os.path.exists(zip_path):
            log(f"Downloading {zip_name}...")
            temp_zip = zip_path + ".tmp"
            try:
                if os.path.exists(temp_zip):
                    os.remove(temp_zip)
                urllib.request.urlretrieve(url, temp_zip)
                os.replace(temp_zip, zip_path)
            finally:
                if os.path.exists(temp_zip):
                    os.remove(temp_zip)
            log(f"Downloaded {zip_name}")
        else:
            log(f"Using cached {zip_name}")
        if expected_sha256:
            actual_sha256 = sha256_file(zip_path)
            if actual_sha256.lower() != expected_sha256.lower():
                raise RuntimeError(f"SHA-256 mismatch for {zip_name}: expected {expected_sha256}, got {actual_sha256}")
            log(f"Verified {zip_name} SHA-256: {actual_sha256}")
        return zip_path

    def _extract_all_dlls_from_path(zip_path: str, inner_prefix: str, dest_dir: str) -> List[str]:
        """Extract every .dll under a given prefix path from a zip archive."""
        import zipfile

        extracted = set()
        with zipfile.ZipFile(zip_path, "r") as zf:
            for entry in zf.infolist():
                fname = os.path.basename(entry.filename)
                if fname.endswith(".dll") and entry.filename.startswith(inner_prefix) and fname not in extracted:
                    zf.extract(entry, dest_dir)
                    extracted_path = os.path.normpath(os.path.join(dest_dir, entry.filename))
                    dest_path = os.path.join(dest_dir, fname)
                    if os.path.exists(extracted_path) and extracted_path != dest_path:
                        safe_replace_or_rename(extracted_path, dest_path)
                    extracted.add(fname)
        for root, dirs, files in os.walk(dest_dir, topdown=False):
            for d in dirs:
                if d in {"bin", "Samples", "Kits"} or d.startswith("amd_"):
                    dir_path = os.path.join(root, d)
                    try:
                        if os.path.isdir(dir_path) and not os.listdir(dir_path):
                            os.rmdir(dir_path)
                    except OSError:
                        pass
        return list(extracted)

    def _download_and_extract(url: str, zip_name: str, archive_inner_path: str, dlls: List[str]) -> None:
        """Download a SDK zip, extract specific DLLs to testapp_dir."""
        zip_path = _ensure_zip(url, zip_name)

        # Extract only needed DLLs from the archive
        import zipfile

        missing_from_archive = [d for d in dlls if not os.path.exists(os.path.join(testapp_dir, d))]
        if not missing_from_archive:
            return

        log(f"Extracting {len(missing_from_archive)} DLL(s) from {zip_name}...")
        extracted = set()
        with zipfile.ZipFile(zip_path, "r") as zf:
            for entry in zf.infolist():
                fname = os.path.basename(entry.filename)
                if (
                    fname in missing_from_archive
                    and entry.filename.startswith(archive_inner_path)
                    and fname not in extracted
                ):
                    zf.extract(entry, testapp_dir)
                    extracted_path = os.path.normpath(os.path.join(testapp_dir, entry.filename))
                    dest_path = os.path.join(testapp_dir, fname)
                    if os.path.exists(extracted_path) and extracted_path != dest_path:
                        safe_replace_or_rename(extracted_path, dest_path)
                    extracted.add(fname)
        log(f"Extracted {len(extracted)} DLL(s) from {zip_name}")
        # Clean up nested subdirs left by partial extraction (only SDK artifact dirs)
        sdk_parent_dirs = {"bin", "Samples", "Kits"}
        for root, dirs, files in os.walk(testapp_dir, topdown=False):
            for d in dirs:
                if d in sdk_parent_dirs or d.startswith("amd_"):
                    dir_path = os.path.join(root, d)
                    try:
                        if os.path.isdir(dir_path) and not os.listdir(dir_path):
                            os.rmdir(dir_path)
                    except OSError:
                        pass

    def _extract_streamline_headers(zip_path: str) -> None:
        import zipfile

        extracted = 0
        with zipfile.ZipFile(zip_path, "r") as zf:
            for entry in zf.infolist():
                if entry.is_dir() or not entry.filename.startswith("include/"):
                    continue
                if not entry.filename.lower().endswith((".h", ".hpp")):
                    continue
                dest_path = os.path.join(streamline_include_dir, entry.filename)
                os.makedirs(os.path.dirname(dest_path), exist_ok=True)
                with zf.open(entry, "r") as src, open(dest_path, "wb") as dst:
                    shutil.copyfileobj(src, dst)
                extracted += 1
        log(f"Extracted {extracted} Streamline header(s)")

    def _extract_fidelityfx_headers(zip_path: str) -> None:
        import zipfile

        allowed_prefixes = (
            "Kits/FidelityFX/api/include/",
            "Kits/FidelityFX/framegeneration/include/",
            # ffx_upscale.h/.hpp only -- the per-implementation fsr3/ and gpu/ shader headers live
            # under upscalers/fsr3/include/ and are intentionally NOT matched by this prefix.
            "Kits/FidelityFX/upscalers/include/",
        )
        extracted = 0
        with zipfile.ZipFile(zip_path, "r") as zf:
            for entry in zf.infolist():
                if entry.is_dir() or not entry.filename.lower().endswith((".h", ".hpp")):
                    continue
                marker_index = entry.filename.find("Kits/FidelityFX/")
                if marker_index < 0:
                    continue
                rel_path = entry.filename[marker_index:]
                if not rel_path.startswith(allowed_prefixes):
                    continue
                dest_path = os.path.join(fidelityfx_include_dir, rel_path)
                os.makedirs(os.path.dirname(dest_path), exist_ok=True)
                with zf.open(entry, "r") as src, open(dest_path, "wb") as dst:
                    shutil.copyfileobj(src, dst)
                extracted += 1
        log(f"Extracted {extracted} FidelityFX header(s)")

    def _extract_fidelityfx_vulkan_package(zip_path: str) -> None:
        import zipfile

        extracted_headers = 0
        extracted_dll = False
        extracted_license = False
        with zipfile.ZipFile(zip_path, "r") as zf:
            for entry in zf.infolist():
                normalized = entry.filename.replace("\\", "/")
                if entry.is_dir():
                    continue
                if normalized.startswith("ffx-api/include/") and normalized.lower().endswith((".h", ".hpp")):
                    rel_path = normalized[len("ffx-api/include/") :]
                    dest_path = os.path.join(fidelityfx_vk_include_dir, *rel_path.split("/"))
                    os.makedirs(os.path.dirname(dest_path), exist_ok=True)
                    with zf.open(entry, "r") as src, open(dest_path, "wb") as dst:
                        shutil.copyfileobj(src, dst)
                    extracted_headers += 1
                elif include_runtime_dlls and normalized == "PrebuiltSignedDLL/amd_fidelityfx_vk.dll":
                    with zf.open(entry, "r") as src, open(ffx_vk_dll, "wb") as dst:
                        shutil.copyfileobj(src, dst)
                    extracted_dll = True
                elif include_runtime_dlls and normalized == "sdk/LICENSE.txt":
                    with zf.open(entry, "r") as src, open(ffx_vk_license, "wb") as dst:
                        shutil.copyfileobj(src, dst)
                    extracted_license = True
        if not os.path.exists(ffx_vk_header_probe):
            raise RuntimeError("FidelityFX 1.1.4 Vulkan API headers were not found in the pinned archive")
        if include_runtime_dlls:
            if not os.path.exists(ffx_vk_dll) or not pe_has_authenticode_certificate(ffx_vk_dll):
                raise RuntimeError("FidelityFX 1.1.4 Vulkan runtime is missing its Authenticode certificate")
            if not os.path.exists(ffx_vk_license):
                raise RuntimeError("FidelityFX 1.1.4 license was not found in the pinned archive")
        if include_runtime_dlls:
            log(
                f"Prepared FidelityFX 1.1.4 Vulkan package: {extracted_headers} headers, "
                f"DLL={'yes' if extracted_dll else 'cached'}, license={'yes' if extracted_license else 'cached'}"
            )
        else:
            log(f"Prepared FidelityFX 1.1.4 Vulkan headers: {extracted_headers} extracted")

    if missing_fsr:
        _download_and_extract(
            FFX_SDK_URL, FFX_SDK_ZIP_NAME, "Samples/Upscalers/FidelityFX_FSR/dx12/x64/Release/", fsr_dlls
        )

    # Download and extract Streamline DLLs if any missing
    if missing_sl:
        zip_path_sl = _ensure_zip(STREAMLINE_SDK_URL, STREAMLINE_SDK_ZIP_NAME)
        # Streamline: extract ALL .dll files under bin/x64/ (interposer needs companion DLLs + NGX)
        extracted_sl = _extract_all_dlls_from_path(zip_path_sl, "bin/x64/", testapp_dir)
        log(f"Extracted {len(extracted_sl)} Streamline DLL(s)")
        missing_sl = [d for d in sl_known_dlls if not os.path.exists(os.path.join(testapp_dir, d))]

    if not os.path.exists(streamline_header_probe):
        _extract_streamline_headers(_ensure_zip(STREAMLINE_SDK_URL, STREAMLINE_SDK_ZIP_NAME))

    if not os.path.exists(ffx_header_probe) or not os.path.exists(ffx_upscale_header_probe):
        _extract_fidelityfx_headers(_ensure_zip(FFX_SDK_SOURCE_URL, FFX_SDK_SOURCE_ZIP_NAME))

    if missing_vk_headers or missing_vk_runtime:
        _extract_fidelityfx_vulkan_package(_ensure_zip(FFX_VK_SDK_URL, FFX_VK_SDK_ZIP_NAME, FFX_VK_SDK_SHA256))
    elif include_runtime_dlls and not pe_has_authenticode_certificate(ffx_vk_dll):
        raise RuntimeError("Cached FidelityFX 1.1.4 Vulkan runtime has no Authenticode certificate")

    # Copy _nvngx.dll from NVIDIA driver DriverStore if not present
    nvngx_dest = os.path.join(testapp_dir, "_nvngx.dll")
    if include_runtime_dlls and not os.path.exists(nvngx_dest):
        for src in _nvngx_sys_paths:
            if os.path.exists(src):
                shutil.copy2(src, nvngx_dest)
                log("Copied _nvngx.dll from DriverStore")
                break
        else:
            log("_nvngx.dll not found in DriverStore (NVIDIA NGX not activated on this system)")

    # Final report
    if include_runtime_dlls:
        all_expected = fsr_dlls + sl_known_dlls + ["_nvngx.dll"]
        still_missing = [d for d in all_expected if not os.path.exists(os.path.join(testapp_dir, d))]
        if still_missing:
            log(f"Warning: some FG DLLs could not be extracted: {still_missing}")
        else:
            log("All FG SDK DLLs ready for test apps")
    else:
        log("All FG SDK headers ready for cross-compiled test apps")


def setup_fg_sdk_headers(skip_updates: bool = False) -> None:
    """Prepare only the SDK headers required to cross-compile FG test apps."""
    setup_fg_sdk_dependencies(skip_updates=skip_updates, include_runtime_dlls=False)


def setup_fg_sdk_dlls(skip_updates: bool = False) -> None:
    """Prepare SDK headers and Windows runtime DLLs for native test apps."""
    setup_fg_sdk_dependencies(skip_updates=skip_updates, include_runtime_dlls=True)


def setup_fg_sdk_for_host(skip_updates: bool = False) -> None:
    """Prepare compile-time SDK inputs and native-only runtime payloads."""
    if IS_LINUX:
        setup_fg_sdk_headers(skip_updates=skip_updates)
    else:
        setup_fg_sdk_dlls(skip_updates=skip_updates)


def safe_replace_or_rename(src: str, dst: str) -> None:
    """Rename or copy+delete src to dst, handling cross-device moves."""
    try:
        os.replace(src, dst)
    except OSError:
        shutil.copy2(src, dst)
        os.remove(src)


def get_fg_sdk_include_flags() -> List[str]:
    """Return generated SDK include paths used by the DX12 FG test apps."""
    streamline_include = os.path.join(FG_SDK_INCLUDE_DIR, "streamline", "include")
    fidelityfx_root = os.path.join(FG_SDK_INCLUDE_DIR, "fidelityfx", "Kits", "FidelityFX")
    return [
        "-I" + streamline_include,
        "-I" + os.path.join(fidelityfx_root, "api", "include"),
        "-I" + os.path.join(fidelityfx_root, "framegeneration", "include"),
        "-I" + os.path.join(fidelityfx_root, "upscalers", "include"),
    ]


def get_vulkan_fg_sdk_include_flags() -> List[str]:
    """Return the isolated Streamline 2.11.1 + FidelityFX 1.1.4 Vulkan include paths."""
    return [
        "-I" + os.path.join(FG_SDK_INCLUDE_DIR, "streamline", "include"),
        "-I" + os.path.join(FG_SDK_INCLUDE_DIR, "fidelityfx_vk_v1_1_4"),
    ]


def compile_vulkan_fg_shaders(env: Dict[str, str]) -> str:
    """Compile, validate, and embed Vulkan FG shaders. Returns the generated include directory."""
    shader_dir = os.path.join(PROJECT_ROOT, "testapp", "shaders")
    output_dir = os.path.join(OBJ_DIR, "vulkan_fg_shaders")
    os.makedirs(output_dir, exist_ok=True)
    glslang, spirv_val = get_vulkan_fg_shader_tools()

    shader_specs = [
        ("vulkan_fg_fullscreen.vert", "vert", "kFullscreenVertexSpirv"),
        ("vulkan_fg_scene.frag", "frag", "kSceneFragmentSpirv"),
        ("vulkan_fg_taa.frag", "frag", "kTaaFragmentSpirv"),
        ("vulkan_fg_ui.frag", "frag", "kUiFragmentSpirv"),
        ("vulkan_fg_compose.frag", "frag", "kComposeFragmentSpirv"),
        ("vulkan_fg_present.frag", "frag", "kPresentFragmentSpirv"),
    ]
    embedded: List[tuple[str, bytes]] = []
    compiled_count = 0
    for source_name, stage, symbol in shader_specs:
        source_path = os.path.join(shader_dir, source_name)
        spv_path = os.path.join(output_dir, source_name + ".spv")
        cache_path = spv_path + ".build-cache.json"
        if not os.path.exists(source_path):
            raise RuntimeError(f"Missing Vulkan FG shader source: {source_path}")
        compile_command = [glslang, "-V", "--target-env", "vulkan1.2", "-S", stage, source_path, "-o", spv_path]
        signature_digest = hashlib.sha256()
        signature_digest.update(sha256_file(source_path).encode("ascii"))
        signature_digest.update(fingerprint_link_input(glslang).encode("ascii"))
        signature_digest.update(fingerprint_link_input(spirv_val).encode("ascii"))
        signature_digest.update("\0".join(compile_command[1:]).encode("utf-8", errors="surrogatepass"))
        input_signature = signature_digest.hexdigest()
        cache_valid = False
        if env.get("FORCE_REBUILD") != "1" and os.path.isfile(spv_path):
            try:
                with open(cache_path, "r", encoding="utf-8") as cache_file:
                    cache = json.load(cache_file)
                cache_valid = (
                    cache.get("input_signature") == input_signature
                    and cache.get("output_sha256") == sha256_file(spv_path)
                )
            except (OSError, json.JSONDecodeError):
                cache_valid = False
        if not cache_valid:
            run_command(compile_command, env=env)
            write_json_atomic(
                cache_path,
                {
                    "input_signature": input_signature,
                    "output_sha256": sha256_file(spv_path),
                },
            )
            compiled_count += 1
        run_command([spirv_val, "--target-env", "vulkan1.2", spv_path], env=env)
        with open(spv_path, "rb") as src:
            payload = src.read()
        if not payload or len(payload) % 4 != 0:
            raise RuntimeError(f"Invalid SPIR-V payload generated for {source_name}")
        embedded.append((symbol, payload))

    header_lines = [
        "// Generated by build.py from validated GLSL. Do not edit.\n#pragma once\n\n#include <cstdint>\n\n",
        "namespace testapp::vkfg::shaders {\n",
    ]
    for symbol, payload in embedded:
        words = [int.from_bytes(payload[offset : offset + 4], "little") for offset in range(0, len(payload), 4)]
        header_lines.append(f"inline constexpr uint32_t {symbol}[] = {{\n")
        for offset in range(0, len(words), 8):
            chunk = ", ".join(f"0x{word:08x}u" for word in words[offset : offset + 8])
            header_lines.append(f"    {chunk},\n")
        header_lines.append("};\n")
    header_lines.append("}  // namespace testapp::vkfg::shaders\n")
    header_path = os.path.join(output_dir, "vulkan_fg_shaders.h")
    header_changed = write_text_atomic_if_changed(header_path, "".join(header_lines))
    log(
        f"Vulkan FG shaders: {compiled_count} compiled, {len(shader_specs) - compiled_count} cached, "
        f"{len(shader_specs)} validated, header {'updated' if header_changed else 'unchanged'}"
    )
    return output_dir


def check_python_lsp_tools() -> bool:
    """Check and install Python LSP/lint/format tools for better IDE support."""
    bootstrap_env = apply_workspace_temp_environment(os.environ.copy())
    bootstrap_ok = True

    user_scripts_dir = os.path.join(site.getuserbase(), "Scripts" if IS_WINDOWS else "bin")
    if user_scripts_dir not in bootstrap_env.get("PATH", ""):
        bootstrap_env["PATH"] = user_scripts_dir + os.pathsep + bootstrap_env.get("PATH", "")

    try:
        subprocess.run(
            [sys.executable, "-m", "pip", "--version"],
            capture_output=True,
            check=True,
            env=bootstrap_env,
        )
    except (subprocess.CalledProcessError, FileNotFoundError):
        log("pip not found. Bootstrapping with ensurepip...")
        try:
            run_logged_subprocess(
                [sys.executable, "-m", "ensurepip", "--upgrade"],
                check=True,
                env=bootstrap_env,
            )
        except subprocess.CalledProcessError as e:
            log(f"Warning: Failed to bootstrap pip: {e}")
            log("Python lint tooling cannot be prepared; the lint stage will report the missing tools.")
            return False

    tools = ["pyright", "flake8", "black"]

    for tool in tools:
        try:
            subprocess.run(
                [sys.executable, "-m", tool, "--version"],
                capture_output=True,
                check=True,
                env=bootstrap_env,
            )
        except (subprocess.CalledProcessError, FileNotFoundError):
            log(f"{tool} not found. Installing via pip...")
            try:
                cmd = [
                    sys.executable,
                    "-m",
                    "pip",
                    "install",
                    "--disable-pip-version-check",
                    "--upgrade-strategy",
                    "only-if-needed",
                ]
                if not is_virtual_environment():
                    cmd.append("--user")
                if IS_LINUX and not is_virtual_environment():
                    cmd.append("--break-system-packages")
                cmd.append(tool)
                run_logged_subprocess(cmd, check=True, env=bootstrap_env)
                log(f"{tool} installed successfully.")
            except subprocess.CalledProcessError as e:
                bootstrap_ok = False
                log(f"Warning: Failed to install {tool}: {e}")
                log(f"{tool} remains unavailable; the lint stage will report the missing managed tool.")
    return bootstrap_ok


def get_linux_ffmpeg_root() -> str:
    local_import_lib = os.path.join(FFMPEG_DIR, "lib", "libavcodec.dll.a")
    if os.path.exists(local_import_lib):
        return FFMPEG_DIR
    return os.path.join(get_linux_msys2_dir(), "clang64")


def find_ffmpeg_include_dir(ffmpeg_root: str) -> str:
    include_dir = os.path.join(ffmpeg_root, "include")
    if not os.path.isdir(include_dir):
        return include_dir

    if os.path.exists(os.path.join(include_dir, "libavcodec", "avcodec.h")):
        return include_dir

    for entry in sorted(os.listdir(include_dir)):
        candidate = os.path.join(include_dir, entry)
        if entry.startswith("ffmpeg") and os.path.isdir(candidate):
            return candidate

    return include_dir


def get_linux_ffmpeg_build_flags(env: Dict[str, str], pkg_config: Optional[str]) -> tuple[List[str], List[str]]:
    ffmpeg_root = get_linux_ffmpeg_root()
    ffmpeg_include = find_ffmpeg_include_dir(ffmpeg_root)
    ffmpeg_lib = os.path.join(ffmpeg_root, "lib")
    msys2_include_root = os.path.join(get_linux_msys2_dir(), "clang64", "include")
    env_ffmpeg = env.copy()
    env_ffmpeg["PKG_CONFIG_PATH"] = os.pathsep.join(
        [os.path.join(ffmpeg_lib, "pkgconfig"), env_ffmpeg.get("PKG_CONFIG_PATH", "")]
    ).rstrip(os.pathsep)

    pkgs = ["libavcodec", "libavformat", "libavutil", "libswresample", "libswscale"]
    ffmpeg_flags: List[str]
    if pkg_config:
        try:
            ffmpeg_flags_raw = run_command([pkg_config, "--cflags", "--libs"] + pkgs, env=env_ffmpeg).strip().split()

            ffmpeg_flags = []
            msys2_dir = get_linux_msys2_dir()
            for flag in ffmpeg_flags_raw:
                if flag in ["-ldl", "-lshaderc_shared"]:
                    continue
                if flag.startswith("-I/") and not os.path.exists(flag[2:]):
                    abs_path = os.path.join(msys2_dir, flag[2:].lstrip("/"))
                    if os.path.exists(abs_path):
                        flag = "-I" + abs_path
                elif flag.startswith("-L/") and not os.path.exists(flag[2:]):
                    abs_path = os.path.join(msys2_dir, flag[2:].lstrip("/"))
                    if os.path.exists(abs_path):
                        flag = "-L" + abs_path
                if flag == "-I" + msys2_include_root:
                    ffmpeg_flags.extend(["-idirafter", msys2_include_root])
                else:
                    ffmpeg_flags.append(flag)
        except Exception as e:
            log(f"pkg-config failed, using manual FFmpeg paths: {e}")
            ffmpeg_flags = ["-I" + ffmpeg_include, "-L" + ffmpeg_lib]
    else:
        ffmpeg_flags = ["-I" + ffmpeg_include, "-L" + ffmpeg_lib]

    ffmpeg_import_lib_names = [
        "libavformat.dll.a",
        "libavcodec.dll.a",
        "libswresample.dll.a",
        "libswscale.dll.a",
        "libavutil.dll.a",
    ]
    ffmpeg_import_libs = []
    for import_lib_name in ffmpeg_import_lib_names:
        import_lib_path = os.path.join(ffmpeg_lib, import_lib_name)
        if os.path.exists(import_lib_path):
            ffmpeg_import_libs.append(import_lib_path)

    if len(ffmpeg_import_libs) != len(ffmpeg_import_lib_names):
        ffmpeg_import_libs = [
            "-L" + ffmpeg_lib,
            "-lavformat",
            "-lavcodec",
            "-lswresample",
            "-lswscale",
            "-lavutil",
        ]

    return ffmpeg_flags, ffmpeg_import_libs


# MSYS2 packages to download for Linux builds (Windows-specific libs not in Arch repos)
LINUX_MSYS2_PACKAGES = [
    "mingw-w64-clang-x86_64-vulkan-headers",
    "mingw-w64-clang-x86_64-vulkan-loader",
    "mingw-w64-clang-x86_64-spirv-headers",
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
    "mingw-w64-clang-x86_64-svt-av1",
    "mingw-w64-clang-x86_64-opus",
    "mingw-w64-clang-x86_64-libwinpthread",
    "mingw-w64-clang-x86_64-crt",
]

LINUX_MSYS2_REQUIRED_SENTINELS = [
    os.path.join("clang64", "share", "licenses", "crt", "COPYING.MinGW-w64-runtime.txt"),
]

MSYS2_REPO_URL = "https://repo.msys2.org/mingw/clang64"
MSYS2_MINGW64_REPO_URL = "https://repo.msys2.org/mingw/mingw64"


def get_linux_msys2_gtest_subdir(arch: str = "x64", compiler_exe: Optional[str] = None) -> str:
    if arch == "x86":
        return "mingw32"
    compiler = compiler_exe or get_compiler_exe(arch)
    return "clang64" if is_clang_compiler(compiler) else "mingw64"


def get_linux_msys2_gtest_package_name(arch: str = "x64") -> str:
    subdir = get_linux_msys2_gtest_subdir(arch)
    if subdir == "clang64":
        return "mingw-w64-clang-x86_64-gtest"
    if subdir == "mingw64":
        return "mingw-w64-x86_64-gtest"
    return "mingw-w64-i686-gtest"


def get_linux_msys2_packages() -> List[str]:
    return [
        LINUX_MSYS2_PACKAGES[0],
        LINUX_MSYS2_PACKAGES[1],
        LINUX_MSYS2_PACKAGES[2],
        get_linux_msys2_gtest_package_name(),
        *LINUX_MSYS2_PACKAGES[3:],
    ]


def get_linux_msys2_required_sentinels() -> List[str]:
    return LINUX_MSYS2_REQUIRED_SENTINELS + [os.path.join(get_linux_msys2_gtest_subdir("x64"), "lib", "libgtest.a")]


def get_linux_msys2_repo_url_for_package(pkg: str) -> str:
    if pkg.startswith("mingw-w64-clang-"):
        return MSYS2_REPO_URL
    if pkg.startswith("mingw-w64-x86_64-"):
        return MSYS2_MINGW64_REPO_URL
    return MSYS2_REPO_URL


def extract_linux_msys2_package(pkg_path: str, destination: str) -> None:
    tar_exe = shutil.which("tar")
    if not tar_exe:
        raise RuntimeError("Linux MSYS2 package extraction requires a 'tar' executable")

    extract_cmds = [
        [tar_exe, "-xf", pkg_path, "-C", destination],
        [tar_exe, "--zstd", "-xf", pkg_path, "-C", destination],
    ]
    errors = []
    for cmd in extract_cmds:
        result = subprocess.run(cmd, capture_output=True, text=True)
        if result.returncode == 0:
            return
        errors.append(result.stderr.strip() or result.stdout.strip() or "unknown tar error")

    raise RuntimeError(f"Failed to extract {os.path.basename(pkg_path)}: {' | '.join(errors)}")


def linux_msys2_packages_complete(msys_linux_dir: str) -> bool:
    clang64_dir = os.path.join(msys_linux_dir, "clang64")
    include_dir = os.path.join(clang64_dir, "include")
    if not (os.path.isdir(include_dir) and os.listdir(include_dir)):
        return False

    for rel_path in get_linux_msys2_required_sentinels():
        if not os.path.exists(os.path.join(msys_linux_dir, rel_path)):
            return False

    return True


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

    # Check if packages already extracted (marker file + required sentinels present).
    if os.path.exists(marker_file) and linux_msys2_packages_complete(msys_linux_dir):
        log("MSYS2 packages already installed - skipping setup")
        return msys_linux_dir

    if os.path.exists(marker_file):
        log("MSYS2 package tree incomplete - refreshing setup")

    log("Setting up MSYS2 packages for Linux cross-compilation...")
    os.makedirs(msys_linux_dir, exist_ok=True)
    os.makedirs(os.path.join(clang64_dir, "include"), exist_ok=True)
    os.makedirs(os.path.join(clang64_dir, "lib"), exist_ok=True)

    repo_html_cache: Dict[str, str] = {}

    for pkg in get_linux_msys2_packages():
        try:
            repo_url = get_linux_msys2_repo_url_for_package(pkg)
            repo_html = repo_html_cache.get(repo_url)
            if repo_html is None:
                req = urllib.request.Request(repo_url, headers={"User-Agent": "Mozilla/5.0"})
                with urllib.request.urlopen(req, timeout=30) as response:
                    repo_html = response.read().decode("utf-8", errors="replace")
                repo_html_cache[repo_url] = repo_html

            # Find package file in repo (with version)
            log(f"Checking for {pkg} in MSYS2 repo...")
            pattern = rf'href="({re.escape(pkg)}[^"]*\.pkg\.tar\.zst)"'
            match = re.search(pattern, repo_html)
            if not match:
                raise RuntimeError(f"Could not find package {pkg} in {repo_url}")

            pkg_file = match.group(1)
            pkg_url = f"{repo_url}/{pkg_file}"
            pkg_path = os.path.join(msys_linux_dir, pkg_file)

            # Download if not exists
            if not os.path.exists(pkg_path):
                log(f"Downloading {pkg_file}...")
                urllib.request.urlretrieve(pkg_url, pkg_path)

            # Extract package
            log(f"Extracting {pkg_file}...")
            extract_linux_msys2_package(pkg_path, msys_linux_dir)

            log(f"Package {pkg} ready")

        except Exception as e:
            raise RuntimeError(f"Failed to prepare Linux MSYS2 package {pkg}: {e}") from e

    verify_msys2_ffmpeg_build_deps(msys_linux_dir)

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


def get_linux_msys2_lib_dir(arch: str = "x64") -> str:
    msys2_dir = get_linux_msys2_dir()
    if arch == "x86":
        return os.path.join(msys2_dir, "mingw32", "lib")
    return os.path.join(msys2_dir, "clang64", "lib")


def get_linux_msys2_gtest_lib_dir(arch: str = "x64") -> str:
    return os.path.join(get_linux_msys2_dir(), get_linux_msys2_gtest_subdir(arch), "lib")


def get_host_msys2_dir():
    return get_linux_msys2_dir() if IS_LINUX else MSYS2_DIR


def resolve_msys2_gtest_link_inputs(lib_dir: str, *, prefer_static: bool = False) -> List[str]:
    """Resolve GoogleTest link inputs from an extracted MSYS2 MinGW lib dir."""

    def pick(candidates: List[str]) -> Optional[str]:
        for candidate in candidates:
            if os.path.exists(candidate):
                return candidate
        return None

    if prefer_static:
        gtest_main = pick(
            [
                os.path.join(lib_dir, "libgtest_main.a"),
                os.path.join(lib_dir, "libgtest_main.dll.a"),
            ]
        )
        gtest = pick(
            [
                os.path.join(lib_dir, "libgtest.a"),
                os.path.join(lib_dir, "libgtest.dll.a"),
            ]
        )
    else:
        gtest_main = pick(
            [
                os.path.join(lib_dir, "libgtest_main.dll.a"),
                os.path.join(lib_dir, "libgtest_main.a"),
            ]
        )
        gtest = pick(
            [
                os.path.join(lib_dir, "libgtest.dll.a"),
                os.path.join(lib_dir, "libgtest.a"),
            ]
        )

    if gtest_main and gtest:
        return [gtest_main, gtest]

    missing = []
    if not gtest_main:
        missing.append("libgtest_main(.dll.a/.a)")
    if not gtest:
        missing.append("libgtest(.dll.a/.a)")
    raise RuntimeError(f"Missing MSYS2 GoogleTest libraries in {lib_dir}: {', '.join(missing)}")


# ============================================================================
# ImGui Setup - REMOVED: No longer using ImGui
# Custom overlay renderer (custom_overlay) replaces ImGui
# ============================================================================

# --- FFmpeg Configuration ---
FFMPEG_URL = "https://git.ffmpeg.org/ffmpeg.git"
FFNVCODEC_URL = "https://git.videolan.org/git/ffmpeg/nv-codec-headers.git"
FFMPEG_DEPENDENCY_MANIFEST = os.path.join(PROJECT_ROOT, "ffmpeg_dependencies.json")
FFMPEG_DEPENDENCY_MANIFEST_DATA = load_dependency_manifest(FFMPEG_DEPENDENCY_MANIFEST)
FFMPEG_DEPENDENCY_PREFIX = dependency_prefix(PROJECT_ROOT)
FFMPEG_RUNTIME_DLL_PATTERNS = [
    "avcodec-*.dll",
    "avformat-*.dll",
    "avutil-*.dll",
    "swresample-*.dll",
    "swscale-*.dll",
]
WINDOWS_FFMPEG_RUNTIME_DEPS = manifest_runtime_dlls(FFMPEG_DEPENDENCY_MANIFEST_DATA)
WINDOWS_FFMPEG_OPTIONAL_RUNTIME_DEPS = manifest_runtime_dlls(FFMPEG_DEPENDENCY_MANIFEST_DATA, optional=True)
WINDOWS_SANITIZER_RUNTIME_DEPS = [
    "libclang_rt.asan_dynamic-x86_64.dll",
    "libc++.dll",  # Required by ASan runtime; shared with FFmpeg via libvpl-2.dll
]
LINUX_FFMPEG_RUNTIME_DEPS = [
    "libbz2-1.dll",
    "libxml2-16.dll",
    "libmodplug-1.dll",
    "libgme.dll",
    "libiconv-2.dll",
    "libc++.dll",
    "libva.dll",
    "libva_win32.dll",
    "libvpl-2.dll",
    "libopus-0.dll",
    "libwinpthread-1.dll",
    "libgcc_s_seh-1.dll",
    "libstdc++-6.dll",
]


def get_windows_ffmpeg_runtime_deps(runtime_bin: str) -> List[str]:
    """Return mandatory deps plus libcharset only when libiconv imports it."""
    deps = list(WINDOWS_FFMPEG_RUNTIME_DEPS)
    libcharset_name = "libcharset-1.dll"
    libcharset_path = os.path.join(runtime_bin, libcharset_name)
    if not os.path.exists(libcharset_path):
        return deps

    objdump_exe = os.path.join(MSYS2_DIR, "clang64", "bin", "llvm-objdump.exe")
    libiconv_path = os.path.join(runtime_bin, "libiconv-2.dll")
    if not os.path.exists(objdump_exe) or not os.path.exists(libiconv_path):
        raise RuntimeError("Cannot determine whether libiconv imports optional libcharset-1.dll")
    result = subprocess.run([objdump_exe, "-p", libiconv_path], capture_output=True, text=True, check=True)
    if libcharset_name.lower() in parse_pe_import_names(result.stdout):
        deps.append(libcharset_name)
        log("[FFmpeg] libiconv imports libcharset-1.dll; including the optional runtime DLL")
    else:
        log("[FFmpeg] libiconv does not import libcharset-1.dll; leaving it out of the bundle")
    return deps


def get_windows_ffmpeg_pkg_config_path(existing: str = "") -> str:
    paths = [
        os.path.join(FFMPEG_DEPENDENCY_PREFIX, "lib", "pkgconfig"),
        os.path.join(FFMPEG_DIR, "lib", "pkgconfig"),
    ]
    if existing:
        paths.append(existing)
    return os.pathsep.join(paths)


def to_unix(p):
    """Convert Windows path to MSYS2 Unix path."""
    p = p.replace("\\", "/")
    if len(p) >= 2 and p[1] == ":":
        drive = p[0].lower()
        return "/" + drive + p[2:]
    return p


def get_ffmpeg_runtime_dlls(ffmpeg_bin_src):
    dlls = []
    seen = set()
    for pattern in FFMPEG_RUNTIME_DLL_PATTERNS:
        for dll in sorted(glob.glob(os.path.join(ffmpeg_bin_src, pattern))):
            name = os.path.basename(dll).lower()
            if name in seen:
                continue
            seen.add(name)
            dlls.append(dll)
    return dlls


def resolve_ffmpeg_runtime_dll_names(ffmpeg_bin_src):
    """Resolve the current versioned FFmpeg DLL names from the install tree."""
    resolved = {}
    for prefix in ("avcodec", "avformat", "avutil", "swresample", "swscale"):
        matches = sorted(glob.glob(os.path.join(ffmpeg_bin_src, f"{prefix}-*.dll")))
        if not matches:
            raise RuntimeError(f"Missing FFmpeg runtime DLL for {prefix} in {ffmpeg_bin_src}")

        # A previous FFmpeg install can leave an older major-version DLL behind.
        # Keep the highest numeric suffix and let the destination sync remove the
        # stale copy instead of linking against a hard-coded historical version.
        def version_key(path):
            match = re.search(r"-(\d+)\.dll$", os.path.basename(path), re.IGNORECASE)
            return (int(match.group(1)) if match else -1, os.path.basename(path).lower())

        selected = max(matches, key=version_key)
        if len(matches) > 1:
            log(
                f"[FFmpeg] Multiple {prefix} runtime DLLs found; selecting {os.path.basename(selected)} "
                f"and removing stale versions from the bundle"
            )
        resolved[prefix] = os.path.basename(selected)
    return resolved


def sync_ffmpeg_runtime_dlls(
    ffmpeg_bin_src,
    ffmpeg_bin_dst,
    runtime_deps,
    extra_search_dirs,
    required_runtime_deps=False,
    private_runtime_root=None,
):
    resolved_names = resolve_ffmpeg_runtime_dll_names(ffmpeg_bin_src)
    ffmpeg_dlls = [os.path.join(ffmpeg_bin_src, name) for name in resolved_names.values()]

    dep_sources = {}
    for dep in runtime_deps:
        for search_dir in extra_search_dirs:
            src = os.path.join(search_dir, dep)
            if os.path.exists(src):
                dep_sources[dep] = src
                break

    missing_deps = [dep for dep in runtime_deps if dep not in dep_sources]
    if missing_deps:
        if required_runtime_deps:
            raise RuntimeError(
                "Required source-built FFmpeg runtime dependencies are missing: " + ", ".join(sorted(missing_deps))
            )
        log(
            "[FFmpeg] Optional runtime dependencies not found in configured search paths: "
            + ", ".join(sorted(missing_deps))
        )

    if private_runtime_root:
        for dependency, source in dep_sources.items():
            if not is_path_within(source, private_runtime_root):
                raise RuntimeError(
                    f"FFmpeg runtime dependency {dependency} resolved outside the private source prefix: {source}"
                )

    keep_names = {os.path.basename(dll).lower() for dll in ffmpeg_dlls}
    keep_names.update(dep.lower() for dep in dep_sources)

    os.makedirs(ffmpeg_bin_dst, exist_ok=True)
    for existing in glob.glob(os.path.join(ffmpeg_bin_dst, "*.dll")):
        existing_name = os.path.basename(existing).lower()
        if existing_name in keep_names:
            continue
        if safe_delete_file(existing):
            log(f"Removed stale FFmpeg/runtime DLL {os.path.basename(existing)}")
        else:
            log(f"WARNING: Could not remove stale FFmpeg/runtime DLL {os.path.basename(existing)}")

    copied_count = 0
    cached_count = 0
    for dll in ffmpeg_dlls:
        dst = os.path.join(ffmpeg_bin_dst, os.path.basename(dll))
        success, copied = safe_copy_file_if_changed(dll, dst)
        if not success:
            raise RuntimeError(f"Failed to copy {os.path.basename(dll)} to {ffmpeg_bin_dst}")
        copied_count += int(copied)
        cached_count += int(not copied)
        log(f"{'Copied' if copied else 'Kept'} {os.path.basename(dll)} in ffmpeg dir", detail=True)

    for dep in runtime_deps:
        src = dep_sources.get(dep)
        if not src:
            continue
        dst = os.path.join(ffmpeg_bin_dst, dep)
        success, copied = safe_copy_file_if_changed(src, dst)
        if not success:
            raise RuntimeError(f"Failed to copy runtime dependency {dep} to {ffmpeg_bin_dst}")
        copied_count += int(copied)
        cached_count += int(not copied)
        log(f"{'Copied' if copied else 'Kept'} runtime dep {dep} in ffmpeg dir", detail=True)

    log(f"FFmpeg runtime sync: {copied_count} copied, {cached_count} unchanged")

    if private_runtime_root:
        objdump_exe = os.path.join(MSYS2_DIR, "clang64", "bin", "llvm-objdump.exe")
        if not os.path.exists(objdump_exe):
            raise RuntimeError(f"Missing LLVM PE import checker: {objdump_exe}")
        verify_pe_import_closure(ffmpeg_bin_dst, objdump_exe, logger=log)


def remove_redundant_root_runtime_dlls(root_dir: str, runtime_deps: List[str]) -> None:
    for dep_name in sorted(set(runtime_deps)):
        dep_path = os.path.join(root_dir, dep_name)
        if not os.path.exists(dep_path):
            continue
        if safe_delete_file(dep_path):
            log(f"Removed redundant root runtime DLL {dep_name}")
        else:
            log(f"WARNING: Could not remove redundant root runtime DLL {dep_name}")


def sync_windows_sanitizer_runtime_dlls(target_dir: str) -> None:
    if IS_LINUX:
        return

    clang_bin = os.path.join(MSYS2_DIR, "clang64", "bin")
    os.makedirs(target_dir, exist_ok=True)
    for dll_name in WINDOWS_SANITIZER_RUNTIME_DEPS:
        if dll_name.lower() == "libclang_rt.asan_dynamic-x86_64.dll":
            # ASan is a developer-only build runtime.  It remains resolved
            # from the MSYS2 toolchain during sanitizer tests and must never
            # become a product-bundle dependency copied from clang64/bin.
            log("Leaving the MSYS2 ASan developer runtime out of the product bundle")
            continue
        if dll_name.lower() == "libc++.dll":
            src = os.path.join(FFMPEG_DEPENDENCY_PREFIX, "bin", dll_name)
        else:
            src = os.path.join(clang_bin, dll_name)
        if not os.path.exists(src):
            raise RuntimeError(f"Missing sanitizer runtime DLL: {src}")
        dst = os.path.join(target_dir, dll_name)
        if not safe_copy_file(src, dst):
            raise RuntimeError(f"Failed to copy sanitizer runtime DLL {dll_name} to {target_dir}")
        log(f"Copied sanitizer runtime DLL {dll_name}")


def remove_stale_windows_sanitizer_runtime_dlls(target_dir: str) -> None:
    if IS_LINUX:
        return

    for dll_name in WINDOWS_SANITIZER_RUNTIME_DEPS:
        dll_path = os.path.join(target_dir, dll_name)
        if not os.path.exists(dll_path):
            continue
        if safe_delete_file(dll_path):
            log(f"Removed stale sanitizer runtime DLL {dll_name}")
        else:
            log(f"WARNING: Could not remove stale sanitizer runtime DLL {dll_name}")


def get_msys_license_root():
    if IS_LINUX:
        return os.path.join(get_linux_msys2_dir(), "clang64", "share", "licenses")
    return os.path.join(FFMPEG_DEPENDENCY_PREFIX, "share", "licenses")


def get_amf_headers_license_path():
    return os.path.join(
        get_host_msys2_dir(),
        "clang64",
        "share",
        "licenses",
        "amf-headers",
        "LICENSE",
    )


def copy_bundled_runtime_licenses(licenses_dst, ffmpeg_bin_dst):
    if not os.path.isdir(licenses_dst) or not os.path.isdir(ffmpeg_bin_dst):
        return

    license_root = get_msys_license_root()
    build_time_license_specs = [
        (get_amf_headers_license_path(), "MIT_AMF-Headers.txt"),
    ]
    license_specs = [
        (
            "libiconv-2.dll",
            [
                (
                    os.path.join(license_root, "libiconv", "COPYING.LIB"),
                    "LGPLv2.1_libiconv.txt",
                ),
            ],
        ),
        (
            "libcharset-1.dll",
            [
                (
                    os.path.join(license_root, "libiconv", "libcharset", "COPYING.LIB"),
                    "LGPLv2.1_libcharset.txt",
                ),
            ],
        ),
        (
            "libbz2-1.dll",
            [
                (
                    os.path.join(license_root, "bzip2", "LICENSE"),
                    "BSL-1.0_bzip2.txt",
                ),
            ],
        ),
        (
            "libgme.dll",
            [
                (
                    os.path.join(license_root, "libgme", "license.txt"),
                    "Expat_libgme.txt",
                ),
            ],
        ),
        (
            "libmodplug-1.dll",
            [
                (
                    os.path.join(license_root, "libmodplug", "LICENSE"),
                    "LGPLv2.1_libmodplug.txt",
                ),
            ],
        ),
        (
            "libvpl-2.dll",
            [
                (
                    os.path.join(license_root, "libvpl", "LICENSE"),
                    "MIT_libvpl.txt",
                ),
            ],
        ),
        (
            "libopus-0.dll",
            [
                (
                    os.path.join(license_root, "opus", "COPYING"),
                    "BSD-3-Clause_libopus.txt",
                ),
            ],
        ),
        (
            "libc++.dll",
            [
                (
                    os.path.join(license_root, "libc++", "LICENSE"),
                    "Apache-2.0_with_LLVM-exception_llvm-runtime.txt",
                ),
            ],
        ),
        (
            "libunwind.dll",
            [
                (
                    os.path.join(license_root, "libunwind", "LICENSE"),
                    "Apache-2.0_with_LLVM-exception_llvm-runtime.txt",
                ),
            ],
        ),
        (
            "libva.dll",
            [
                (
                    os.path.join(license_root, "libva", "COPYING"),
                    "MIT_libva.txt",
                ),
            ],
        ),
        (
            "libva_win32.dll",
            [
                (
                    os.path.join(license_root, "libva", "COPYING"),
                    "MIT_libva.txt",
                ),
            ],
        ),
        (
            "libSvtAv1Enc-4.dll",
            [
                (
                    os.path.join(license_root, "svt-av1", "LICENSE"),
                    "BSD-3-Clause-Clear_svt-av1.txt",
                ),
                (
                    os.path.join(license_root, "svt-av1", "PATENTS.md"),
                    "AOM-Patent-License-1.0_svt-av1.txt",
                ),
            ],
        ),
        (
            "libaom.dll",
            [
                (
                    os.path.join(license_root, "aom", "LICENSE"),
                    "BSD-2-Clause_libaom.txt",
                ),
                (
                    os.path.join(license_root, "aom", "PATENTS"),
                    "AOM-Patent-License-1.0_libaom.txt",
                ),
            ],
        ),
        (
            "libwinpthread-1.dll",
            [
                (
                    os.path.join(license_root, "libwinpthread", "COPYING"),
                    "Mingw-w64-runtime_libwinpthread.txt",
                ),
            ],
        ),
        (
            "libgcc_s_seh-1.dll",
            [
                (
                    os.path.join(license_root, "crt", "COPYING.MinGW-w64-runtime.txt"),
                    "Mingw-w64-runtime_libgcc.txt",
                ),
            ],
        ),
        (
            "libstdc++-6.dll",
            [
                (
                    os.path.join(license_root, "crt", "COPYING.MinGW-w64-runtime.txt"),
                    "Mingw-w64-runtime_libstdcxx.txt",
                ),
            ],
        ),
    ]

    bundled_dlls = {entry.lower() for entry in os.listdir(ffmpeg_bin_dst) if entry.lower().endswith(".dll")}
    copied_license_names = set()
    mapped_runtime_dlls = set()

    # AMF is compiled from SDK headers but loads the driver-provided runtime at
    # run time, so no bundled DLL exists that could trigger this notice.
    for src, dst_name in build_time_license_specs:
        if not os.path.exists(src):
            raise RuntimeError(f"Missing bundled build-time license source: {src}")
        dst = os.path.join(licenses_dst, dst_name)
        if not safe_copy_file(src, dst):
            raise RuntimeError(f"Failed to copy bundled build-time license {dst_name}")
        copied_license_names.add(dst_name.lower())
        log(f"Copied bundled build-time license {dst_name}")

    for dll_name, outputs in license_specs:
        dll_name_lower = dll_name.lower()
        if dll_name_lower not in bundled_dlls:
            continue
        mapped_runtime_dlls.add(dll_name_lower)
        for src, dst_name in outputs:
            if not os.path.exists(src):
                raise RuntimeError(f"Missing bundled runtime license source: {src}")
            dst_name_lower = dst_name.lower()
            if dst_name_lower in copied_license_names:
                continue
            dst = os.path.join(licenses_dst, dst_name)
            if not safe_copy_file(src, dst):
                raise RuntimeError(f"Failed to copy bundled runtime license {dst_name}")
            copied_license_names.add(dst_name_lower)
            log(f"Copied bundled runtime license {dst_name}")

    known_ffmpeg_prefixes = (
        "avcodec-",
        "avdevice-",
        "avfilter-",
        "avformat-",
        "avutil-",
        "swresample-",
        "swscale-",
    )
    for dll_name in sorted(bundled_dlls):
        if dll_name.startswith(known_ffmpeg_prefixes):
            continue
        if dll_name in mapped_runtime_dlls:
            continue
        log(f"WARNING: Bundled runtime DLL {dll_name} has no configured license copy rule")


class FFmpegBuilder:
    def __init__(self, root_dir, msys_dir, install_dir, license_mode="gpl", dependency_prefix_path=None):
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
        self.dependency_prefix = dependency_prefix_path or dependency_prefix(root_dir)
        self.dependency_unix_prefix = to_unix(self.dependency_prefix)
        self.license_mode = "lgpl"  # Changed to LGPL per user request

    def _vulkan_import_lib(self, arch: str) -> Optional[str]:
        return get_linux_vulkan_import_lib_path(arch)

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
        msys_pkgconfig = os.path.join(self.msys, "clang64", "lib", "pkgconfig")
        dependency_inc = to_unix(os.path.join(self.dependency_prefix, "include"))
        dependency_lib = to_unix(os.path.join(self.dependency_prefix, "lib"))
        dependency_pkgconfig = os.path.join(self.dependency_prefix, "lib", "pkgconfig")

        env["CC"] = "clang"
        env["CXX"] = "clang++"
        env["CFLAGS"] = (
            f"-O3 -mguard=cf -fstack-protector-strong -D_FORTIFY_SOURCE=2 "
            f"-ffunction-sections -fdata-sections -I{dependency_inc} "
            f"-I{self.prefix}/include -I{msys_inc}"
        )
        env["CXXFLAGS"] = (
            f"-O3 -mguard=cf -fstack-protector-strong -D_FORTIFY_SOURCE=2 "
            f"-ffunction-sections -fdata-sections -I{dependency_inc} "
            f"-I{self.prefix}/include -I{msys_inc}"
        )
        env["LDFLAGS"] = f"-Wl,--gc-sections -Wl,--guard-cf " f"-L{dependency_lib} -L{self.prefix}/lib -L{msys_lib}"
        env["PKG_CONFIG"] = f"{pkg_config} --static"
        # pkg-config here is a native Windows binary, so it expects Windows-style paths.
        # Using /c/... MSYS paths makes the NVCodec probe fail to locate ffnvcodec.pc.
        env["PKG_CONFIG_PATH"] = os.pathsep.join(
            [dependency_pkgconfig, os.path.join(self.win_prefix, "lib", "pkgconfig"), msys_pkgconfig]
        )
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
        if isinstance(cmd, list):
            cmd_list = cmd
            cmd_str = " ".join(cmd_list)
        else:
            cmd_list = shlex.split(cmd, posix=False)
            cmd_str = cmd
        log(f"[FFmpeg] EXEC: {cmd_str}", detail=True)
        try:
            if env is None:
                env = os.environ.copy()
            if env and "PATH" not in env:
                env["PATH"] = os.environ["PATH"]

            run_logged_subprocess(cmd_list, cwd=cwd, env=env, check=check, shell=False)
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
            [make_exe, f"PREFIX={self.prefix}", "install"],
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

        # Apply custom patches (LGPL 2.1) from patches/ffmpeg/
        patches_dir = os.path.join(PROJECT_ROOT, "patches", "ffmpeg")
        if os.path.isdir(patches_dir):
            git_exe = self.get_tool_path("git")
            patch_files = sorted(f for f in os.listdir(patches_dir) if f.endswith(".patch"))
            patch_paths = [os.path.join(patches_dir, patch_file) for patch_file in patch_files]
            normalized_targets = normalize_custom_patch_targets(build_dir, patch_paths)
            if normalized_targets:
                log(
                    f"[FFmpeg] Normalized LF line endings for {len(normalized_targets)} custom patch "
                    f"target(s): {', '.join(normalized_targets)}"
                )
            for pf in patch_files:
                patch_path = os.path.join(patches_dir, pf)
                log(f"[FFmpeg] Applying patch: {pf}")
                self.run(
                    [git_exe, "apply", "--verbose", patch_path],
                    cwd=build_dir,
                    env=self.get_msys_env(),
                )
            if patch_files:
                log(f"[FFmpeg] Applied {len(patch_files)} patch(es)")

        env = self.get_msys_env()
        make_exe = self.get_tool_path("make")
        bash_exe = self.get_tool_path("bash")

        # Define msys_lib for extra-ldflags
        msys_lib = to_unix(os.path.join(self.msys, "clang64", "lib"))
        dependency_lib = to_unix(os.path.join(self.dependency_prefix, "lib"))

        conf = [
            bash_exe,
            "./configure",
            f"--prefix={self.prefix}",
            "--target-os=mingw32",
            "--enable-shared",
            "--disable-static",  # SHARED BUILD
            # '--pkg-config-flags="--static"',
            "--arch=x86_64",
            # Linking fixes
            # We explicitly link dependent C++ libraries to ensure they are available to avcodec.dll
            # libvpl (for QSV) often needs -lvpl -lstdc++ and system libs
            "--extra-libs=-lc++ -lvpl -lstdc++ -lole32 -lgdi32 -luuid",
            # Toolchain
            "--extra-libs=-lc++",
            # Toolchain - Use MSYS2 Clang
            "--cc=clang",
            "--cxx=clang++",
            "--ar=llvm-ar",
            "--nm=llvm-nm",
            "--ranlib=llvm-ranlib",
            # Optimization
            # AAC NMR and multiple FFmpeg DSP/psychoacoustic paths use NaN/Inf
            # sentinels. -ffast-math makes those undefined and can silently
            # invalidate the encoder's quality decisions.
            "--extra-cflags=-O3 -flto -mguard=cf",
            "--extra-cxxflags=-O3 -flto -mguard=cf",
            "--extra-ldflags=-flto -O3 -Wl,--guard-cf",
            f"--extra-ldflags=-L{dependency_lib} -L{msys_lib}",
            # Keep the FFmpeg build redistributable under LGPLv2.1+.
            # The extra components used here (FFNVCodec headers, AMF headers,
            # oneVPL/libvpl, MediaFoundation, Windows HW accel APIs) do not
            # require enabling GPL, version3, or nonfree mode.
            "--disable-gpl",
            # Components
            "--disable-doc",
            "--disable-programs",
            "--enable-ffmpeg",
            "--enable-ffprobe",
            "--disable-avdevice",
            "--disable-avfilter",
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
            "--enable-libaom",  # AOM AV1 encoder (10-bit 4:4:4 AVIF screenshots)
            "--enable-libsvtav1",  # SVT-AV1 encoder (fast AV1, BSD license)
            "--enable-libopus",  # Opus audio encoder with correct packetization support
            # Tuning
            "--disable-encoders",
            "--disable-decoders",
            "--disable-muxers",
            "--disable-demuxers",
            "--disable-parsers",
            "--disable-bsfs",
            "--disable-protocols",
            "--enable-protocol=file",
            "--enable-muxer=mp4,matroska,mov,flv,ts,avif",
            "--enable-demuxer=concat,matroska,mov,mp4",
            # SW Encoders (Audio)
            "--enable-encoder=aac,libopus,flac,alac,pcm_s16le,pcm_s24le,pcm_f32le",
            "--enable-decoder=aac,opus,flac,alac,pcm_s16le,pcm_s24le,pcm_f32le",
            "--enable-parser=aac,opus,flac",
            # HW Encoders
            "--enable-encoder=h264_nvenc,hevc_nvenc,av1_nvenc",
            "--enable-encoder=h264_amf,hevc_amf,av1_amf",
            "--enable-encoder=h264_qsv,hevc_qsv,av1_qsv,vp9_qsv",
            "--enable-encoder=h264_mf,hevc_mf",  # MediaFoundation
            # FFmpeg configure component names use underscores even though the
            # runtime encoder name exposed by libavcodec is "libaom-av1".
            "--enable-encoder=libaom_av1",  # AOM AV1 (HDR 4:4:4 still images)
            "--enable-encoder=libsvtav1",  # SVT-AV1 (for AVIF screenshots)
            # HW Decoders
            "--enable-decoder=h264,hevc,av1,vp9,mjpeg",
            "--enable-decoder=libaom_av1",  # Deterministic 10-bit 4:4:4 AVIF verification/decoding
            "--enable-decoder=h264_qsv,hevc_qsv,av1_qsv,vp9_qsv",
            "--enable-decoder=h264_cuvid,hevc_cuvid,vp9_cuvid,av1_cuvid",
            "--enable-hwaccel=h264_nvdec,hevc_nvdec,av1_nvdec",
            "--enable-hwaccel=h264_d3d11va,hevc_d3d11va,av1_d3d11va",
        ]

        self.run(conf, cwd=build_dir, env=env)
        self.run([make_exe, f"-j{cpu_count()}"], cwd=build_dir, env=env)
        self.run([make_exe, "install"], cwd=build_dir, env=env)


FFMPEG_BUILD_CONFIGURATION_VERSION = 8


def ffmpeg_build_configuration_fingerprint():
    """Track local configure/patch inputs independently of the upstream commit."""
    digest = hashlib.sha256(f"configure-v{FFMPEG_BUILD_CONFIGURATION_VERSION}\n".encode("ascii"))
    digest.update(b"dependency-manifest\n")
    digest.update(dependency_manifest_fingerprint(FFMPEG_DEPENDENCY_MANIFEST).encode("ascii"))
    patches_dir = os.path.join(PROJECT_ROOT, "patches", "ffmpeg")
    if os.path.isdir(patches_dir):
        for patch_name in sorted(name for name in os.listdir(patches_dir) if name.endswith(".patch")):
            digest.update(patch_name.encode("utf-8"))
            with open(os.path.join(patches_dir, patch_name), "rb") as patch_file:
                digest.update(patch_file.read())
    return digest.hexdigest()


def compile_custom_ffmpeg(skip_updates=False):
    """Build FFmpeg and its source dependency closure.

    Args:
        skip_updates: If True, reuse verified source-built outputs when they are current.
            Plain builds intentionally rebuild the complete source closure.
    """
    if IS_LINUX:
        log("Running on Linux/WSL - using MSYS2 FFmpeg (downloaded from repo)")
        return  # FFmpeg is downloaded as part of MSYS2 packages

    def dependency_log(message: str) -> None:
        log(message, detail="] EXEC:" in message)

    dependency_builder = SourceDependencyBuilder(
        project_root=PROJECT_ROOT,
        msys2_dir=MSYS2_DIR,
        manifest_path=FFMPEG_DEPENDENCY_MANIFEST,
        logger=dependency_log,
        runner=run_logged_subprocess,
    )
    try:
        full_source_rebuild = not skip_updates
        dependency_builder.ensure(force_rebuild=full_source_rebuild)
    except Exception as e:
        log(f"FFmpeg source dependency build failed: {e}")
        sys.exit(1)

    # Use internal builder after the complete source-built dependency closure
    # is available. The private prefix is intentionally first for pkg-config,
    # headers, and linker resolution.
    builder = FFmpegBuilder(
        root_dir=PROJECT_ROOT,
        msys_dir=MSYS2_DIR,
        install_dir=FFMPEG_DIR,
        dependency_prefix_path=dependency_builder.prefix,
    )
    builder.setup_dirs()

    # Check if FFmpeg repo exists and get current commit for tracking
    ffmpeg_repo = os.path.join(builder.repos_dir, "ffmpeg")
    commit_file = os.path.join(builder.build_root, "last_built_commit.txt")
    configuration_file = os.path.join(builder.build_root, "last_build_configuration.txt")
    current_configuration = ffmpeg_build_configuration_fingerprint()
    previous_configuration = ""
    if os.path.exists(configuration_file):
        with open(configuration_file, "r", encoding="utf-8") as f:
            previous_configuration = f.read().strip()

    # Plain builds are the explicit fresh-source path. --skip-updates retains the
    # verified-cache path and only rebuilds when required for correctness.
    needs_rebuild = full_source_rebuild

    if not os.path.exists(os.path.join(FFMPEG_DIR, "lib", "libavcodec.dll.a")):
        # No built FFmpeg - definitely need to build
        needs_rebuild = True
        log("FFmpeg not built yet - building...")
    elif previous_configuration != current_configuration:
        needs_rebuild = True
        log(
            "FFmpeg local build configuration changed "
            f"({previous_configuration[:8] if previous_configuration else 'unstamped'}"
            f" -> {current_configuration[:8]}) - rebuilding without changing the pinned source"
        )
    elif skip_updates and os.path.isdir(os.path.join(BIN_DIR, "ffmpeg")):
        # Prebuilt DLLs present and updates skipped - skip entirely
        log("FFmpeg DLLs present, --skip-updates: skipping FFmpeg build.")
        try:
            log("Copying FFmpeg DLLs...")
            ffmpeg_bin_src = os.path.join(FFMPEG_DIR, "bin")
            ffmpeg_bin_dst = os.path.join(BIN_DIR, "ffmpeg")
            runtime_deps = get_windows_ffmpeg_runtime_deps(dependency_builder.bin_dir)
            sync_ffmpeg_runtime_dlls(
                ffmpeg_bin_src,
                ffmpeg_bin_dst,
                runtime_deps,
                [dependency_builder.bin_dir],
                required_runtime_deps=True,
                private_runtime_root=dependency_builder.prefix,
            )
            log("Custom FFmpeg Setup Complete.")
        except Exception as e:
            log(f"FFmpeg Setup Failed: {e}")
            sys.exit(1)
        return
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
        elif full_source_rebuild:
            log("Plain python build.py requested a fresh FFmpeg source rebuild")
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
            with open(configuration_file, "w", encoding="utf-8") as f:
                f.write(current_configuration)
            log(f"Built FFmpeg commit: {current_commit[:8]}")
        except Exception as e:
            log(f"FFmpeg Build Failed: {e}")
            sys.exit(1)

    # Post-build (or if already built): Copy DLLs to bin/ffmpeg
    try:
        log("Copying FFmpeg DLLs...")
        ffmpeg_bin_src = os.path.join(FFMPEG_DIR, "bin")
        ffmpeg_bin_dst = os.path.join(BIN_DIR, "ffmpeg")
        runtime_deps = get_windows_ffmpeg_runtime_deps(dependency_builder.bin_dir)
        sync_ffmpeg_runtime_dlls(
            ffmpeg_bin_src,
            ffmpeg_bin_dst,
            runtime_deps,
            [dependency_builder.bin_dir],
            required_runtime_deps=True,
            private_runtime_root=dependency_builder.prefix,
        )

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
        apply_workspace_temp_environment(env)
        env["PATH"] = compilers["x64"]["bin"] + os.pathsep + env.get("PATH", "")
        env["CC"] = compilers["x64"]["cc"]
        env["CXX"] = compilers["x64"]["cxx"]
        env["PKG_CONFIG_PATH"] = ""
        env["DISABLE_CCACHE"] = "1"
        return env, compilers["x64"]["bin"]

    clang_bin = os.path.join(MSYS2_DIR, "clang64", "bin")
    usr_bin = os.path.join(MSYS2_DIR, "usr", "bin")
    env = os.environ.copy()
    apply_workspace_temp_environment(env)
    env["PATH"] = clang_bin + os.pathsep + usr_bin + os.pathsep + env.get("PATH", "")
    private_lib = os.path.join(FFMPEG_DEPENDENCY_PREFIX, "lib")
    env["PKG_CONFIG_PATH"] = os.pathsep.join(
        [
            os.path.join(private_lib, "pkgconfig"),
            os.path.join(MSYS2_DIR, "clang64", "lib", "pkgconfig"),
        ]
    )
    env.pop("CPLUS_INCLUDE_PATH", None)
    env["LIBRARY_PATH"] = os.pathsep.join([private_lib, os.path.join(MSYS2_DIR, "clang64", "lib")])
    env["CCACHE_DIR"] = os.path.join(MSYS2_DIR, ".ccache")
    env["DISABLE_CCACHE"] = "1"
    return env, clang_bin


def get_env_x86():
    if IS_LINUX:
        compilers = get_mingw_compilers()
        if compilers is None:
            log("ERROR: MinGW compiler discovery failed on Linux host")
            sys.exit(1)
        if not compilers["x86"]["cxx"]:
            raise RuntimeError("No mingw-w64 x86 compiler found on Linux host")
        env = os.environ.copy()
        apply_workspace_temp_environment(env)
        x86_bin = compilers["x86"]["bin"]
        env["PATH"] = x86_bin + os.pathsep + env.get("PATH", "")
        env["CC"] = compilers["x86"]["cc"]
        env["CXX"] = compilers["x86"]["cxx"]
        env["PKG_CONFIG_PATH"] = ""
        env["DISABLE_CCACHE"] = "1"
        return env, x86_bin

    # Cross-compile x86 from clang64: both clang64/bin (compiler) and
    # mingw32/bin (windres, pkg-config, runtime libs) must be on PATH.
    clang64_bin = os.path.join(MSYS2_DIR, "clang64", "bin")
    mingw32_bin = os.path.join(MSYS2_DIR, "mingw32", "bin")
    usr_bin = os.path.join(MSYS2_DIR, "usr", "bin")
    env = os.environ.copy()
    apply_workspace_temp_environment(env)
    env["PATH"] = clang64_bin + os.pathsep + mingw32_bin + os.pathsep + usr_bin + os.pathsep + env.get("PATH", "")
    env["PKG_CONFIG_PATH"] = os.path.join(MSYS2_DIR, "mingw32", "lib", "pkgconfig")
    env["CCACHE_DIR"] = os.path.join(MSYS2_DIR, ".ccache")
    env["DISABLE_CCACHE"] = "1"
    return env, mingw32_bin


def propagate_build_control_environment(source_env: Dict[str, str], target_env: Dict[str, str]) -> None:
    """Copy build-mode controls into independently constructed architecture environments."""
    for flag in (
        "FORCE_REBUILD",
        "CE_BUILD_JOBS",
        "CE_PRODUCTION_BUILD",
        "CE_SANITIZE",
        "CE_DISABLE_LTO",
        "DISABLE_CCACHE",
    ):
        if flag in source_env:
            target_env[flag] = source_env[flag]


def get_parallel_job_count(env: Dict[str, str], task_count: int) -> int:
    requested_workers = env.get("CE_BUILD_JOBS", "").strip()
    if requested_workers:
        try:
            workers = int(requested_workers)
        except ValueError:
            log(f"Warning: invalid CE_BUILD_JOBS={requested_workers!r}; using auto worker count")
            workers = cpu_count()
    else:
        workers = cpu_count()
    return max(1, min(workers, task_count or 1))


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


@lru_cache(maxsize=None)
def compute_file_content_hash(path: str) -> str:
    with open(path, "rb") as f:
        return hashlib.md5(f.read()).hexdigest()[:16]


@lru_cache(maxsize=None)
def compute_compiler_fingerprint(clang_exe: str) -> str:
    resolved_compiler = clang_exe if os.path.isfile(clang_exe) else shutil.which(clang_exe)
    if not resolved_compiler or not os.path.isfile(resolved_compiler):
        raise FileNotFoundError(f"Unable to fingerprint compiler: {clang_exe}")
    absolute_compiler = os.path.abspath(resolved_compiler)
    return f"{absolute_compiler}:{sha256_file(absolute_compiler)}"


def compute_build_signature(
    src: str, clang_exe: str, compile_flags: List[str], dependencies: Optional[List[str]] = None
) -> str:
    """Create a stable signature from source, project headers, compiler, and flags."""
    with open(src, "rb") as f:
        src_hash = hashlib.md5(f.read()).hexdigest()[:16]
    tool_fingerprint = "\n".join([compute_compiler_fingerprint(clang_exe)] + compile_flags)
    tool_hash = hashlib.md5(tool_fingerprint.encode("utf-8")).hexdigest()[:16]

    dependency_hash = hashlib.md5()
    project_root = os.path.normcase(os.path.abspath(PROJECT_ROOT))
    for dependency in sorted(set(dependencies or [])):
        absolute_dependency = os.path.normcase(os.path.abspath(dependency))
        try:
            if os.path.commonpath([project_root, absolute_dependency]) != project_root:
                continue
        except ValueError:
            continue
        if not os.path.isfile(absolute_dependency):
            continue
        dependency_hash.update(absolute_dependency.encode("utf-8", errors="surrogatepass"))
        dependency_hash.update(compute_file_content_hash(absolute_dependency).encode("ascii"))
    return f"{src_hash}:{tool_hash}:{dependency_hash.hexdigest()[:16]}"


LINK_CACHE_SCHEMA_VERSION = 1


@lru_cache(maxsize=None)
def compute_link_input_fingerprint(path: str, size: int, mtime_ns: int) -> str:
    del size, mtime_ns
    return sha256_file(path)


def fingerprint_link_input(path: str) -> str:
    stat_result = os.stat(path)
    return compute_link_input_fingerprint(os.path.abspath(path), stat_result.st_size, stat_result.st_mtime_ns)


@lru_cache(maxsize=8)
def get_link_resource_dir(clang_exe: str) -> Optional[str]:
    return detect_clang_resource_dir(os.environ.copy(), clang_exe)


def get_link_search_directories(command: List[str], clang_exe: str) -> List[str]:
    directories: List[str] = []

    def add_directory(path: str) -> None:
        normalized = os.path.abspath(path)
        if os.path.isdir(normalized) and normalized not in directories:
            directories.append(normalized)

    index = 1
    while index < len(command):
        argument = command[index]
        if argument == "-L" and index + 1 < len(command):
            add_directory(command[index + 1])
            index += 2
            continue
        if argument.startswith("-L") and len(argument) > 2:
            add_directory(argument[2:])
        if argument.startswith("--sysroot="):
            sysroot = argument.split("=", 1)[1]
            add_directory(os.path.join(sysroot, "lib"))
        index += 1

    compiler_dir = os.path.dirname(os.path.abspath(clang_exe))
    add_directory(os.path.join(compiler_dir, "..", "lib"))
    if IS_WINDOWS:
        add_directory(os.path.join(MSYS2_DIR, "clang64", "lib"))
        if is_x86_compile_command(command):
            add_directory(os.path.join(MSYS2_DIR, "mingw32", "lib"))
    resource_dir = get_link_resource_dir(clang_exe)
    if resource_dir:
        add_directory(os.path.join(resource_dir, "lib", "windows"))
    return directories


def collect_link_dependency_paths(command: List[str], clang_exe: str, cwd: Optional[str] = None) -> List[str]:
    base_dir = os.path.abspath(cwd or PROJECT_ROOT)
    dependencies: set[str] = set()

    skip_path = False
    for argument in command[1:]:
        if skip_path:
            skip_path = False
            continue
        if argument == "-o":
            skip_path = True
            continue
        if argument.startswith("-"):
            continue
        candidate = argument if os.path.isabs(argument) else os.path.join(base_dir, argument)
        if os.path.isfile(candidate):
            dependencies.add(os.path.abspath(candidate))

    search_directories = get_link_search_directories(command, clang_exe)
    library_names = [argument[2:] for argument in command if argument.startswith("-l") and len(argument) > 2]
    implicit_runtime_names = [
        "c++",
        "c++abi",
        "unwind",
        "stdc++",
        "gcc",
        "gcc_eh",
        "winpthread",
        "pthread",
        "mingw32",
        "mingwex",
        "msvcrt",
        "ucrt",
    ]
    for library_name in library_names + implicit_runtime_names:
        for directory in search_directories:
            for filename in (
                f"lib{library_name}.a",
                f"lib{library_name}.dll.a",
                f"{library_name}.lib",
            ):
                candidate = os.path.join(directory, filename)
                if os.path.isfile(candidate):
                    dependencies.add(os.path.abspath(candidate))

    compiler_dir = os.path.dirname(os.path.abspath(clang_exe))
    for linker_name in ("ld.lld.exe", "lld-link.exe", "ld.exe") if IS_WINDOWS else ("ld.lld", "ld"):
        candidate = os.path.join(compiler_dir, linker_name)
        if os.path.isfile(candidate):
            dependencies.add(os.path.abspath(candidate))
    return sorted(dependencies, key=os.path.normcase)


def compute_link_signature(
    command: List[str], env: Dict[str, str], cwd: Optional[str] = None
) -> tuple[str, List[str]]:
    if not command:
        raise ValueError("Cannot fingerprint an empty link command")
    clang_exe = command[0]
    digest = hashlib.sha256()
    digest.update(f"schema={LINK_CACHE_SCHEMA_VERSION}\n".encode("ascii"))
    digest.update(compute_compiler_fingerprint(clang_exe).encode("utf-8", errors="surrogatepass"))
    digest.update(b"\0")
    for argument in command[1:]:
        digest.update(argument.encode("utf-8", errors="surrogatepass"))
        digest.update(b"\0")
    for variable in ("PATH", "LIB", "LIBRARY_PATH"):
        digest.update(variable.encode("ascii") + b"=")
        digest.update(env.get(variable, "").encode("utf-8", errors="surrogatepass"))
        digest.update(b"\0")

    dependencies = collect_link_dependency_paths(command, clang_exe, cwd)
    for dependency in dependencies:
        digest.update(os.path.normcase(dependency).encode("utf-8", errors="surrogatepass"))
        digest.update(b"\0")
        digest.update(fingerprint_link_input(dependency).encode("ascii"))
        digest.update(b"\0")
    return digest.hexdigest(), dependencies


def link_cache_manifest_path(output_path: str) -> str:
    return output_path + ".link-cache.json"


def load_link_cache_manifest(output_path: str) -> Optional[Dict[str, Any]]:
    try:
        with open(link_cache_manifest_path(output_path), "r", encoding="utf-8") as manifest_file:
            manifest = json.load(manifest_file)
        return manifest if isinstance(manifest, dict) else None
    except (OSError, json.JSONDecodeError):
        return None


def link_cache_manifest_matches(manifest: Optional[Dict[str, Any]], input_signature: str) -> bool:
    try:
        if not manifest or manifest.get("schema") != LINK_CACHE_SCHEMA_VERSION:
            return False
        if manifest.get("input_signature") != input_signature:
            return False
        for required_output, expected_hash in manifest["outputs"].items():
            if not os.path.isfile(required_output) or sha256_file(required_output) != expected_hash:
                return False
        return True
    except (OSError, KeyError, TypeError):
        return False


def validate_cached_link_output(output_path: str, env: Dict[str, str]) -> bool:
    try:
        manifest = load_link_cache_manifest(output_path)
        if not manifest:
            return False
        command = manifest["command"]
        cwd = manifest.get("cwd")
        current_signature, _ = compute_link_signature(command, env, cwd)
        return link_cache_manifest_matches(manifest, current_signature)
    except (OSError, ValueError, KeyError, TypeError):
        return False


def run_cached_link(
    command: List[str],
    env: Dict[str, str],
    output_path: str,
    *,
    required_outputs: Optional[List[str]] = None,
    cwd: Optional[str] = None,
) -> bool:
    required = [os.path.abspath(path) for path in (required_outputs or [output_path])]
    output_path = os.path.abspath(output_path)
    manifest_path = link_cache_manifest_path(output_path)
    input_signature, dependencies = compute_link_signature(command, env, cwd)
    if env.get("FORCE_REBUILD") != "1" and link_cache_manifest_matches(
        load_link_cache_manifest(output_path), input_signature
    ):
        log(f"Link cache hit: {os.path.relpath(output_path, PROJECT_ROOT)}", detail=True)
        return False

    try:
        os.remove(manifest_path)
    except FileNotFoundError:
        pass
    run_command(command, env=env, cwd=cwd)
    missing_outputs = [path for path in required if not os.path.isfile(path)]
    if missing_outputs:
        raise RuntimeError("Link did not produce required output(s): " + ", ".join(missing_outputs))
    manifest = {
        "schema": LINK_CACHE_SCHEMA_VERSION,
        "command": command,
        "cwd": os.path.abspath(cwd) if cwd else None,
        "input_signature": input_signature,
        "input_count": len(dependencies),
        "outputs": {path: sha256_file(path) for path in required},
    }
    write_json_atomic(manifest_path, manifest)
    return True


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

    if not os.path.exists(dep_file):
        # If object exists but dep file is missing, recompile to generate dep file.
        return True
    deps = parse_dep_file(dep_file)
    if not deps:
        # Invalid/empty dep file can miss header changes and cause ABI skew.
        return True

    # Also check if source, project-header content, or compile settings changed.
    # Header content hashing prevents ABI-skewed objects even when a checkout or
    # restore gives a changed header an older timestamp.
    hash_file = obj + ".hash"
    try:
        signature = compute_build_signature(src, clang_exe, compile_flags, deps)
        if os.path.exists(hash_file):
            with open(hash_file, "r") as f:
                stored_hash = f.read().strip()
            if signature != stored_hash:
                return True  # Content and/or flags changed, recompile
        else:
            # No hash file, need to create one (first compile or old build)
            return True
    except Exception:
        # Incremental reuse is a correctness feature, not a best-effort optimization. If its
        # content signature cannot be proven, rebuild the object instead of trusting timestamps.
        return True

    # Check dependency timestamps as a cheap second signal, including toolchain
    # headers outside the project root that are intentionally not content-hashed.
    obj_mtime = os.path.getmtime(obj)
    for dep in deps:
        try:
            if os.path.exists(dep) and os.path.getmtime(dep) > obj_mtime:
                return True
        except OSError:
            return True  # Error accessing dependency, safer to recompile

    return False


def normalize_compile_command_arg(arg: str) -> str:
    """Normalize compile_commands argument paths for clangd compatibility."""
    if IS_WINDOWS:
        return arg.replace("\\", "/")
    return arg


def _find_first_existing_path(candidates: List[str]) -> Optional[str]:
    for candidate in candidates:
        if candidate and os.path.isdir(candidate):
            return candidate.replace("\\", "/")
    return None


def _append_unique_flag(arguments: List[str], flag: str) -> None:
    if flag and flag not in arguments:
        insert_at = len(arguments)
        if "-c" in arguments:
            insert_at = arguments.index("-c")
        arguments.insert(insert_at, flag)


def _has_flag_with_prefix(arguments: List[str], prefix: str) -> bool:
    return any(arg == prefix or arg.startswith(prefix) for arg in arguments)


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
        seen_files = set()
        unique_commands = []
        sorted_commands = sorted(COMPILE_COMMANDS, key=lambda x: x["file"])
        for cmd in sorted_commands:
            enriched_cmd = enrich_compile_command_for_clangd(dict(cmd))
            is_x86 = is_x86_compile_command(enriched_cmd["arguments"])
            if enriched_cmd["file"] not in seen_files:
                unique_commands.append(enriched_cmd)
                seen_files.add(enriched_cmd["file"])
            elif not is_x86:
                for i, existing in enumerate(unique_commands):
                    if existing["file"] == enriched_cmd["file"]:
                        unique_commands[i] = enriched_cmd
                        break
        compile_commands_path = os.path.join(PROJECT_ROOT, "compile_commands.json")
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

    # Verify the compiled object file on Linux cross-compilation.
    if IS_LINUX and os.path.exists(obj):
        _verify_cross_object(obj, clang_exe, src)

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

    tests_dir = os.path.join(PROJECT_ROOT, "tests")
    os.makedirs(tests_dir, exist_ok=True)
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

    screenshot_encoding_src = os.path.join(PROJECT_ROOT, "captureengine", "screenshot_encoding.cpp")
    screenshot_encoding_obj = os.path.join(obj_dir, "captureengine", "screenshot_encoding.test.o").replace("\\", "/")
    compile_tasks.append((test_cflags + strict_fp_flags, screenshot_encoding_src, screenshot_encoding_obj))

    pseudo_overlay_src = os.path.join(PROJECT_ROOT, "captureengine", "pseudo_overlay.cpp")
    pseudo_overlay_obj = os.path.join(obj_dir, "captureengine", "pseudo_overlay.test.o").replace("\\", "/")
    compile_tasks.append((test_cflags, pseudo_overlay_src, pseudo_overlay_obj))

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
        [get_windres_exe("x64"), os.path.join(tests_dir, "config_template.rc"), "-o", config_resource_obj],
        env=env,
        cwd=tests_dir,
    )

    log("Linking Unit Tests...")
    # Order: Tests -> Common -> MediaEngine -> HookCommon -> HookWrappers -> Libs
    cmd = (
        [clang_exe]
        + test_objs
        + common_objs
        + me_objs
        + [screenshot_encoding_obj, pseudo_overlay_obj]
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
    ffmpeg_bin = os.path.join(PROJECT_ROOT, "installed", "captureengine", "ffmpeg")
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


def run_tests(env, test_exe, gtest_filter=None, run_python_tools=True):
    log("=== Running Unit Tests ===")
    if not os.path.exists(test_exe):
        log("Error: Test executable not found.")
        return False

    # Ensure required DLLs are on PATH (libgtest.dll, FFmpeg DLLs)
    msys_bin = os.path.join(get_host_msys2_dir(), "clang64", "bin")
    ffmpeg_dir = os.path.join(PROJECT_ROOT, "installed", "captureengine", "ffmpeg")
    test_env = dict(env)
    test_env["PATH"] = ffmpeg_dir + os.pathsep + msys_bin + os.pathsep + test_env.get("PATH", "")

    if IS_LINUX:
        wine_exe = shutil.which("wine64") or shutil.which("wine")
        if not wine_exe:
            log("Error: Running unit_tests.exe on Linux requires Wine in PATH.")
            return False
        cmd = [wine_exe, test_exe]
    else:
        cmd = [test_exe]
    if gtest_filter:
        cmd.append(f"--gtest_filter={gtest_filter}")
        log(f"Applying unit test filter: {gtest_filter}")
    log("Launching unit_tests.exe...")
    start = time.time()
    result = subprocess.run(
        cmd,
        env=test_env,
        capture_output=True,
        text=True,
        encoding="utf-8",
        errors="replace",
    )
    elapsed = time.time() - start
    log(f"unit_tests.exe finished in {elapsed:.1f}s")
    record_verification_artifact("unit_test_exe", test_exe)
    if result.returncode != 0:
        failure_path = record_unit_test_failure_output(cmd, result)
        log(f"=== Unit Tests FAILED (exit code {result.returncode}) ===")
        if failure_path:
            log(f"Unit-test diagnostics saved to {failure_path}")
        log_unit_test_output_tail("stdout", result.stdout)
        log_unit_test_output_tail("stderr", result.stderr)
        record_verification_step(
            "unit_tests",
            "failed",
            duration_seconds=elapsed,
            details={"exit_code": result.returncode, "gtest_filter": gtest_filter},
        )
        return False

    log("=== Unit Tests Passed ===")
    record_verification_step(
        "unit_tests",
        "passed",
        duration_seconds=elapsed,
        details={"exit_code": result.returncode, "gtest_filter": gtest_filter},
    )
    if gtest_filter or not run_python_tools:
        if not run_python_tools:
            log("Skipping duplicate Python tool self-tests in sanitizer child")
            record_verification_step("python_tool_self_tests", "skipped", details={"reason": "sanitizer_child"})
        return True
    return run_python_tool_self_tests(env)


def run_python_tool_self_tests(env):
    log("=== Running Python Tool Self-Tests ===")
    tool_tests = [
        (
            "ffmpeg_patch_utils",
            [sys.executable, "-m", "unittest", "-v", os.path.join(PROJECT_ROOT, "test_ffmpeg_patch_utils.py")],
        ),
        (
            "build_flag_policy",
            [sys.executable, "-m", "unittest", "-v", os.path.join(PROJECT_ROOT, "test_build_flags.py")],
        ),
        (
            "pe_hardening_policy",
            [sys.executable, "-m", "unittest", "-v", os.path.join(PROJECT_ROOT, "test_pe_hardening.py")],
        ),
        (
            "analyze_av_sync_stimulus",
            [sys.executable, os.path.join(PROJECT_ROOT, "tools", "analyze_av_sync_stimulus.py"), "--self-test"],
        ),
        (
            "analyze_capture_av",
            [sys.executable, os.path.join(PROJECT_ROOT, "tools", "analyze_capture_av.py"), "--self-test"],
        ),
        (
            "run_av_sync_matrix",
            [sys.executable, os.path.join(PROJECT_ROOT, "tools", "run_av_sync_matrix.py"), "--self-test"],
        ),
    ]

    def run_one(tool_test):
        name, command = tool_test
        start = time.time()
        result = subprocess.run(
            command,
            env=env,
            capture_output=True,
            text=True,
            encoding="utf-8",
            errors="replace",
        )
        elapsed = time.time() - start
        return name, command, result, elapsed

    worker_count = get_parallel_job_count(env, len(tool_tests))
    results = []
    with ThreadPoolExecutor(max_workers=worker_count) as executor:
        futures = [executor.submit(run_one, tool_test) for tool_test in tool_tests]
        for future in as_completed(futures):
            results.append(future.result())

    result_by_name = {result[0]: result for result in results}
    ok = True
    for expected_name, _ in tool_tests:
        name, command, result, elapsed = result_by_name[expected_name]
        record_verification_step(
            f"python_tool_self_test.{name}",
            "passed" if result.returncode == 0 else "failed",
            duration_seconds=elapsed,
            details={"exit_code": result.returncode, "command": command},
        )
        if result.stdout:
            log(f"[python_tool_self_test:{name}:stdout]\n{result.stdout.rstrip()}", detail=True)
        if result.stderr:
            log(f"[python_tool_self_test:{name}:stderr]\n{result.stderr.rstrip()}", detail=result.returncode == 0)
        if result.returncode != 0:
            log(f"Python tool self-test failed: {name} (exit code {result.returncode})")
            ok = False
    if ok:
        log("=== Python Tool Self-Tests Passed ===")
    else:
        log("=== Python Tool Self-Tests FAILED ===")
    return ok


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
        "--target-fps",
        "120",
        "--min-frame-ratio",
        "0.60",
        "--max-avg-frame-ratio",
        "1.35",
        "--max-frame-spike-ratio",
        "4.0",
        "--max-spike-pct",
        "5.0",
    ]

    if full_matrix:
        targets = [
            ("full_matrix", ["--api", "all", "--arch", "both"]),
        ]
    else:
        targets = [
            ("smoke_dx9", ["--api", "dx9", "--arch", "both"]),
            ("smoke_dx11", ["--api", "dx11", "--arch", "x64"]),
            ("smoke_modern", ["--api", "both", "--arch", "x64"]),
        ]

    for label, args in targets:
        result_json = os.path.join(logs_dir, f"integration_{label}.json")
        cmd = base_cmd + args + ["--results-json", result_json]
        log(f"Executing: {' '.join(cmd)}")
        target_start = time.time()
        result = subprocess.run(cmd, cwd=os.path.dirname(script), env=env)
        record_verification_artifact(f"integration_{label}", result_json)
        if result.returncode != 0:
            log(f"ERROR: Integration test target failed: {label}")
            record_verification_step(
                "integration_tests",
                "failed",
                duration_seconds=time.time() - target_start,
                details={"target": label, "exit_code": result.returncode, "mode": mode},
            )
            sys.exit(1)

    log("=== Integration Tests Passed ===")
    record_verification_step(
        "integration_tests",
        "passed",
        details={"mode": mode, "targets": [label for label, _ in targets]},
    )


def write_process_diagnostics_artifact(
    artifact_name: str,
    filename: str,
    command: List[str],
    result: subprocess.CompletedProcess,
) -> Optional[str]:
    text = "\n".join(
        [
            f"command: {subprocess.list2cmdline(command)}",
            f"exit_code: {result.returncode}",
            "",
            "[stdout]",
            result.stdout or "<empty>",
            "",
            "[stderr]",
            result.stderr or "<empty>",
            "",
        ]
    )
    return write_verification_artifact(artifact_name, filename, text)


def run_lint(env, *, advisory=False):
    log("=== Running Linting ===")
    lint_start = time.time()
    checks_ok = True
    lint_details: Dict[str, Any] = {}

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
            format_outputs: List[str] = []
            format_issue_files = set()

            for i in range(0, len(files), chunk_size):
                chunk = files[i : i + chunk_size]
                cmd = [clang_format, "--dry-run", "-Werror"] + chunk
                res = subprocess.run(cmd, capture_output=True, text=True, env=env)
                combined_output = "\n".join(part for part in (res.stdout, res.stderr) if part)
                if combined_output:
                    format_outputs.append(f"$ {subprocess.list2cmdline(cmd)}\n{combined_output}")
                    for line in combined_output.splitlines():
                        match = re.match(r"^(.+?):\d+:\d+:\s+(?:error|warning):", line)
                        if match:
                            format_issue_files.add(os.path.normpath(match.group(1)))
                if res.returncode != 0:
                    issues_found += 1

            write_verification_artifact(
                "clang_format_diagnostics",
                "clang_format.log",
                "\n\n".join(format_outputs) + ("\n" if format_outputs else "clang-format: no diagnostics\n"),
            )

            if issues_found > 0:
                log(
                    f"WARNING: C++ style issues found in {len(format_issue_files)} file(s) "
                    f"across {issues_found} batch(es)."
                )
                log("Run 'python build.py --format' to fix them automatically.")
                checks_ok = False
                lint_details["clang_format_batches_with_issues"] = issues_found
                lint_details["clang_format_files_with_issues"] = len(format_issue_files)
            else:
                log("C++ Style: OK")
                lint_details["clang_format_batches_with_issues"] = 0
                lint_details["clang_format_files_with_issues"] = 0
    else:
        log("Error: clang-format not found.")
        checks_ok = False
        lint_details["clang_format_missing"] = True

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
        py_targets = [
            "build.py",
            "ffmpeg_dependencies.py",
            "ffmpeg_patch_utils.py",
            "test_ffmpeg_dependencies.py",
            "test_ffmpeg_patch_utils.py",
            "testapp",
        ]

        cmd = [sys.executable, "-m", "flake8"] + py_targets
        res = subprocess.run(cmd, capture_output=True, text=True)
        write_process_diagnostics_artifact("flake8_diagnostics", "flake8.log", cmd, res)

        if res.returncode != 0:
            log_failure_output_tail("flake8", "\n".join(part for part in (res.stdout, res.stderr) if part))
            log("Python Style: FAILED")
            checks_ok = False
            lint_details["flake8_exit_code"] = res.returncode
        else:
            log("Python Style: OK")
            lint_details["flake8_exit_code"] = 0
    else:
        log("Error: flake8 not installed. (Run 'pip install flake8')")
        checks_ok = False
        lint_details["flake8_missing"] = True

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
        write_process_diagnostics_artifact("pyright_diagnostics", "pyright.log", cmd, res)
        if res.returncode != 0:
            log_failure_output_tail("pyright", "\n".join(part for part in (res.stdout, res.stderr) if part))
            log("Python Types: FAILED")
            checks_ok = False
            lint_details["pyright_exit_code"] = res.returncode
        else:
            log("Python Types: OK")
            lint_details["pyright_exit_code"] = 0
    else:
        log("Error: pyright not installed. (Run 'pip install pyright')")
        checks_ok = False
        lint_details["pyright_missing"] = True

    # 4. C++ Static Analysis (clang-tidy) — uses compile_commands.json, no recompilation
    clang_tidy = os.path.join(MSYS2_DIR, "clang64", "bin", "clang-tidy.exe")
    if IS_LINUX:
        clang_tidy = shutil.which("clang-tidy") or clang_tidy

    if clang_tidy and (IS_LINUX or os.path.exists(clang_tidy)):
        log("Running clang-tidy...")
        run_clang_tidy_script = os.path.join(MSYS2_DIR, "clang64", "bin", "run-clang-tidy")
        compile_db = os.path.join(PROJECT_ROOT, "compile_commands.json")
        if os.path.exists(run_clang_tidy_script) and os.path.exists(compile_db):
            try:
                with open(compile_db, "r", encoding="utf-8") as compile_db_file:
                    compile_db_entries = len(json.load(compile_db_file))
                lint_details["compile_database_entries"] = compile_db_entries
                lint_details["compile_database_sha256"] = sha256_file(compile_db)
                record_verification_artifact("compile_commands", compile_db)
            except Exception as error:
                lint_details["compile_database_error"] = str(error)
            num_workers = cpu_count()
            cmd = [
                sys.executable,
                run_clang_tidy_script,
                "-p",
                PROJECT_ROOT,
                "-j",
                str(num_workers),
                "-quiet",
                "-extra-arg=-w",
            ]
            res = subprocess.run(cmd, capture_output=True, text=True, env=env)
            write_process_diagnostics_artifact("clang_tidy_diagnostics", "clang_tidy.log", cmd, res)
            # Count warnings from stdout (non-fatal for now: existing codebase has
            # many latent issues). We report them so developers can see them without
            # breaking the build.
            warning_lines = []
            if res.stdout:
                for line in res.stdout.splitlines():
                    if "warning:" in line:
                        warning_lines.append(line)
            warning_count = len(warning_lines)
            lint_details["clang_tidy_warnings"] = warning_count
            lint_details["clang_tidy_exit_code"] = res.returncode
            check_counts = Counter()
            subsystem_counts = Counter()
            for line in warning_lines:
                check_match = re.search(r"\[([^\]]+)\]\s*$", line)
                if check_match:
                    check_counts[check_match.group(1)] += 1
                path_match = re.match(r"^(.+?):\d+:\d+:\s+warning:", line)
                if path_match:
                    try:
                        relative_path = os.path.relpath(path_match.group(1), PROJECT_ROOT)
                    except ValueError:
                        relative_path = path_match.group(1)
                    subsystem_counts[relative_path.replace("\\", "/").split("/", 1)[0]] += 1
            lint_details["clang_tidy_checks"] = dict(check_counts.most_common())
            lint_details["clang_tidy_subsystems"] = dict(subsystem_counts.most_common())
            if res.returncode != 0 or warning_count > 0:
                log(f"clang-tidy: {warning_count} warning(s) found (non-fatal)")
                if check_counts:
                    top_checks = ", ".join(f"{name}={count}" for name, count in check_counts.most_common(8))
                    log(f"clang-tidy top checks: {top_checks}")
                if subsystem_counts:
                    top_subsystems = ", ".join(
                        f"{name}={count}" for name, count in subsystem_counts.most_common(8)
                    )
                    log(f"clang-tidy affected subsystems: {top_subsystems}")
                diagnostics_path = verification_artifact_path("clang_tidy.log")
                if diagnostics_path:
                    log(f"Complete clang-tidy diagnostics: {diagnostics_path}")
            else:
                log("clang-tidy: OK")
        else:
            log("Skipping clang-tidy (run-clang-tidy script or compile_commands.json missing)")
            lint_details["clang_tidy_skipped"] = True
    else:
        log("clang-tidy not found. Install via MSYS2: " "pacman -S mingw-w64-clang-x86_64-clang-tools-extra")
        lint_details["clang_tidy_missing"] = True

    clang_tidy_has_findings = bool(lint_details.get("clang_tidy_warnings")) or bool(
        lint_details.get("clang_tidy_exit_code")
    )
    lint_status = "passed"
    if not checks_ok:
        lint_status = "warning" if advisory else "failed"
    elif clang_tidy_has_findings:
        lint_status = "warning"
    record_verification_step(
        "lint",
        lint_status,
        duration_seconds=time.time() - lint_start,
        details={**lint_details, "advisory": advisory},
    )

    return checks_ok


def run_format(env):
    log("=== Running Auto-Format ===")
    format_start = time.time()
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

    record_verification_step(
        "format",
        "passed" if format_ok else "failed",
        duration_seconds=time.time() - format_start,
    )

    return format_ok


def compile_testapps(env, x86_env, clang_exe, cflags):
    """Compile test applications using Clang (and x86 if available)"""
    log("Compiling Test Applications...")

    # x64 test apps intentionally exercise injection into an effectively
    # CFG-instrumented process. Only the broken i686 CRT/load-config path is
    # exempted below.
    cflags = make_cpp_cflags(TESTAPP_OPT_FLAGS_X64, compiler_exe=clang_exe)

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
        x86_arch_flags: List[str] = []
    else:
        clang_exe_x86 = get_compiler_exe("x86")
        have_x86 = clang_exe_x86 is not None and os.path.exists(clang_exe_x86)
        x86_arch_flags = [
            "--target=i686-w64-mingw32",
            "--sysroot=" + os.path.join(MSYS2_DIR, "mingw32"),
            "-mstackrealign",
            "-stdlib=libstdc++",
        ]

    if env.get("CE_SANITIZE") == "1":
        have_x86 = False
        log("Sanitizer mode: skipping x86 test applications (ASan runtime unavailable)")

    cflags_x86 = [
        f
        for f in make_cpp_cflags(
            TESTAPP_OPT_FLAGS_X86,
            compiler_exe=clang_exe_x86,
            arch_flags=x86_arch_flags,
            suppress_microsoft_exception_spec=True,
            enable_cfg=False,
        )
        if f != "-flto"
    ]

    if not IS_LINUX and have_x86 and clang_exe_x86 is not None:
        clangd_x86_flags = _clangd_extra_flags_for_arch("x86", clang_exe_x86)
        x86_include_flags = [
            flag for flag in clangd_x86_flags if flag.startswith("-isystem") or flag.startswith("-resource-dir=")
        ]
        if x86_include_flags:
            cflags_x86.extend(x86_include_flags)

    x86_linker_prefix: List[str] = []
    if not IS_LINUX and have_x86 and clang_exe_x86 is not None:
        x86_std_lib_path = ""
        try:
            res = subprocess.check_output(
                [clang_exe_x86, "-print-libgcc-file-name", "--target=i686-w64-mingw32"],
                encoding="utf-8",
            ).strip()
            x86_std_lib_path = os.path.dirname(res)
        except Exception as e:
            log(f"Warning: Failed to find x86 test app lib path: {e}")
        if not x86_std_lib_path:
            x86_std_lib_path = os.path.join(MSYS2_DIR, "mingw32", "lib")
        x86_linker_prefix = [
            "-Wl,--kill-at",
            "-fuse-ld=lld",
            "-stdlib=libstdc++",
            "-static-libstdc++",
            "-rtlib=libgcc",
            "--unwindlib=libgcc",
            "-lpthread",
            "-L" + x86_std_lib_path,
        ]

    tasks = []
    fg_sdk_cflags = cflags + get_fg_sdk_include_flags()
    vulkan_fg_src = os.path.join(testapp_src_dir, "vulkan_fg_switch_test.cpp")
    vulkan_fg_shader_include = None
    if os.path.exists(vulkan_fg_src):
        vulkan_fg_shader_include = compile_vulkan_fg_shaders(env)
    vulkan_fg_cflags = cflags + get_vulkan_fg_sdk_include_flags()
    if vulkan_fg_shader_include:
        vulkan_fg_cflags.append("-I" + vulkan_fg_shader_include)

    def add_task(desc, cmd, cwd=None, task_env=None):
        if task_env is None:
            task_env = x86_env if x86_env is not None and is_x86_compile_command(cmd) else env
        tasks.append((desc, cmd, cwd, make_task_temp_environment(task_env, desc)))

    def make_cmd(compiler, flags, source, linker_flags, output, *, arch="x64"):
        effective_linker_flags = list(linker_flags) + list(LD_OPT_FLAGS)
        if arch == "x64":
            effective_linker_flags.extend(get_x64_linker_flags(compiler))
        else:
            effective_linker_flags.extend(get_x86_testapp_cfg_link_flags(compiler))
        append_windows_pdb_linker_flag(effective_linker_flags, output)
        return [compiler] + flags + [source] + effective_linker_flags + ["-o", output]

    def make_cmd_x86(compiler, flags, source, linker_flags, output):
        return make_cmd(compiler, flags, source, x86_linker_prefix + list(linker_flags), output, arch="x86")

    vulkan_lib = get_linux_vulkan_import_lib_path("x64")
    vulkan_lib_x86 = get_linux_vulkan_import_lib_path("x86")
    if vulkan_lib is None:
        raise RuntimeError("Vulkan import library unavailable for required x64 test applications")

    # DX12 Test App
    dx12_src = os.path.join(testapp_src_dir, "dx12_test.cpp")
    dx12_exe = os.path.join(testapp_bin_dir, "dx12_test.exe")
    if os.path.exists(dx12_src):
        dx12_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
        ]
        dx12_ldflags.extend(
            [
                "-ld3d12",
                "-ldxgi",
                "-ld3dcompiler",
                "-lgdi32",
                "-luser32",
                "-lshcore",
                "-lavrt",
            ]
        )
        add_task(
            "dx12_test.exe",
            make_cmd(clang_exe, cflags, dx12_src, dx12_ldflags, dx12_exe),
        )

        if have_x86:
            dx12_exe_x86 = os.path.join(x86_bin_dir, "dx12_test.exe")
            add_task(
                "dx12_test.exe (x86)",
                make_cmd_x86(clang_exe_x86, cflags_x86, dx12_src, dx12_ldflags, dx12_exe_x86),
            )

    # DX12 + WASAPI A/V Sync Stimulus App
    av_sync_src = os.path.join(testapp_src_dir, "dx12_av_sync_test.cpp")
    av_sync_exe = os.path.join(testapp_bin_dir, "dx12_av_sync_test.exe")
    if os.path.exists(av_sync_src):
        av_sync_ldflags = list(dx12_ldflags)
        av_sync_ldflags.extend(["-lole32", "-luuid", "-lwinmm"])
        add_task(
            "dx12_av_sync_test.exe",
            make_cmd(clang_exe, cflags, av_sync_src, av_sync_ldflags, av_sync_exe),
        )

        if have_x86:
            av_sync_exe_x86 = os.path.join(x86_bin_dir, "dx12_av_sync_test.exe")
            add_task(
                "dx12_av_sync_test.exe (x86)",
                make_cmd_x86(clang_exe_x86, cflags_x86, av_sync_src, av_sync_ldflags, av_sync_exe_x86),
            )

    # FSR FG DX12 Test App
    fsr_fg_src = os.path.join(testapp_src_dir, "dx12_fsr_fg_test.cpp")
    fsr_fg_exe = os.path.join(testapp_bin_dir, "dx12_fsr_fg_test.exe")
    if os.path.exists(fsr_fg_src):
        fsr_fg_ldflags = list(dx12_ldflags)
        add_task(
            "dx12_fsr_fg_test.exe",
            make_cmd(clang_exe, fg_sdk_cflags, fsr_fg_src, fsr_fg_ldflags, fsr_fg_exe),
        )
        if have_x86:
            log("Skipping dx12_fsr_fg_test.exe (x86): FG SDK runtime DLLs are x64-only")

    # DLSS FG DX12 Test App
    dlss_fg_src = os.path.join(testapp_src_dir, "dx12_dlss_fg_test.cpp")
    dlss_fg_exe = os.path.join(testapp_bin_dir, "dx12_dlss_fg_test.exe")
    if os.path.exists(dlss_fg_src):
        dlss_fg_ldflags = list(dx12_ldflags)
        add_task(
            "dx12_dlss_fg_test.exe",
            make_cmd(clang_exe, fg_sdk_cflags, dlss_fg_src, dlss_fg_ldflags, dlss_fg_exe),
        )
        if have_x86:
            log("Skipping dx12_dlss_fg_test.exe (x86): FG SDK runtime DLLs are x64-only")

    # DLSS/FSR FG Switching DX12 Test App
    fg_switch_src = os.path.join(testapp_src_dir, "dx12_fg_switch_test.cpp")
    fg_switch_exe = os.path.join(testapp_bin_dir, "dx12_fg_switch_test.exe")
    if os.path.exists(fg_switch_src):
        fg_switch_ldflags = list(dx12_ldflags)
        add_task(
            "dx12_fg_switch_test.exe",
            make_cmd(clang_exe, fg_sdk_cflags, fg_switch_src, fg_switch_ldflags, fg_switch_exe),
        )
        if have_x86:
            log("Skipping dx12_fg_switch_test.exe (x86): FG SDK runtime DLLs are x64-only")

    # DX11 Test App
    dx11_src = os.path.join(testapp_src_dir, "dx11_test.cpp")
    dx11_exe = os.path.join(testapp_bin_dir, "dx11_test.exe")
    if os.path.exists(dx11_src):
        dx11_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
        ]
        dx11_ldflags.extend(
            [
                "-ld3d11",
                "-ldxgi",
                "-lgdi32",
                "-luser32",
                "-lshcore",
                "-lavrt",
            ]
        )
        add_task(
            "dx11_test.exe",
            make_cmd(clang_exe, cflags, dx11_src, dx11_ldflags, dx11_exe),
        )

        if have_x86:
            dx11_exe_x86 = os.path.join(x86_bin_dir, "dx11_test.exe")
            add_task(
                "dx11_test.exe (x86)",
                make_cmd_x86(clang_exe_x86, cflags_x86, dx11_src, dx11_ldflags, dx11_exe_x86),
            )

    # DX9 / DX9Ex Test Apps
    dx9_src = os.path.join(testapp_src_dir, "dx9_test.cpp")
    dx9_exe = os.path.join(testapp_bin_dir, "dx9_test.exe")
    if os.path.exists(dx9_src):
        dx9_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
        ]
        dx9_ldflags.extend(
            [
                "-ld3d9",
                "-lgdi32",
                "-luser32",
                "-lavrt",
            ]
        )
        dx9ex_exe = os.path.join(testapp_bin_dir, "dx9ex_test.exe")
        dx9ex_cflags = list(cflags) + ["-DCE_TESTAPP_D3D9EX=1"]

        add_task("dx9_test.exe", make_cmd(clang_exe, cflags, dx9_src, dx9_ldflags, dx9_exe))
        add_task(
            "dx9ex_test.exe",
            make_cmd(clang_exe, dx9ex_cflags, dx9_src, dx9_ldflags, dx9ex_exe),
        )

        if have_x86:
            dx9_exe_x86 = os.path.join(x86_bin_dir, "dx9_test.exe")
            dx9ex_exe_x86 = os.path.join(x86_bin_dir, "dx9ex_test.exe")
            dx9ex_cflags_x86 = list(cflags_x86) + ["-DCE_TESTAPP_D3D9EX=1"]
            add_task(
                "dx9_test.exe (x86)",
                make_cmd_x86(clang_exe_x86, cflags_x86, dx9_src, dx9_ldflags, dx9_exe_x86),
            )
            add_task(
                "dx9ex_test.exe (x86)",
                make_cmd_x86(clang_exe_x86, dx9ex_cflags_x86, dx9_src, dx9_ldflags, dx9ex_exe_x86),
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
                make_cmd_x86(clang_exe_x86, cflags_x86, dx10_src, dx10_ldflags, dx10_exe_x86),
            )

    # Vulkan Test App
    vulkan_src = os.path.join(testapp_src_dir, "vulkan_test.cpp")
    vulkan_exe = os.path.join(testapp_bin_dir, "vulkan_test.exe")
    if os.path.exists(vulkan_src):
        if vulkan_lib is not None:
            vulkan_ldflags = [
                "-static",
                "-Wl,--subsystem,windows",
                vulkan_lib,
                "-lgdi32",
                "-luser32",
                "-lshcore",
                "-lavrt",
            ]
            add_task(
                "vulkan_test.exe",
                make_cmd(clang_exe, cflags, vulkan_src, vulkan_ldflags, vulkan_exe),
            )
        elif IS_LINUX:
            log("Linux host: skipping vulkan_test.exe - Vulkan import library unavailable")

        if have_x86:
            vulkan_exe_x86 = os.path.join(x86_bin_dir, "vulkan_test.exe")
            if vulkan_lib_x86:
                vulkan_ldflags_x86 = [
                    "-static",
                    "-Wl,--subsystem,windows",
                    vulkan_lib_x86,
                    "-lgdi32",
                    "-luser32",
                    "-lshcore",
                    "-lavrt",
                ]
                add_task(
                    "vulkan_test.exe (x86)",
                    make_cmd_x86(
                        clang_exe_x86,
                        cflags_x86,
                        vulkan_src,
                        vulkan_ldflags_x86,
                        vulkan_exe_x86,
                    ),
                )
            else:
                log("Linux host: skipping vulkan_test.exe (x86) - Vulkan import library unavailable")

    # DLSS/FSR FG Switching Vulkan Test App (the Vulkan FidelityFX/Streamline runtimes are x64-only).
    vulkan_fg_exe = os.path.join(testapp_bin_dir, "vulkan_fg_switch_test.exe")
    if os.path.exists(vulkan_fg_src):
        if vulkan_lib is not None:
            vulkan_fg_ldflags = [
                "-static",
                "-Wl,--subsystem,windows",
                vulkan_lib,
                "-lgdi32",
                "-luser32",
                "-lshcore",
                "-lavrt",
                "-lversion",
            ]
            add_task(
                "vulkan_fg_switch_test.exe",
                make_cmd(clang_exe, vulkan_fg_cflags, vulkan_fg_src, vulkan_fg_ldflags, vulkan_fg_exe),
            )
        elif IS_LINUX:
            log("Linux host: skipping vulkan_fg_switch_test.exe - Vulkan import library unavailable")
        if have_x86:
            log("Skipping vulkan_fg_switch_test.exe (x86): FidelityFX/Streamline Vulkan runtimes are x64-only")

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
            add_task(
                "opengl_test.exe (x86)",
                make_cmd_x86(clang_exe_x86, cflags_x86, opengl_src, opengl_ldflags, opengl_exe_x86),
            )

    # Legacy OpenGL Test App
    opengl_legacy_src = os.path.join(testapp_src_dir, "opengl_legacy_test.cpp")
    opengl_legacy_exe = os.path.join(testapp_bin_dir, "opengl_legacy_test.exe")
    if os.path.exists(opengl_legacy_src):
        opengl_legacy_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
            "-lopengl32",
            "-lgdi32",
            "-luser32",
            "-lshcore",
            "-lavrt",
        ]

        add_task(
            "opengl_legacy_test.exe",
            make_cmd(
                clang_exe,
                cflags,
                opengl_legacy_src,
                opengl_legacy_ldflags,
                opengl_legacy_exe,
            ),
        )

        if have_x86:
            opengl_legacy_exe_x86 = os.path.join(x86_bin_dir, "opengl_legacy_test.exe")
            add_task(
                "opengl_legacy_test.exe (x86)",
                make_cmd_x86(
                    clang_exe_x86,
                    cflags_x86,
                    opengl_legacy_src,
                    opengl_legacy_ldflags,
                    opengl_legacy_exe_x86,
                ),
            )

    # DirectDraw7 Test App
    directdraw7_src = os.path.join(testapp_src_dir, "directdraw7_test.cpp")
    directdraw7_exe = os.path.join(testapp_bin_dir, "directdraw7_test.exe")
    if os.path.exists(directdraw7_src):
        directdraw7_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
            "-lddraw",
            "-ldxguid",
            "-lgdi32",
            "-luser32",
            "-lavrt",
        ]

        add_task(
            "directdraw7_test.exe",
            make_cmd(clang_exe, cflags, directdraw7_src, directdraw7_ldflags, directdraw7_exe),
        )

        if have_x86:
            directdraw7_exe_x86 = os.path.join(x86_bin_dir, "directdraw7_test.exe")
            add_task(
                "directdraw7_test.exe (x86)",
                make_cmd_x86(
                    clang_exe_x86,
                    cflags_x86,
                    directdraw7_src,
                    directdraw7_ldflags,
                    directdraw7_exe_x86,
                ),
            )

    # DX6 Test App
    dx6_src = os.path.join(testapp_src_dir, "dx6_test.cpp")
    dx6_exe = os.path.join(testapp_bin_dir, "dx6_test.exe")
    if os.path.exists(dx6_src):
        dx6_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
            "-lddraw",
            "-ldxguid",
            "-lgdi32",
            "-luser32",
            "-lavrt",
        ]

        add_task(
            "dx6_test.exe",
            make_cmd(clang_exe, cflags, dx6_src, dx6_ldflags, dx6_exe),
        )

        if have_x86:
            dx6_exe_x86 = os.path.join(x86_bin_dir, "dx6_test.exe")
            add_task(
                "dx6_test.exe (x86)",
                make_cmd_x86(
                    clang_exe_x86,
                    cflags_x86,
                    dx6_src,
                    dx6_ldflags,
                    dx6_exe_x86,
                ),
            )

    # DX7 Test App
    dx7_src = os.path.join(testapp_src_dir, "dx7_test.cpp")
    dx7_exe = os.path.join(testapp_bin_dir, "dx7_test.exe")
    if os.path.exists(dx7_src):
        dx7_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
            "-lddraw",
            "-ldxguid",
            "-lgdi32",
            "-luser32",
            "-lavrt",
        ]

        add_task(
            "dx7_test.exe",
            make_cmd(clang_exe, cflags, dx7_src, dx7_ldflags, dx7_exe),
        )

        if have_x86:
            dx7_exe_x86 = os.path.join(x86_bin_dir, "dx7_test.exe")
            add_task(
                "dx7_test.exe (x86)",
                make_cmd_x86(
                    clang_exe_x86,
                    cflags_x86,
                    dx7_src,
                    dx7_ldflags,
                    dx7_exe_x86,
                ),
            )

    # DX8 Test App
    dx8_src = os.path.join(testapp_src_dir, "dx8_test.cpp")
    dx8_exe = os.path.join(testapp_bin_dir, "dx8_test.exe")
    if os.path.exists(dx8_src):
        dx8_ldflags = [
            "-static",
            "-Wl,--subsystem,windows",
            "-lgdi32",
            "-luser32",
            "-lavrt",
        ]

        add_task(
            "dx8_test.exe",
            make_cmd(clang_exe, cflags, dx8_src, dx8_ldflags, dx8_exe),
        )

        if have_x86:
            dx8_exe_x86 = os.path.join(x86_bin_dir, "dx8_test.exe")
            add_task(
                "dx8_test.exe (x86)",
                make_cmd_x86(
                    clang_exe_x86,
                    cflags_x86,
                    dx8_src,
                    dx8_ldflags,
                    dx8_exe_x86,
                ),
            )

    # Execute all tasks in parallel
    if not tasks:
        return

    log("Test app compiler temp directories are isolated per task to avoid parallel temp-file collisions")

    def compile_app(t):
        desc, cmd, cwd, tenv = t
        source_index = next(index for index, argument in enumerate(cmd) if argument.endswith(".cpp"))
        output_index = cmd.index("-o", source_index + 1)
        compiler = cmd[0]
        compile_flags = cmd[1:source_index]
        source = cmd[source_index]
        linker_flags = cmd[source_index + 1 : output_index]
        output = cmd[output_index + 1]
        arch = "x86" if is_x86_compile_command(cmd) else "x64"
        object_dir = os.path.join(OBJ_DIR, "testapps", arch)
        object_path = os.path.join(object_dir, os.path.splitext(os.path.basename(output))[0] + ".o")
        os.makedirs(object_dir, exist_ok=True)

        log(f"Building {desc}...", detail=True)
        compiled = compile_object(tenv, compiler, compile_flags, source, object_path)

        # On Linux, log the cross-linker path for diagnostic purposes.
        if IS_LINUX:
            linker_info = _get_linux_cross_linker_info(compiler)
            if linker_info and linker_info.startswith("not_found("):
                log(
                    f"WARNING: Cross-linker not found for {compiler} - reported: {linker_info}",
                    detail=True,
                )

        link_driver_flags = [
            flag
            for flag in compile_flags
            if flag.startswith(("--target=", "--sysroot=", "-stdlib=", "-fsanitize="))
            or flag in ("-m32", "-m64")
        ]
        link_command = [compiler] + link_driver_flags + [object_path] + linker_flags + ["-o", output]
        required_outputs = [output]
        if IS_WINDOWS:
            required_outputs.append(pdb_path_for_binary(output))
        linked = run_cached_link(
            link_command,
            tenv,
            output,
            required_outputs=required_outputs,
            cwd=cwd,
        )
        state = "built" if compiled or linked else "cached"
        log(f"Test app {state}: {desc}", detail=True)
        return compiled, linked

    for _, command, _, _ in tasks:
        compute_compiler_fingerprint(command[0])

    worker_count = get_parallel_job_count(env, len(tasks))
    log(f"Building {len(tasks)} Test Apps in parallel ({worker_count} workers)...")
    errors = []
    built_count = 0
    cached_count = 0
    with ThreadPoolExecutor(max_workers=worker_count) as executor:
        futures = [executor.submit(compile_app, t) for t in tasks]
        for future in as_completed(futures):
            try:
                compiled, linked = future.result()
                if compiled or linked:
                    built_count += 1
                else:
                    cached_count += 1
            except BaseException as e:
                errors.append(e)

    if errors:
        raise RuntimeError(f"{len(errors)} test app(s) failed to build; first error: {errors[0]}")
    log(f"Test app summary: {built_count} built, {cached_count} cached, {len(tasks)} total")

    stale_shader_sidecars = glob.glob(os.path.join(testapp_bin_dir, "vulkan_fg_*.spv"))
    if stale_shader_sidecars:
        raise RuntimeError(f"Vulkan FG runtime shader sidecars are forbidden: {stale_shader_sidecars}")
    if os.path.exists(vulkan_fg_src):
        if not os.path.exists(vulkan_fg_exe):
            raise RuntimeError("vulkan_fg_switch_test.exe was not produced")
        if IS_WINDOWS and not os.path.exists(pdb_path_for_binary(vulkan_fg_exe)):
            raise RuntimeError("vulkan_fg_switch_test.pdb was not produced")


def compile_vulkan_layer(env, clang_exe, cflags, arch):
    """Compile VK_LAYER_CE_overlay - Vulkan implicit layer for overlay and capture"""
    log(f"Compiling Vulkan Layer ({arch})...")

    vulkan_lib = get_linux_vulkan_import_lib_path(arch)
    if not vulkan_lib:
        if arch == "x86" and IS_LINUX:
            log("Linux host: skipping Vulkan Layer (x86) - Vulkan import library unavailable")
            return
        raise RuntimeError(f"Vulkan import library unavailable for required Vulkan Layer ({arch})")

    layer_dir = os.path.join(PROJECT_ROOT, "hook", "vulkan_layer")
    bin_dir = CAPTURE_BIN_DIR
    obj_dir = os.path.join(PROJECT_ROOT, "build", "obj", arch, "vulkan_layer")
    os.makedirs(obj_dir, exist_ok=True)

    # Layer source files - split into layer/support and hook/common sources
    # hook/common sources are shared with the hook DLL and must use the same
    # optimization flags (HOOK_OPT_FLAGS_X64/X86) to maintain consistency.
    layer_only_sources = [
        os.path.join(layer_dir, "layer_main.cpp"),
        os.path.join(layer_dir, "vulkan_layer.cpp"),
        os.path.join(layer_dir, "layer_ipc.cpp"),
        os.path.join(layer_dir, "layer_overlay.cpp"),
        os.path.join(layer_dir, "layer_capture.cpp"),
        os.path.join(layer_dir, "layer_bridge.cpp"),
        os.path.join(layer_dir, "layer_hooks.cpp"),
        # The Vulkan layer intentionally links a selected source set instead of all common objects.
        os.path.join(PROJECT_ROOT, "common", "build_identity.cpp"),
        os.path.join(PROJECT_ROOT, "common", "secure_dll_loading.cpp"),
    ]
    hook_common_sources = [
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
        os.path.join(PROJECT_ROOT, "hook", "common", "screenshot_hook.cpp"),
    ]

    # Shared layer-specific flags (include paths, defines)
    layer_extra_flags = [
        "-I" + layer_dir,
        "-I" + os.path.join(PROJECT_ROOT, "common"),
        "-I" + os.path.join(PROJECT_ROOT, "hook", "common"),
        "-DVK_NO_PROTOTYPES",
        "-DIMGUI_IMPL_VULKAN_NO_PROTOTYPES",
        "-DVK_USE_PLATFORM_WIN32_KHR",
        "-DVK_LAYER_CE_OVERLAY",
    ]

    # Build separate cflags: hook/common sources use hook optimization flags
    if arch == "x64":
        hook_opt_flags = HOOK_OPT_FLAGS_X64
    else:
        hook_opt_flags = HOOK_OPT_FLAGS_X86

    hook_cflags = (
        make_cpp_cflags(
            hook_opt_flags,
            compiler_exe=clang_exe,
            suppress_microsoft_exception_spec=True,
            enable_cfg=arch == "x64",
        )
        + layer_extra_flags
    )
    layer_cflags = cflags + layer_extra_flags

    # x86 cross-compilation from clang64: need --target, --sysroot, and
    # -stdlib=libstdc++ because mingw32 only has libstdc++, not libc++.
    if arch == "x86":
        if not IS_LINUX and is_clang_compiler(clang_exe):
            log("[INFO] x86 Vulkan layer: adding cross-compilation flags (Clang on mingw32)")
            x86_cross_flags = [
                "--target=i686-w64-mingw32",
                "--sysroot=" + os.path.join(MSYS2_DIR, "mingw32"),
                "-stdlib=libstdc++",
            ]
            hook_cflags.extend(x86_cross_flags)
            layer_cflags.extend(x86_cross_flags)

    # Add Vulkan headers include path (from MSYS2 on Linux)
    if IS_LINUX:
        vulkan_include = os.path.join(get_linux_msys2_dir(), "clang64", "include")
        if os.path.exists(vulkan_include):
            hook_cflags.extend(["-idirafter", vulkan_include])
            layer_cflags.extend(["-idirafter", vulkan_include])

    layer_objs = []

    # helper to add sources
    def add_sources(sources, dest_obj_dir):
        added = []
        for src in sources:
            if not os.path.exists(src):
                log(f"Warning: Layer source not found: {src}")
                continue
            basename = os.path.splitext(os.path.basename(src))[0]
            obj = os.path.join(dest_obj_dir, basename + ".o")
            added.append((src, obj))
            layer_objs.append(obj)
        return added

    # Compile layer-specific sources with layer cflags
    layer_src_obj_pairs = add_sources(layer_only_sources, obj_dir)
    # Compile hook/common sources with hook cflags (consistent with hook DLL)
    hook_src_obj_pairs = add_sources(hook_common_sources, obj_dir)

    if not layer_src_obj_pairs and not hook_src_obj_pairs:
        log("Error: No layer sources found.")
        return

    # Run parallel compilation for each group with their respective flags
    total_compiled = 0
    total_skipped = 0
    if layer_src_obj_pairs:
        c, s = parallel_compile(env, clang_exe, layer_cflags, layer_src_obj_pairs)
        total_compiled += c
        total_skipped += s
    if hook_src_obj_pairs:
        c, s = parallel_compile(env, clang_exe, hook_cflags, hook_src_obj_pairs)
        total_compiled += c
        total_skipped += s
    compiled = total_compiled
    skipped = total_skipped
    if compiled > 0:
        log(f"Vulkan Layer ({arch}): compiled {compiled}, skipped {skipped}")

    # Link layer DLL
    if arch == "x64":
        layer_dll_name = "VK_LAYER_CE_overlay.dll"
    else:
        layer_dll_name = "VK_LAYER_CE_overlay_x86.dll"

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

    ldflags.extend(LD_OPT_FLAGS)  # Keep hardening and debug info for crash dumps
    if arch == "x86" and IS_LINUX:
        ldflags.append("-Wl,--allow-multiple-definition")
    if arch == "x64":
        ldflags.extend(get_x64_linker_flags(clang_exe))  # High-entropy ASLR and supported CFG
    if arch == "x86":
        ldflags.append("-Wl,--kill-at")
        if not IS_LINUX:
            ldflags.extend(
                [
                    "--target=i686-w64-mingw32",
                    "--sysroot=" + os.path.join(MSYS2_DIR, "mingw32"),
                    "-fuse-ld=lld",
                    "-stdlib=libstdc++",
                    "-rtlib=libgcc",
                    "--unwindlib=libgcc",
                    "-lpthread",
                ]
            )

        # Re-add -static for proper linking
        if "-static" not in ldflags:
            ldflags.insert(0, "-static")

    append_windows_pdb_linker_flag(ldflags, layer_dll)

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

        # Give every packaged build its own loader identity. The Windows loader
        # keeps only the first occurrence of a layer name, so a stale manifest
        # from another build must not shadow the current one. Discovery also
        # carries this build number, keeping older layers dormant if both load.
        layer_name_base = "VK_LAYER_CE_overlay" if arch == "x64" else "VK_LAYER_CE_overlay_x86"
        layer_name = f"{layer_name_base}_b{CURRENT_BUILD_NUMBER}"

        manifest = {
            "file_format_version": "1.2.0",
            "layer": {
                "name": layer_name,
                "type": "GLOBAL",
                "library_path": manifest_dll_name,
                "api_version": "1.3.0",
                "implementation_version": str(CURRENT_BUILD_NUMBER),
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

    except Exception as e:
        log(f"Error linking layer: {e}")
        raise


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
            if os.path.basename(src) == "screenshot_encoding.cpp":
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
    tests_dir = os.path.join(PROJECT_ROOT, "tests")
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


def sanitizer_regression_command(ccache_flag: bool) -> List[str]:
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
    return cmd


def run_sanitizer_regression_pass(ccache_flag: bool) -> None:
    """Run a second validation pass with ASan/UBSan + unit tests."""
    cmd = sanitizer_regression_command(ccache_flag)

    log("=== Running sanitizer regression cadence pass ===")
    sanitizer_start = time.time()
    sanitizer_log = verification_artifact_path("sanitize_regression.log")
    sanitizer_detail_log = verification_artifact_path("sanitize_regression.details.log")
    if sanitizer_log:
        cmd.append(f"--log-file={sanitizer_log}")
    if sanitizer_detail_log:
        cmd.append(f"--detail-log={sanitizer_detail_log}")
    result = run_logged_subprocess(cmd, cwd=PROJECT_ROOT)
    elapsed = time.time() - sanitizer_start
    if result.returncode != 0:
        log(f"ERROR: Sanitizer regression pass failed (exit code {result.returncode})")
        record_verification_step(
            "sanitize_regression",
            "failed",
            duration_seconds=elapsed,
            details={"exit_code": result.returncode},
        )
        sys.exit(result.returncode)
    log("=== Sanitizer regression cadence pass: OK ===")
    if sanitizer_log:
        record_verification_artifact("sanitize_regression_log", sanitizer_log)
    if sanitizer_detail_log:
        record_verification_artifact("sanitize_regression_detail_log", sanitizer_detail_log)
    record_verification_step("sanitize_regression", "passed", duration_seconds=elapsed)


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


def main():
    script_dir = os.path.dirname(os.path.abspath(__file__))
    os.chdir(script_dir)
    apply_workspace_temp_environment()

    global DETAIL_LOG_FILE, LOG_FILE, VERIFICATION_FINAL_EXIT_CODE, CONCISE_OUTPUT, VERBOSE_COMMANDS

    args = sys.argv[1:]
    VERBOSE_COMMANDS = "--verbose-commands" in args
    CONCISE_OUTPUT = not VERBOSE_COMMANDS
    log_file_override = parse_flag_value("--log-file")
    if log_file_override:
        LOG_FILE = os.path.abspath(log_file_override)
    detail_log_override = parse_flag_value("--detail-log")
    if detail_log_override:
        DETAIL_LOG_FILE = os.path.abspath(detail_log_override)
    os.makedirs(os.path.dirname(LOG_FILE), exist_ok=True)
    if DETAIL_LOG_FILE:
        os.makedirs(os.path.dirname(DETAIL_LOG_FILE), exist_ok=True)

    # 0. Backup Sources - DISABLED per user request
    # backup_sources(script_dir)

    if os.path.exists(LOG_FILE):
        try:
            os.remove(LOG_FILE)
        except Exception:
            pass
    if DETAIL_LOG_FILE and os.path.abspath(DETAIL_LOG_FILE) != os.path.abspath(LOG_FILE) and os.path.exists(
        DETAIL_LOG_FILE
    ):
        try:
            os.remove(DETAIL_LOG_FILE)
        except Exception:
            pass

    log("=== Starting Build ===")

    # Parse flags early for force rebuild
    force_rebuild = "--force-rebuild" in sys.argv
    resume_requested = "--resume" in sys.argv
    if force_rebuild and resume_requested:
        log("ERROR: --resume and --force-rebuild are mutually exclusive")
        sys.exit(2)
    if force_rebuild:
        log("FORCE REBUILD: Cleaning all object files...")
        if os.path.exists(OBJ_DIR):
            try:
                shutil.rmtree(OBJ_DIR)
                log("Object directory cleaned.")
            except Exception as e:
                log(f"Warning: Could not clean {OBJ_DIR}: {e}")

    # Always clean object files to avoid struct layout mismatches
    # if os.path.exists(OBJ_DIR):
    #     log("Cleaning object files for fresh build...")
    #     try:
    #         shutil.rmtree(OBJ_DIR)
    #     except Exception as e:
    #         log(f"Warning: Could not clean {OBJ_DIR}: {e}")

    # Preserve log files in logs/ subfolder across builds.
    # Logs, crash dumps, and CSV files are kept so that crash analysis can
    # be performed even after a rebuild.  Delete them manually when needed.
    logs_dir = os.path.join(BIN_DIR, "logs")
    if os.path.exists(logs_dir):
        log("Preserving existing log files (not cleaning logs/).")

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

    # Parse flags
    default_quality_mode = is_default_quality_invocation(args)
    verify_flag = "--verify" in sys.argv
    skip_updates = "--skip-updates" in sys.argv
    run_tests_flag = "--run-tests" in sys.argv
    run_integration_flag = "--run-integration-tests" in sys.argv
    full_integration_flag = "--full-integration" in sys.argv
    lint_flag = "--lint" in sys.argv
    format_flag = "--format" in sys.argv
    ccache_flag = "--ccache" in sys.argv
    no_build_flag = "--no-build" in sys.argv
    gtest_filter = parse_flag_value("--gtest-filter")
    tests_only_flag = "--tests-only" in sys.argv
    sanitize_flag = "--sanitize" in sys.argv
    sanitize_x86_flag = "--sanitize-x86" in sys.argv
    sanitize_regression_flag = "--sanitize-regression" in sys.argv
    sanitize_regression_child = "--sanitize-regression-child" in sys.argv
    # --production: build signed production binaries (requires CE_PRODUCTION_BUILD=1)
    # Dev builds do NOT pass this flag; signature verification is a warning only.
    production_flag = "--production" in sys.argv or "CE_PRODUCTION_BUILD" in os.environ
    # --force is now DEFAULT behavior for reliability (disable with --incremental)
    resume_flag = "--resume" in sys.argv
    incremental_flag = "--incremental" in sys.argv or resume_flag
    force_flag = not incremental_flag  # Force rebuild by default

    if resume_flag and no_build_flag:
        log("ERROR: --resume requires a build and cannot be combined with --no-build")
        sys.exit(2)
    if resume_flag and not skip_updates:
        log("ERROR: --resume requires --skip-updates so the toolchain/dependency boundary stays unchanged")
        sys.exit(2)

    # --jobs N: override parallel compilation worker count (default: all CPU cores)
    jobs_flag = None
    for i, arg in enumerate(sys.argv):
        if arg == "--jobs" and i + 1 < len(sys.argv):
            try:
                jobs_flag = int(sys.argv[i + 1])
            except ValueError:
                log(f"Warning: invalid --jobs value {sys.argv[i + 1]!r}; using auto worker count")
            break
        elif arg.startswith("--jobs="):
            try:
                jobs_flag = int(arg.split("=", 1)[1])
            except ValueError:
                log(f"Warning: invalid --jobs value {arg!r}; using auto worker count")
            break
    if VERBOSE_COMMANDS:
        log("Verbose command logging enabled (--verbose-commands)")
    else:
        log("Concise console output enabled; complete commands and subprocess output go to the detailed log")

    if sanitize_x86_flag:
        log(
            "ERROR: x86 ASan/UBSan coverage was explicitly requested, but the required MinGW x86 sanitizer "
            "runtime is unavailable. Refusing to silently skip x86 coverage."
        )
        sys.exit(2)

    try:
        current_build_number = resolve_build_number_for_invocation(
            sanitize_regression_child=sanitize_regression_child,
            resume_failed_build=resume_flag,
            no_build=no_build_flag,
            tests_only=tests_only_flag,
        )
    except RuntimeError as error:
        action = "resume failed build" if resume_flag else "reuse current build version"
        log(f"ERROR: Cannot {action}: {error}")
        sys.exit(1)
    if sanitize_regression_child:
        log(f"Reusing parent build version: 0.1.{current_build_number}")
    elif resume_flag:
        log(f"Resuming failed build version: 0.1.{current_build_number}")
    elif no_build_flag:
        log(f"No-build verification reuses current build version: 0.1.{current_build_number}")
    elif tests_only_flag:
        log(f"Tests-only build reuses current product version: 0.1.{current_build_number}")
    # Store for use by compile_project
    global CURRENT_BUILD_NUMBER
    CURRENT_BUILD_NUMBER = current_build_number
    init_verification_context(
        args,
        current_build_number,
        verify_flag or default_quality_mode,
        not sanitize_regression_child,
    )
    record_verification_artifact("live_build_log", LOG_FILE)

    if gtest_filter:
        log(f"Using unit test filter (--gtest-filter): {gtest_filter}")

    if tests_only_flag:
        log("Tests-only build mode enabled (--tests-only)")

    setup_start = time.time()
    setup_msys2(skip_updates=skip_updates)
    record_verification_step(
        "toolchain_setup",
        "passed",
        duration_seconds=time.time() - setup_start,
        details={"skip_updates": skip_updates},
    )
    if should_bootstrap_python_tools(default_quality_mode, verify_flag, lint_flag, format_flag):
        python_bootstrap_start = time.time()
        python_bootstrap_ok = check_python_lsp_tools()
        record_verification_step(
            "python_tool_bootstrap",
            "passed" if python_bootstrap_ok else "warning",
            duration_seconds=time.time() - python_bootstrap_start,
        )
    else:
        log("Skipping optional Python tooling bootstrap for this build")
        record_verification_step("python_tool_bootstrap", "skipped")
    env, clang_bin = get_env()

    if jobs_flag:
        env["CE_BUILD_JOBS"] = str(jobs_flag)
        log(f"Parallel jobs set to {jobs_flag} (--jobs)")

    if default_quality_mode:
        log("Default quality mode: running lint/LSP checks and unit/regression tests (no integration apps)")
        run_tests_flag = True
        lint_flag = True
        sanitize_regression_flag = True

    if verify_flag:
        log("Verification mode: running canonical post-change checks in one pass")
        run_tests_flag = True
        lint_flag = True
        sanitize_regression_flag = True

    if full_integration_flag:
        run_integration_flag = True
    if run_integration_flag:
        run_tests_flag = True
    if sanitize_regression_child:
        sanitize_regression_flag = False

    integration_coverage = "full" if full_integration_flag else "smoke" if run_integration_flag else "not_run"
    record_verification_coverage("integration_tests", integration_coverage)
    if tests_only_flag or no_build_flag:
        signature_coverage = "not_applicable"
    else:
        signature_coverage = "enforced" if production_flag else "advisory"
    record_verification_coverage("production_signatures", signature_coverage)
    record_verification_coverage(
        "sanitizers",
        "x64_asan_ubsan" if sanitize_flag or sanitize_regression_flag else "not_run",
    )
    record_verification_coverage("x86_sanitizers", "unavailable")
    if run_integration_flag:
        test_app_coverage = "executed"
    elif tests_only_flag:
        test_app_coverage = "not_built"
    elif no_build_flag:
        test_app_coverage = "not_run"
    else:
        test_app_coverage = "compiled_not_executed"
    record_verification_coverage("test_apps", test_app_coverage)

    # Store flags in env for access in compile functions
    env["FORCE_REBUILD"] = "1" if force_flag else "0"
    if production_flag:
        env["CE_PRODUCTION_BUILD"] = "1"
        log("PRODUCTION BUILD: DLL signature verification will be enforced")
    else:
        log("DEV BUILD: DLL signature verification is advisory only")

    if incremental_flag:
        mode = "failed-build resume" if resume_flag else "incremental build"
        log(f"Validated {mode} - unchanged objects may be reused by content signature")
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
        HOOK_OPT_FLAGS_X64.extend(sanitize_compile)
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
            log("Integration mode: smoke (--run-integration-tests)")

    if format_flag:
        format_ok = run_format(env)
        if not format_ok:
            log("Auto-format completed with issues.")
        if action_args(args) == ["--format"] and not lint_flag and not run_tests_flag and not run_integration_flag:
            VERIFICATION_FINAL_EXIT_CODE = 0 if format_ok else 1
            sys.exit(VERIFICATION_FINAL_EXIT_CODE)

    standalone_lint = lint_flag and is_standalone_lint_invocation(args)
    if standalone_lint:
        lint_ok = run_lint(env, advisory=False)
        if not lint_ok:
            log("Lint/LSP checks reported issues.")
        VERIFICATION_FINAL_EXIT_CODE = 0 if lint_ok else 1
        sys.exit(VERIFICATION_FINAL_EXIT_CODE)

    externals_prepared = False
    if sanitize_regression_flag and not sanitize_regression_child and not sanitize_flag and not no_build_flag:
        # The sanitizer child and final product build use the same non-instrumented
        # source dependency/FFmpeg closure. Prepare it once before either pass.
        external_start = time.time()
        compile_custom_ffmpeg(skip_updates=skip_updates)
        externals_prepared = True
        record_verification_step(
            "external_preparation",
            "passed",
            duration_seconds=time.time() - external_start,
            details={"skip_updates": skip_updates, "consumers": ["sanitizer", "product"]},
        )

    if sanitize_regression_flag and not sanitize_regression_child:
        if sanitize_flag:
            log("Sanitizer regression cadence requested in sanitizer mode; skipping nested pass")
        else:
            # Run sanitizer validation first so final installed artifacts remain
            # non-sanitized unless --sanitize was explicitly requested.
            run_sanitizer_regression_pass(ccache_flag=ccache_flag)

    build_start = time.time()
    if no_build_flag:
        log("Build skipped (--no-build)")
        if run_tests_flag:
            tests_dir = os.path.join(PROJECT_ROOT, "tests")
            test_exe = os.path.join(tests_dir, "unit_tests.exe")
            if not os.path.exists(test_exe):
                log(f"Error: {test_exe} not found. Build first without --no-build.")
                sys.exit(1)
            if not validate_cached_link_output(test_exe, env):
                log("Error: unit_tests.exe is stale or lacks a valid link-cache manifest. Rebuild tests first.")
                sys.exit(1)
            copy_test_runtime_dlls(tests_dir)
            if not run_tests(
                env,
                test_exe,
                gtest_filter=gtest_filter,
                run_python_tools=not sanitize_regression_child,
            ):
                sys.exit(1)
    else:
        compile_project(
            env,
            clang_bin,
            skip_updates=skip_updates,
            should_run_tests=run_tests_flag,
            gtest_filter=gtest_filter,
            tests_only=tests_only_flag,
            externals_prepared=externals_prepared,
            run_python_tools=not sanitize_regression_child,
        )
    record_verification_step(
        "build",
        "passed",
        duration_seconds=time.time() - build_start,
        details={
            "tests_only": tests_only_flag,
            "run_tests": run_tests_flag,
            "run_integration": run_integration_flag,
            "skip_updates": skip_updates,
            "sanitize": sanitize_flag,
            "no_build": no_build_flag,
            "resume": resume_flag,
        },
    )

    # Publish the database before advisory lint so clang-tidy sees this build's
    # exact compiler, flags, and sources. The atexit hook still preserves a
    # partial database when compilation fails before reaching this point.
    if COMPILE_COMMANDS:
        write_compile_commands_json()
    compile_commands_path = os.path.join(PROJECT_ROOT, "compile_commands.json")
    if os.path.exists(compile_commands_path):
        record_verification_artifact("compile_commands", compile_commands_path)
        try:
            with open(compile_commands_path, "r", encoding="utf-8") as f:
                cc_data = json.load(f)
            record_verification_step(
                "compile_commands",
                "passed",
                details={"entries": len(cc_data), "sha256": sha256_file(compile_commands_path)},
            )
        except Exception:
            record_verification_step("compile_commands", "passed")

    if lint_flag:
        lint_ok = run_lint(env, advisory=True)
        if not lint_ok:
            log("Lint/LSP checks reported advisory issues; complete diagnostics were retained as artifacts.")

    if run_integration_flag:
        ensure_debug_logging()
        run_integration_tests(env, full_matrix=full_integration_flag)

    VERIFICATION_FINAL_EXIT_CODE = 0


if __name__ == "__main__":
    main()
