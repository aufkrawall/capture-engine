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
import threading
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
from collections import deque
from functools import lru_cache
from concurrent.futures import ThreadPoolExecutor, as_completed
from multiprocessing import cpu_count
import re
import json

from typing import Any, Dict, Iterable, List, Mapping, Optional, Set, Tuple, Union

from tools.ffmpeg_dependencies import (
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
from tools.ffmpeg_patch_utils import normalize_custom_patch_targets
from tools.lint_driver import run_lint
from tools.python_tool_self_tests import run_tool_self_tests
from tools.verification_stage_cache import (
    collect_link_manifest_inputs,
    collect_stage_inputs,
    discover_project_inputs,
    success_manifest_matches,
    write_success_manifest,
)
from tools.clang_tidy_cache import (
    analyze_warning_output,
    run_snapshot_preflight,
    write_compile_database_snapshot,
)

sys.modules.setdefault("build", sys.modules[__name__])

# --- Platform Detection ---
IS_WINDOWS = sys.platform == "win32"
IS_LINUX = sys.platform.startswith("linux")
IS_WSL = IS_LINUX and "microsoft" in platform.uname().release.lower()

if IS_WINDOWS:
    # A build must fail, never wait for a mouse click. Windows answers a hard
    # error - an unloadable child image, a missing removable volume - with a
    # modal message box that blocks the raising process indefinitely, and this
    # error mode is inherited by every child, including the test and sanitizer
    # runs. SEM_NOGPFAULTERRORBOX is deliberately not set: crash reporting must
    # keep producing the dumps this project debugs from.
    import ctypes

    SEM_FAILCRITICALERRORS = 0x0001
    SEM_NOOPENFILEERRORBOX = 0x8000
    # Read-modify-write so an inherited mode survives; still single-threaded here.
    _INHERITED_ERROR_MODE = ctypes.windll.kernel32.SetErrorMode(0)
    ctypes.windll.kernel32.SetErrorMode(
        _INHERITED_ERROR_MODE | SEM_FAILCRITICALERRORS | SEM_NOOPENFILEERRORBOX
    )

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
    # CET-compatible codegen (IBT landing pads, shadow-stack-compatible
    # prologues). Enforcement signaling is intentionally NOT emitted: Windows
    # reads CET compatibility from the IMAGE_DEBUG_TYPE_EX_DLLCHARACTERISTICS
    # debug entry (EX_CET_COMPAT), which the clang64 -> lld GNU-mode linker
    # cannot produce (lld has no /cetcompat), and setting the legacy
    # DllCharacteristics bit is invalid (the field is 16-bit). Re-enable when
    # lld gains /cetcompat or the link driver switches to lld-link, then make
    # verify_pe_hardening require the entry. Windows CFG is
    # -mguard=cf/--guard-cf.
    "-fcf-protection=full",
    "-march=x86-64",
    "-mtune=generic",
    "-fvisibility=hidden",
    "-ffunction-sections",
    "-fdata-sections",
] + COMMON_HARDENING_FLAGS + PRIVACY_PREFIX_MAP_FLAGS

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
] + COMMON_HARDENING_FLAGS + PRIVACY_PREFIX_MAP_FLAGS

# x86 builds use generic optimization (no AVX on 32-bit)
OPT_FLAGS_X86 = [
    "-O3",
    "-flto",
    "-march=i686",
    "-mtune=generic",
    "-fvisibility=hidden",
    "-ffunction-sections",
    "-fdata-sections",
] + COMMON_HARDENING_FLAGS + PRIVACY_PREFIX_MAP_FLAGS

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
] + COMMON_HARDENING_FLAGS + PRIVACY_PREFIX_MAP_FLAGS

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
] + COMMON_HARDENING_FLAGS + PRIVACY_PREFIX_MAP_FLAGS
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
# conversion must not vary with contraction/reassociation decisions; register
# every split half of a listed file too. Clang's aggregate strict model has no
# GCC spelling, so use its explicit GCC equivalents on system MinGW GCC.
STRICT_FP_MEDIA_SOURCES = {
    "app_audio_capture.cpp",
    "app_audio_capture_activation.cpp",
    "app_audio_capture_loop.cpp",
    "audio_capture.cpp",
    "audio_capture_loop.cpp",
    "audio_encoder.cpp",
    "audio_encoder_encode.cpp",
    "audio_encoder_flush.cpp",
    "audio_latency_probe.cpp",
    "audio_resampler.cpp",
    "mediaengine.cpp",
    "process_loopback_capture.cpp",
    "process_loopback_worker.cpp",
    "video_encoder.cpp",
    "video_encoder_codec.cpp",
    "video_encoder_configure.cpp",
    "video_encoder_convert_bgra.cpp",
    "video_encoder_convert_shaders.cpp",
    "video_encoder_conversion.cpp",
    "video_encoder_encode.cpp",
    "video_encoder_encode_input.cpp",
    "video_encoder_finalize.cpp",
    "video_encoder_format.cpp",
    "video_encoder_framegrab.cpp",
    "video_encoder_lifecycle.cpp",
    "video_encoder_start.cpp",
    "video_encoder_textures.cpp",
    "video_encoder_write.cpp",
}
STRICT_FP_SCREENSHOT_SOURCES = {"screenshot_encoding.cpp", "screenshot_hdr_encoding.cpp"}
CLANG_STRICT_FP_FLAGS = ["-ffp-model=strict"]
GCC_STRICT_FP_FLAGS = ["-fno-fast-math", "-ffp-contract=off", "-frounding-math", "-fsignaling-nans"]

# --- Configuration ---
BUILD_DIR_NAME = "build"
COMPILE_COMMANDS: List[Dict[str, Any]] = []
CURRENT_BUILD_NUMBER = 0  # Set by bump_and_write_build_version()

PROJECT_ROOT = os.path.dirname(os.path.abspath(__file__))

BUILD_DIR = os.path.join(PROJECT_ROOT, BUILD_DIR_NAME)
ISOLATED_BUILD_ROOT = os.environ.get("CE_ISOLATED_BUILD_ROOT")
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
FG_SDK_INCLUDE_DIR = os.path.join(ISOLATED_BUILD_ROOT, "fg_sdk_include") if ISOLATED_BUILD_ROOT else os.path.join(
    BUILD_DIR, "fg_sdk_include"
)
MSYS2_DIR = os.path.join(BUILD_DIR, "msys64")
OBJ_DIR = os.path.join(ISOLATED_BUILD_ROOT, "obj") if ISOLATED_BUILD_ROOT else os.path.join(BUILD_DIR, "obj")
BIN_DIR = os.path.join(BUILD_DIR, "bin")
INSTALLED_DIR = os.path.join(ISOLATED_BUILD_ROOT, "installed") if ISOLATED_BUILD_ROOT else os.path.join(
    PROJECT_ROOT, "installed"
)
CAPTURE_BIN_DIR = os.path.join(INSTALLED_DIR, "captureengine")
TESTAPP_BIN_DIR = os.path.join(INSTALLED_DIR, "testapp")
BIN_DIR = CAPTURE_BIN_DIR  # output captureengine binaries to installed\captureengine
TEST_OUTPUT_DIR = os.path.join(ISOLATED_BUILD_ROOT, "tests") if ISOLATED_BUILD_ROOT else os.path.join(
    PROJECT_ROOT, "tests"
)
PACKAGE_OUTPUT_DIR = os.path.join(BUILD_DIR, "packages")
CAPTUREENGINE_PACKAGE_NAME = "captureengine.7z"
TESTAPPS_PACKAGE_NAME = "testapps.7z"
FFMPEG_SOURCE_PACKAGE_NAME = "ffmpeg-corresponding-source.7z"
TESTAPP_RUNTIME_NOTE = os.path.join(PROJECT_ROOT, "testapp", "THIRD_PARTY_RUNTIME_REQUIREMENTS.txt")
DEFAULT_LOG_FILE = os.path.join(PROJECT_ROOT, "build.log")
LOG_FILE = DEFAULT_LOG_FILE
DETAIL_LOG_FILE: Optional[str] = None
VERIFICATION_DIR = os.path.join(ISOLATED_BUILD_ROOT or BUILD_DIR, "verification")
VERBOSE_COMMANDS = False
CONCISE_OUTPUT = False
LOG_LOCK = threading.Lock()
FAILURE_OUTPUT_TAIL_LINES = 80
VERIFICATION_CONTEXT: Optional[Dict[str, Any]] = None
_VERIFICATION_RECORD_LOCK = threading.Lock()
VERIFICATION_FINAL_EXIT_CODE = 0
VERIFICATION_ATEXIT_REGISTERED = False
VERIFICATION_FINALIZED = False
WORKSPACE_TEMP_DIR = os.path.join(ISOLATED_BUILD_ROOT, "tmp") if ISOLATED_BUILD_ROOT else os.path.join(BUILD_DIR, "tmp")
CLANG_TIDY_CACHE_DIR = os.path.join(BUILD_DIR, "cache", "clang_tidy", "entries")
CLANG_TIDY_SNAPSHOT_DIR = os.path.join(BUILD_DIR, "cache", "clang_tidy", "full_database")
SANITIZER_STAGE_ROOT = os.path.join(BUILD_DIR, "stages", "sanitize")
SANITIZER_STAGE_MANIFEST = os.path.join(SANITIZER_STAGE_ROOT, "success.json")


def get_compile_commands_path() -> str:
    root = ISOLATED_BUILD_ROOT or PROJECT_ROOT
    return os.path.join(root, "compile_commands.json")


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
    pdb_path = pdb_path_for_binary(binary_path)
    pdb_flag = f"-Wl,--pdb={pdb_path}"
    if pdb_flag not in ldflags:
        ldflags.append(pdb_flag)
    # The image's RSDS debug record embeds whatever path --pdb receives. Embed
    # only the bare PDB file name so the developer profile path never ships in
    # the image; debuggers still locate the sidecar via the module directory
    # and the symbol path.
    alt_flag = f"-Wl,/pdbaltpath:{os.path.basename(pdb_path)}"
    if alt_flag not in ldflags:
        ldflags.append(alt_flag)


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
    formatted = privacy_sanitize_log_text(f"[{timestamp}] {msg}")
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
    with _VERIFICATION_RECORD_LOCK:
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
    with _VERIFICATION_RECORD_LOCK:
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
    with _VERIFICATION_RECORD_LOCK:
        VERIFICATION_CONTEXT.setdefault("steps", {})[name] = step
