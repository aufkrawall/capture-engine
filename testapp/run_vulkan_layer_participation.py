#!/usr/bin/env python3
"""End-to-end check of which processes CaptureEngine's Vulkan layer enters.

The implicit layer's manifest names a small negotiation gate that the Vulkan
loader maps into every Vulkan process; the gate loads the full layer only for a
process CaptureEngine may inject into (hook/vulkan_layer/layer_gate.cpp). This
runner drives ``vulkan_test.exe --probe-ce-layer`` - which creates one instance
and reports which layer images are mapped - through four cases:

  host running, whitelisted executable      -> full layer loaded
  host running, unlisted executable         -> full layer NOT loaded
  no host, executable in the persisted list -> full layer loaded (late wake)
  no host, unlisted executable              -> full layer NOT loaded

The no-host cases are the ones unit tests cannot reach: the first version of
the persisted list was written where the staged layer never looked, so every
Vulkan title started before CaptureEngine was declined. The probe reports a
synchronous fact about its own process, so no case depends on timing; the only
wait is for the host to publish its discovery mapping.

Usage:
    python run_vulkan_layer_participation.py [--arch x64|x86|both]
    python run_vulkan_layer_participation.py --self-test
"""

from __future__ import annotations

import argparse
import ctypes
import json
import re
import shutil
import subprocess
import sys
import time
import winreg
from ctypes import wintypes
from pathlib import Path
from typing import Dict, List, Optional, Tuple

SCRIPT_DIR = Path(__file__).parent.absolute()
PROJECT_ROOT = SCRIPT_DIR.parent
TESTAPP_BIN = PROJECT_ROOT / "installed" / "testapp"
CAPTURE_BIN = PROJECT_ROOT / "installed" / "captureengine"
ABI_HEADER = PROJECT_ROOT / "common" / "shared_defs_detail" / "abi_constants_and_config.h"

PROBE_EXE = "vulkan_test.exe"
UNLISTED_EXE = "ce_layer_probe_unlisted.exe"
REGISTRY_KEY = r"Software\CaptureEngine"
REGISTRY_VALUE = "VulkanLayerTargets"
DISCOVERY_MAGIC = 0xCE12CAFE
HOST_PUBLISH_TIMEOUT_S = 60.0

_PROBE_LINE = re.compile(r"^CE_LAYER_PROBE gate=([01]) layer=([01])\s*$", re.MULTILINE)
_DISCOVERY_NAME = re.compile(r'SHARED_MEM_DISCOVERY\s*=\s*L"((?:[^"\\]|\\.)*)"')


def parse_probe_output(stdout: str) -> Optional[Tuple[bool, bool]]:
    """(gate mapped, full layer mapped) from the probe's report line."""
    match = _PROBE_LINE.search(stdout)
    if not match:
        return None
    return match.group(1) == "1", match.group(2) == "1"


def discovery_mapping_name(header_text: str) -> str:
    """The discovery mapping name, read from the header so it follows version bumps."""
    match = _DISCOVERY_NAME.search(header_text)
    if not match:
        raise RuntimeError("SHARED_MEM_DISCOVERY not found in " + str(ABI_HEADER))
    return match.group(1).replace("\\\\", "\\")


def evaluate_case(expect_layer: bool, observed: Optional[Tuple[bool, bool]]) -> Optional[str]:
    """None when the probe matched the expectation, else a failure description."""
    if observed is None:
        return "probe printed no CE_LAYER_PROBE line"
    gate, layer = observed
    if expect_layer and not layer:
        return "full layer was not loaded into an admitted process"
    if expect_layer and not gate:
        return "full layer loaded without the gate: the manifest does not name the gate"
    if not expect_layer and layer:
        return "full layer was loaded into a process CaptureEngine may not inject into"
    return None


def persisted_list_contains(values: List[str], executable: str) -> bool:
    return executable.lower() in (value.lower() for value in values)


def read_persisted_list() -> Optional[List[str]]:
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, REGISTRY_KEY) as key:
            value, value_type = winreg.QueryValueEx(key, REGISTRY_VALUE)
    except OSError:
        return None
    return list(value) if value_type == winreg.REG_MULTI_SZ else None


def discovery_published(name: str) -> bool:
    """True when a host published a discovery mapping with its layout filled in."""
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenFileMappingW.restype = wintypes.HANDLE
    kernel32.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
    kernel32.MapViewOfFile.restype = ctypes.c_void_p
    kernel32.MapViewOfFile.argtypes = [wintypes.HANDLE, wintypes.DWORD, wintypes.DWORD, wintypes.DWORD, ctypes.c_size_t]
    kernel32.UnmapViewOfFile.argtypes = [ctypes.c_void_p]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    file_map_read = 0x0004
    handle = kernel32.OpenFileMappingW(file_map_read, False, name)
    if not handle:
        return False
    try:
        view = kernel32.MapViewOfFile(handle, file_map_read, 0, 0, 16)
        if not view:
            return False
        try:
            fields = (ctypes.c_uint32 * 4).from_address(view)
            # Magic and layout signature stay zero until the host has fully
            # published, whitelist included (captureengine/inject_main.cpp).
            return fields[1] == DISCOVERY_MAGIC and fields[3] != 0
        finally:
            kernel32.UnmapViewOfFile(view)
    finally:
        kernel32.CloseHandle(handle)


def discovery_mapping_exists(name: str) -> bool:
    kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    kernel32.OpenFileMappingW.restype = wintypes.HANDLE
    kernel32.OpenFileMappingW.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.LPCWSTR]
    kernel32.CloseHandle.argtypes = [wintypes.HANDLE]
    handle = kernel32.OpenFileMappingW(0x0004, False, name)
    if not handle:
        return False
    kernel32.CloseHandle(handle)
    return True


def run_probe(exe: Path) -> Tuple[Optional[Tuple[bool, bool]], str]:
    completed = subprocess.run(
        [str(exe), "--probe-ce-layer"], capture_output=True, text=True, timeout=60, cwd=str(exe.parent)
    )
    return parse_probe_output(completed.stdout), completed.stdout


def kill_captureengine() -> None:
    subprocess.run(["taskkill", "/F", "/IM", "captureengine.exe"], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)


def wait_for_host(name: str, process: subprocess.Popen) -> bool:
    # A bounded wait on the host's own publication, not a timing assumption:
    # the host exposes no event for it, and every probe needs it complete.
    deadline = time.monotonic() + HOST_PUBLISH_TIMEOUT_S
    while time.monotonic() < deadline:
        if discovery_published(name):
            return True
        if process.poll() is not None:
            return False
        time.sleep(0.1)
    return False


def run_matrix(arches: List[str]) -> List[Dict[str, object]]:
    sys.path.insert(0, str(SCRIPT_DIR))
    import run_tests_support as support  # noqa: E402 - shares the matrix's config helpers

    name = discovery_mapping_name(ABI_HEADER.read_text(encoding="utf-8"))
    results: List[Dict[str, object]] = []
    copies: List[Path] = []

    def record(case: str, arch: str, expect_layer: bool, exe: Path) -> None:
        observed, stdout = run_probe(exe)
        error = evaluate_case(expect_layer, observed)
        results.append({"case": case, "arch": arch, "expect_layer": expect_layer, "observed": observed,
                        "error": error, "stdout_tail": stdout[-400:] if stdout else ""})
        print(f"  [{arch}] {case}: {'ok' if error is None else 'FAILED - ' + error}")

    snapshot = support.ensure_testapp_profiles([PROBE_EXE])
    try:
        kill_captureengine()
        for arch in arches:
            probe_dir = TESTAPP_BIN if arch == "x64" else TESTAPP_BIN / "x86"
            unlisted = probe_dir / UNLISTED_EXE
            shutil.copy2(probe_dir / PROBE_EXE, unlisted)
            copies.append(unlisted)

        host = subprocess.Popen([str(CAPTURE_BIN / "captureengine.exe")], stdout=subprocess.DEVNULL,
                                stderr=subprocess.DEVNULL)
        if not wait_for_host(name, host):
            kill_captureengine()
            raise RuntimeError("CaptureEngine did not publish its discovery mapping")
        for arch in arches:
            probe_dir = TESTAPP_BIN if arch == "x64" else TESTAPP_BIN / "x86"
            record("host, whitelisted", arch, True, probe_dir / PROBE_EXE)
            record("host, unlisted", arch, False, probe_dir / UNLISTED_EXE)
        kill_captureengine()
        host.wait(timeout=30)

        persisted = read_persisted_list() or []
        list_error = None if persisted_list_contains(persisted, PROBE_EXE) else (
            f"HKCU\\{REGISTRY_KEY}\\{REGISTRY_VALUE} does not list {PROBE_EXE}: {persisted}")
        results.append({"case": "persisted list", "arch": "-", "error": list_error})
        print(f"  persisted list: {'ok' if list_error is None else 'FAILED - ' + list_error}")

        if discovery_mapping_exists(name):
            # Another process retained the mapping; the gate would still see a
            # published host and the registry path would go untested.
            raise RuntimeError("discovery mapping outlived CaptureEngine; the no-host cases cannot run")
        for arch in arches:
            probe_dir = TESTAPP_BIN if arch == "x64" else TESTAPP_BIN / "x86"
            record("no host, listed", arch, True, probe_dir / PROBE_EXE)
            record("no host, unlisted", arch, False, probe_dir / UNLISTED_EXE)
    finally:
        kill_captureengine()
        for copy in copies:
            copy.unlink(missing_ok=True)
        support.restore_capture_config(snapshot)
    return results


def self_test() -> int:
    assert parse_probe_output("noise\nCE_LAYER_PROBE gate=1 layer=0\n") == (True, False)
    assert parse_probe_output("CE_LAYER_PROBE error=vkCreateInstance result=-3\n") is None
    assert evaluate_case(True, (True, True)) is None
    assert evaluate_case(False, (True, False)) is None
    assert evaluate_case(False, (False, False)) is None, "a declined gate may already be unmapped"
    assert "not loaded" in (evaluate_case(True, (True, False)) or "")
    assert "without the gate" in (evaluate_case(True, (False, True)) or "")
    assert "may not inject" in (evaluate_case(False, (True, True)) or "")
    assert evaluate_case(True, None) is not None
    assert discovery_mapping_name('static constexpr const wchar_t* SHARED_MEM_DISCOVERY = L"Local\\\\CE_Disc_63";') == (
        "Local\\CE_Disc_63")
    assert discovery_mapping_name(ABI_HEADER.read_text(encoding="utf-8")).startswith("Local\\CE_Disc_")
    assert persisted_list_contains(["vulkan_test.exe"], "Vulkan_Test.EXE")
    assert not persisted_list_contains([], "vulkan_test.exe")
    print("run_vulkan_layer_participation self-test passed")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--arch", choices=["x64", "x86", "both"], default="both")
    parser.add_argument("--results-json", default=None)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        return self_test()

    arches = ["x64", "x86"] if args.arch == "both" else [args.arch]
    print("Vulkan layer participation:")
    results = run_matrix(arches)
    if args.results_json:
        Path(args.results_json).write_text(json.dumps(results, indent=2, default=str), encoding="utf-8")
    failures = [result for result in results if result.get("error")]
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
