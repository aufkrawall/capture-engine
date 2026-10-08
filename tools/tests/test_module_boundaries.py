"""Behavioral fixtures for checked includes and the shrinking exception set."""

from __future__ import annotations

import contextlib
import copy
import io
import json
import tempfile
import unittest
from pathlib import Path

from tools import check_module_boundaries as boundaries
from tools.analysis.module_depth import measure


class ModuleBoundaryTest(unittest.TestCase):
    def setUp(self) -> None:
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.config = {
            "version": 1,
            "modules": ["hook", "common", "captureengine", "runtime", "frontend", "include", "tests"],
            "rules": {
                "hook/": ["hook/", "common/"], "common/": ["common/"],
                "captureengine/": ["captureengine/", "common/"], "tests/": ["*"],
                "runtime/": ["@own", "common/"],
                "runtime/core/": ["runtime/core/", "@runtime_public", "common/"],
                "runtime/api/": ["runtime/api/", "runtime/core/core.h", "include/cengine/"],
                "frontend/": ["frontend/", "include/cengine/"], "include/": ["include/"],
            },
            "exceptions": [],
        }

    def write(self, name: str, text: str = "") -> Path:
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(text, encoding="utf-8")
        return path

    def check(self) -> dict:
        return boundaries.evaluate(self.root, self.config)

    def exception(self, source: str, target: str) -> dict:
        return {"source": source, "target": target, "reason": "Existing fixture dependency", "milestone": "M3"}

    def test_allowed_common_and_same_directory_edges(self) -> None:
        self.write("common/platform/path.h")
        self.write("hook/runtime/local.h")
        self.write("hook/runtime/main.cpp", '#include "common/platform/path.h"\n#include "local.h"\n')
        result = self.check()
        self.assertTrue(result["success"])
        self.assertEqual(result["include_edges"], 2)

    def test_hook_cannot_reach_controller_internal_header(self) -> None:
        self.write("captureengine/app/main_internal.h")
        self.write("hook/runtime/main.cpp", '#include "captureengine/app/main_internal.h"\n')
        result = self.check()
        self.assertFalse(result["success"])
        self.assertEqual(result["violations"], [
            {"source": "hook/runtime/main.cpp", "target": "captureengine/app/main_internal.h", "line": 1}
        ])

    def test_shipping_policy_rejects_controller_include_from_hook(self) -> None:
        policy = Path(__file__).resolve().parents[1] / "module_boundaries.json"
        config = json.loads(policy.read_text(encoding="utf-8"))
        self.write("captureengine/app/main_internal.h")
        self.write("hook/runtime/new_dependency.cpp", '#include "captureengine/app/main_internal.h"\n')
        result = boundaries.evaluate(self.root, config)
        self.assertFalse(result["success"])
        self.assertEqual(len(result["violations"]), 1)

    def test_angles_native_separators_case_and_parent_paths_cannot_bypass_rules(self) -> None:
        self.write("captureengine/app/main_internal.h")
        includes = (
            '<captureengine/app/main_internal.h>', '"CAPTUREENGINE\\app\\main_internal.h"',
            '"../../captureengine/app/main_internal.h"',
        )
        for include in includes:
            with self.subTest(include=include):
                self.write("hook/runtime/main.cpp", f"# include {include}\n")
                self.assertFalse(self.check()["success"])

    def test_comments_raw_strings_and_line_splicing(self) -> None:
        text = '''/* #include "bad1.h" */
// #include "bad2.h"
auto source = R"fixture(
#include "bad3.h"
)fixture";
#include \\
"good.h" // comment
// continued comment \\
#include "bad4.h"
'''
        self.assertEqual(list(boundaries.include_directives(text)), [(6, "good.h")])

    def test_unresolved_first_party_header_fails_but_sdk_include_is_ignored(self) -> None:
        self.write("hook/runtime/main.cpp", '#include "common/missing.h"\n#include "sdk_header.h"\n')
        self.assertFalse(self.check()["success"])
        self.assertEqual(len(self.check()["unresolved"]), 1)

    def test_generated_build_identity_is_not_a_source_or_missing_header(self) -> None:
        self.write("common/build_version.h", '#include "captureengine/app/missing.h"\n')
        self.write("common/platform/identity.h", '#include "common/build_version.h"\n')
        result = self.check()
        self.assertTrue(result["success"])
        self.assertEqual(result["source_files"], 1)

    def test_only_own_subsystem_and_core_public_headers_are_allowed(self) -> None:
        self.write("runtime/settings/settings.h")
        self.write("runtime/settings/settings_internal.h")
        self.write("runtime/children/children.cpp", '#include "runtime/settings/settings.h"\n')
        self.assertFalse(self.check()["success"])
        self.write("runtime/children/children.cpp", "")
        self.write("runtime/core/core.cpp", '#include "runtime/settings/settings.h"\n')
        self.assertTrue(self.check()["success"])
        self.write("runtime/core/core.cpp", '#include "runtime/settings/settings_internal.h"\n')
        self.assertFalse(self.check()["success"])

    def test_frontend_can_only_use_public_api_and_own_files(self) -> None:
        self.write("include/cengine/cengine.h")
        self.write("common/logging/logging.h")
        self.write("frontend/app/main.cpp", '#include "include/cengine/cengine.h"\n')
        self.assertTrue(self.check()["success"])
        self.write("frontend/app/main.cpp", '#include "common/logging/logging.h"\n')
        self.assertFalse(self.check()["success"])

    def test_internal_header_reach_counts_distinct_includers(self) -> None:
        self.write("hook/runtime/main_internal.h")
        self.write("hook/runtime/main.cpp", '#include "main_internal.h"\n#include "main_internal.h"\n')
        self.write("hook/overlay/draw.cpp", '#include "hook/runtime/main_internal.h"\n')
        result = self.check()
        self.assertFalse(result["success"])
        self.assertEqual(result["internal_headers"]["hook/runtime/main_internal.h"], {
            "includers": 2, "outside_subsystem": 1,
        })

    def test_exact_exception_does_not_allow_another_includer(self) -> None:
        self.write("captureengine/app/main_internal.h")
        self.write("hook/runtime/main.cpp", '#include "captureengine/app/main_internal.h"\n')
        self.config["exceptions"].append(self.exception("hook/runtime/main.cpp", "captureengine/app/main_internal.h"))
        self.assertTrue(self.check()["success"])
        self.write("hook/runtime/other.cpp", '#include "captureengine/app/main_internal.h"\n')
        self.assertFalse(self.check()["success"])

    def test_adding_an_exception_fails_even_if_it_covers_the_new_edge(self) -> None:
        approved = copy.deepcopy(self.config)
        self.write("captureengine/app/main_internal.h")
        self.write("hook/runtime/main.cpp", '#include "captureengine/app/main_internal.h"\n')
        self.config["exceptions"].append(self.exception("hook/runtime/main.cpp", "captureengine/app/main_internal.h"))
        result = boundaries.evaluate(self.root, self.config, approved)
        self.assertFalse(result["success"])
        self.assertEqual(len(result["added_exceptions"]), 1)

    def test_prune_only_removes_stale_exceptions_and_preserves_metadata(self) -> None:
        item = self.exception("hook/runtime/main.cpp", "captureengine/app/main_internal.h")
        stale = self.exception("hook/runtime/old.cpp", "captureengine/app/main_internal.h")
        self.config["exceptions"] = [item, stale]
        self.write("captureengine/app/main_internal.h")
        self.write("hook/runtime/main.cpp", '#include "captureengine/app/main_internal.h"\n')
        path = self.write("tools/module_boundaries.json", json.dumps(self.config))
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(boundaries.main(["--root", str(self.root), "--prune-exceptions"]), 0)
        self.assertEqual(json.loads(path.read_text(encoding="utf-8"))["exceptions"], [item])

    def test_failed_scan_does_not_rewrite_the_exception_file(self) -> None:
        self.config["exceptions"] = [self.exception("hook/runtime/old.cpp", "captureengine/app/main_internal.h")]
        path = self.write("tools/module_boundaries.json", json.dumps(self.config))
        original = path.read_bytes()
        self.write("captureengine/app/main_internal.h")
        self.write("hook/runtime/main.cpp", '#include "captureengine/app/main_internal.h"\n')
        with contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(boundaries.main(["--root", str(self.root), "--prune-exceptions"]), 1)
        self.assertEqual(path.read_bytes(), original)

    def test_invalid_policy_and_undocumented_exceptions_fail_closed(self) -> None:
        mutations = (
            lambda c: c.update(version=2), lambda c: c.update(rules={}),
            lambda c: c["exceptions"].append({
                "source": "hook/x.cpp", "target": "common/x.h", "reason": "", "milestone": "M3",
            }),
        )
        for mutate in mutations:
            with self.subTest(mutate=mutate):
                config = copy.deepcopy(self.config)
                mutate(config)
                with self.assertRaises(ValueError):
                    boundaries.evaluate(self.root, config)

    def test_depth_reports_declared_surface_and_defers_scope_analysis(self) -> None:
        self.write("include/cengine/cengine_draft.h", "int ce_runtime_create(void);\nint ce_runtime_create(void);\n")
        self.write("runtime/core/core.h", "class Runtime {\n int Submit();\n};\n")
        self.write("runtime/core/core.cpp", "// hidden policy\n// and lifetime\n")
        self.write("frontend/app/main.cpp", "// frontend\n")
        metrics = measure(self.root, self.check())
        self.assertEqual(metrics["public_c_functions"], 1)
        self.assertEqual(metrics["frontend_lines"], 1)
        self.assertEqual(metrics["runtime_modules"]["core"]["hidden_lines"], 2)
        self.assertEqual(metrics["runtime_modules"]["core"]["public_declarations_estimate"], 1)
        self.assertIsNone(metrics["writable_globals"])


if __name__ == "__main__":
    unittest.main()
