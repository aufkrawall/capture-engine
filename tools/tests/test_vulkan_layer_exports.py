"""Unit tests for tools/verify_vulkan_layer_exports.py."""

from __future__ import annotations

import os
import sys
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", ".."))

from tools.verify_vulkan_layer_exports import (  # noqa: E402
    GATE_FORBIDDEN_EXPORTS,
    GATE_REQUIRED_EXPORTS,
    REQUIRED_EXPORTS,
    forbidden_exports,
    missing_exports,
    parse_exported_names,
)

X64_OUTPUT = """File: VK_LAYER_CE_overlay.dll
Format: COFF-x86-64
Arch: x86_64
AddressSize: 64bit
Export {
  Ordinal: 1
  Name: CEVulkanLayerDeviceEnabledPresentMetering
  RVA: 0x1000
}
Export {
  Ordinal: 2
  Name: CEVulkanLayerIsLiveVulkanSurfaceHwnd
  RVA: 0x1010
}
Export {
  Ordinal: 3
  Name: vkGetDeviceProcAddr
  RVA: 0x1020
}
Export {
  Ordinal: 4
  Name: vkGetInstanceProcAddr
  RVA: 0x1030
}
Export {
  Ordinal: 5
  Name: vkNegotiateLoaderLayerInterfaceVersion
  RVA: 0x1040
}
"""

# The regression this file exists for: the shipped layer exported only the three
# Vulkan entry points, so every hook-DLL GetProcAddress for a bridge query
# returned null and failed closed without a word in any log.
X64_OUTPUT_BEFORE_THE_FIX = """File: VK_LAYER_CE_overlay.dll
Export {
  Ordinal: 1
  Name: vkGetDeviceProcAddr
  RVA: 0x1020
}
Export {
  Ordinal: 2
  Name: vkGetInstanceProcAddr
  RVA: 0x1030
}
Export {
  Ordinal: 3
  Name: vkNegotiateLoaderLayerInterfaceVersion
  RVA: 0x1040
}
"""


class ParseExportedNamesTest(unittest.TestCase):
    def test_reads_every_export_name(self) -> None:
        names = parse_exported_names(X64_OUTPUT)
        self.assertEqual(names, set(REQUIRED_EXPORTS))

    def test_ignores_the_file_and_format_header_lines(self) -> None:
        self.assertNotIn("COFF-x86-64", parse_exported_names(X64_OUTPUT))
        self.assertNotIn("VK_LAYER_CE_overlay.dll", parse_exported_names(X64_OUTPUT))

    def test_normalizes_stdcall_decoration_for_the_x86_layer(self) -> None:
        decorated = "Export {\n  Name: _CEVulkanLayerIsLiveVulkanSurfaceHwnd@4\n}\n"
        self.assertEqual(parse_exported_names(decorated), {"CEVulkanLayerIsLiveVulkanSurfaceHwnd"})

    def test_leaves_an_undecorated_name_containing_at_alone(self) -> None:
        self.assertEqual(parse_exported_names("Export {\n  Name: odd@name\n}\n"), {"odd@name"})


class MissingExportsTest(unittest.TestCase):
    def test_a_complete_layer_reports_nothing_missing(self) -> None:
        self.assertEqual(missing_exports(parse_exported_names(X64_OUTPUT), REQUIRED_EXPORTS), [])

    def test_reports_the_bridge_queries_the_shipped_layer_lost(self) -> None:
        missing = missing_exports(parse_exported_names(X64_OUTPUT_BEFORE_THE_FIX), REQUIRED_EXPORTS)
        self.assertEqual(
            missing,
            ["CEVulkanLayerIsLiveVulkanSurfaceHwnd", "CEVulkanLayerDeviceEnabledPresentMetering"],
        )

    def test_reports_missing_names_in_the_required_order(self) -> None:
        missing = missing_exports({"vkGetInstanceProcAddr"}, REQUIRED_EXPORTS)
        self.assertEqual(missing[0], "vkGetDeviceProcAddr")


GATE_OUTPUT = """File: VK_LAYER_CE_gate.dll
Format: COFF-x86-64
Arch: x86_64
AddressSize: 64bit
Export {
  Ordinal: 1
  Name: vkNegotiateLoaderLayerInterfaceVersion
  RVA: 0x1010
}
"""


class GateExportsTest(unittest.TestCase):
    def test_a_negotiation_only_gate_passes(self) -> None:
        exported = parse_exported_names(GATE_OUTPUT)
        self.assertEqual(missing_exports(exported, GATE_REQUIRED_EXPORTS), [])
        self.assertEqual(forbidden_exports(exported, GATE_FORBIDDEN_EXPORTS), [])

    def test_a_gate_exporting_proc_addresses_is_rejected(self) -> None:
        # A loader that falls back to vkGetInstanceProcAddr after a declined
        # negotiation would chain such a gate into every Vulkan process.
        exported = parse_exported_names(GATE_OUTPUT) | {"vkGetInstanceProcAddr"}
        self.assertEqual(forbidden_exports(exported, GATE_FORBIDDEN_EXPORTS), ["vkGetInstanceProcAddr"])

    def test_a_decorated_x86_proc_address_export_is_still_rejected(self) -> None:
        decorated = parse_exported_names("Export {\n  Name: _vkGetDeviceProcAddr@8\n}\n")
        self.assertEqual(forbidden_exports(decorated, GATE_FORBIDDEN_EXPORTS), ["vkGetDeviceProcAddr"])


if __name__ == "__main__":
    unittest.main()
