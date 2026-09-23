#!/usr/bin/env python3
"""Verify the built Vulkan layer DLLs export the names the hook DLL resolves,
and that the negotiation gate exports exactly what the loader needs from it.

The layer exports through ``__declspec(dllexport)`` only; there is no .def file
in any link command. A name that loses the attribute still compiles, still
links, and still has callers - it simply stops being in the export table, and
every ``GetProcAddress`` for it in the hook DLL returns null. Those call sites
fail closed on purpose, so the loss is silent.

That is not hypothetical. ``CEVulkanLayerIsLiveVulkanSurfaceHwnd`` was listed in
``hook/vulkan_layer/layer.def`` and asserted by two source-policy tests, but the
build never read that file, so the final-present vertical-blank rewrite could
never authorize a swapchain (Portal RTX session 20260913_200614). This check
reads the export table of the artifact that actually ships.
"""

from __future__ import annotations

import argparse
import os
import re
import subprocess
import sys
from typing import Iterable, List, Set

# Names the capture hook DLL resolves out of the resident layer at runtime.
REQUIRED_EXPORTS: List[str] = [
    "vkGetInstanceProcAddr",
    "vkGetDeviceProcAddr",
    "vkNegotiateLoaderLayerInterfaceVersion",
    "CEVulkanLayerIsLiveVulkanSurfaceHwnd",
    "CEVulkanLayerDeviceEnabledPresentMetering",
]

LAYER_DLLS: List[str] = ["VK_LAYER_CE_overlay.dll", "VK_LAYER_CE_overlay_x86.dll"]

# The negotiation gate the implicit-layer manifest names (hook/vulkan_layer/
# layer_gate.cpp). It must export negotiation and must NOT export the proc-address
# entry points: when negotiation declines, a loader that falls back to looking
# those names up would otherwise chain the gate into the declined process anyway.
GATE_REQUIRED_EXPORTS: List[str] = ["vkNegotiateLoaderLayerInterfaceVersion"]
GATE_FORBIDDEN_EXPORTS: List[str] = ["vkGetInstanceProcAddr", "vkGetDeviceProcAddr"]
GATE_DLLS: List[str] = ["VK_LAYER_CE_gate.dll", "VK_LAYER_CE_gate_x86.dll"]

_EXPORT_NAME = re.compile(r"^\s*Name:\s*(\S+)\s*$")


def parse_exported_names(readobj_output: str) -> Set[str]:
    """Exported symbol names from ``llvm-readobj --coff-exports`` output.

    The 32-bit layer is linked with ``-Wl,--kill-at``, so a decorated name is
    normalized back to its undecorated form.
    """
    names: Set[str] = set()
    for line in readobj_output.splitlines():
        match = _EXPORT_NAME.match(line)
        if not match:
            continue
        name = match.group(1)
        if name.startswith("_"):
            name = name[1:]
        at_sign = name.rfind("@")
        if at_sign > 0 and name[at_sign + 1 :].isdigit():
            name = name[:at_sign]
        names.add(name)
    return names


def missing_exports(exported: Iterable[str], required: Iterable[str]) -> List[str]:
    present = set(exported)
    return [name for name in required if name not in present]


def forbidden_exports(exported: Iterable[str], forbidden: Iterable[str]) -> List[str]:
    present = set(exported)
    return [name for name in forbidden if name in present]


def read_exports(llvm_readobj: str, dll_path: str) -> Set[str]:
    completed = subprocess.run(
        [llvm_readobj, "--coff-exports", dll_path],
        check=True,
        capture_output=True,
        text=True,
    )
    return parse_exported_names(completed.stdout)


def verify_layer(llvm_readobj: str, dll_path: str) -> List[str]:
    return [
        f"does not export {name}" for name in missing_exports(read_exports(llvm_readobj, dll_path), REQUIRED_EXPORTS)
    ]


def verify_gate(llvm_readobj: str, dll_path: str) -> List[str]:
    exported = read_exports(llvm_readobj, dll_path)
    return [f"does not export {name}" for name in missing_exports(exported, GATE_REQUIRED_EXPORTS)] + [
        f"must not export {name}" for name in forbidden_exports(exported, GATE_FORBIDDEN_EXPORTS)
    ]


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--llvm-readobj", required=True)
    parser.add_argument("--root", required=True, help="Directory holding the built layer DLLs")
    parser.add_argument(
        "--require",
        action="append",
        default=[],
        help="Layer DLL that must exist (repeatable). Others are checked only when present.",
    )
    args = parser.parse_args()

    failures: List[str] = []
    checks = [(dll, verify_layer) for dll in LAYER_DLLS] + [(dll, verify_gate) for dll in GATE_DLLS]
    for dll, verify in checks:
        dll_path = os.path.join(args.root, dll)
        if not os.path.exists(dll_path):
            if dll in args.require:
                failures.append(f"{dll}: missing from {args.root}")
            continue
        for problem in verify(args.llvm_readobj, dll_path):
            failures.append(f"{dll}: {problem}")

    if failures:
        sys.stderr.write("Vulkan layer export verification failed:\n")
        for failure in failures:
            sys.stderr.write(f"  {failure}\n")
        sys.stderr.write(
            "The layer exports through __declspec(dllexport) only; hook-DLL GetProcAddress "
            "callers fail closed in silence when a name is absent.\n"
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
