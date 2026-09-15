# build.py executes its fragments via exec, so its module attributes exist only
# at runtime; pyright cannot see them through the facade.
# pyright: reportAttributeAccessIssue=false

"""What each build gate actually exercises.

Separate from test_build_flags.py because these assertions are about gate *composition*
rather than individual flag handling, and because the coverage keys they pin are the only
machine-readable record of what a passing gate proved. integration_tests was silently
absent from --verify for a long time; these tests exist so that cannot recur unnoticed.
"""

import unittest

import build


class BuildGateCompositionTest(unittest.TestCase):
    def test_verify_does_not_run_the_stages_that_launch_code(self) -> None:
        # --verify is build + unit tests + lint + sanitizers. Nothing in it exercises a real
        # D3D/Vulkan present path or a parser corpus, and the coverage keys have to keep
        # saying so rather than letting a passing gate imply runtime confidence.
        source = build.read_source_text()
        self.assertIn("if verify_flag:", source)
        verify_block = source.split("if verify_flag:", 1)[1].split("if full_integration_flag:", 1)[0]
        self.assertIn("run_tests_flag = True", verify_block)
        self.assertIn("lint_flag = True", verify_block)
        self.assertIn("sanitize_regression_flag = True", verify_block)
        # The integration/fuzz stages must be reachable only through their own opt-in, never
        # from the plain --verify block.
        plain_verify = verify_block.split("if verify_runtime_flag:", 1)[0]
        self.assertNotIn("run_integration_flag = True", plain_verify)
        self.assertNotIn("run_fuzz_flag = True", plain_verify)

    def test_verify_runtime_adds_integration_and_fuzz_on_top_of_verify(self) -> None:
        source = build.read_source_text()
        self.assertIn('verify_runtime_flag = "--verify-runtime" in sys.argv', source)
        # It must imply --verify rather than replace it.
        implication = source.split("if verify_runtime_flag:", 1)[1].split("if verify_flag:", 1)[0]
        self.assertIn("verify_flag = True", implication)
        runtime_block = source.split("Runtime verification mode:", 1)[1].split("if full_integration_flag:", 1)[0]
        self.assertIn("run_integration_flag = True", runtime_block)
        self.assertIn("run_fuzz_flag = True", runtime_block)

    def test_coverage_keys_record_what_each_gate_actually_exercised(self) -> None:
        # These strings are the only machine-readable record of what a gate proved; a future
        # flag refactor must not quietly drop a tier the way integration_tests was dropped.
        source = build.read_source_text()
        self.assertIn('record_verification_coverage("integration_tests", integration_coverage)', source)
        self.assertIn(
            'integration_coverage = "full" if full_integration_flag else "smoke" if run_integration_flag'
            ' else "not_run"',
            source,
        )
        self.assertIn('record_verification_coverage("fuzz", "not_run")', source)
        # x86 sanitizers are genuinely unavailable (MSYS2 clang64 ships no i386 ASan/UBSan
        # runtime), and --sanitize-x86 fails closed rather than skipping silently. Keep the
        # recorded value carrying that reason.
        self.assertIn('record_verification_coverage("x86_sanitizers", "unavailable_no_mingw_i386_runtime")', source)
        self.assertIn("Refusing to silently skip x86 coverage.", source)


if __name__ == "__main__":
    unittest.main()
