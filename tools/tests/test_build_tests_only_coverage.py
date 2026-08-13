# build.py executes its fragments via exec, so its module attributes exist only
# at runtime; pyright cannot see them through the facade.
# pyright: reportAttributeAccessIssue=false

import os
import tempfile
import unittest
from pathlib import Path

import build


class TestsOnlyCoverageWarningTest(unittest.TestCase):
    def test_uncovered_source_warns_only_when_newer_than_product_object_or_unbuilt(self) -> None:
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "hook" / "apis" / "example.cpp"
            source.parent.mkdir(parents=True)
            source.write_text("// source", encoding="utf-8")
            obj = Path(temporary) / "obj" / "hook" / "apis" / "example.o"
            obj.parent.mkdir(parents=True)

            # No product object yet: a product build would compile the source.
            self.assertTrue(build.should_warn_tests_only_uncovered_source(str(source), str(obj)))

            obj.write_text("object", encoding="utf-8")
            older = 1_600_000_000
            newer = 1_700_000_000
            os.utime(obj, (older, older))
            os.utime(source, (older - 60, older - 60))
            # Product object is newer: nothing to warn about.
            self.assertFalse(build.should_warn_tests_only_uncovered_source(str(source), str(obj)))

            os.utime(source, (newer, newer))
            # Source modified after the product object was built: warn.
            self.assertTrue(build.should_warn_tests_only_uncovered_source(str(source), str(obj)))

            # A missing source never warns (the enumerator only feeds existing files).
            self.assertFalse(build.should_warn_tests_only_uncovered_source(str(source / "missing.cpp"), str(obj)))

    def test_tests_only_branch_warns_about_uncompiled_product_sources(self) -> None:
        source = build.read_source_text()
        self.assertIn("log_tests_only_uncompiled_product_sources()", source)
        self.assertIn(
            "WARN: tests-only build does not compile {len(uncovered)} modified product source(s)",
            source,
        )
        # The captureengine subset compiled for tests must not be reported as uncovered.
        self.assertIn("TESTS_ONLY_PSEUDO_OVERLAY_SOURCES", source)
        self.assertIn("hook/wrappers/hook_system.cpp", source)
        # The hook compile exclusions must be shared between the product compile set and the warning.
        self.assertIn("HOOK_DLL_EXCLUDED_SOURCES = (", source)
        self.assertIn("rel in HOOK_DLL_EXCLUDED_SOURCES", source)
        self.assertIn("for rel in HOOK_DLL_EXCLUDED_SOURCES", source)


if __name__ == "__main__":
    unittest.main()
