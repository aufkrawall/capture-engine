# build.py's executed fragments are invisible to static attribute discovery.
# pyright: reportAttributeAccessIssue=false

import unittest

import build


class FuzzInvocationPolicyTest(unittest.TestCase):
    def test_standalone_fuzz_skips_product_even_with_run_options(self) -> None:
        for options in ([], ["--skip-updates", "--concise"], ["--fuzz-seconds", "10"],
                        ["--fuzz-seconds=1", "--jobs=4", "--log-file", "local.log"]):
            with self.subTest(options=options):
                self.assertTrue(build.should_skip_product_build(["--run-fuzz", *options]))

    def test_explicit_build_and_gate_modes_keep_their_contracts(self) -> None:
        for mode in ("--incremental", "--force-rebuild", "--resume", "--tests-only", "--run-tests",
                     "--verify", "--verify-clean", "--verify-runtime", "--sanitize", "--sanitize-regression",
                     "--run-integration-tests", "--full-integration", "--flow-tests", "--production",
                     "--lint", "--format"):
            with self.subTest(mode=mode):
                self.assertFalse(build.should_skip_product_build(["--run-fuzz", mode]))
                self.assertTrue(build.should_skip_product_build(["--run-fuzz", mode, "--no-build"]))

    def test_ordinary_builds_and_explicit_no_build_are_unchanged(self) -> None:
        self.assertFalse(build.should_skip_product_build([]))
        self.assertFalse(build.should_skip_product_build(["--skip-updates", "--concise"]))
        self.assertTrue(build.should_skip_product_build(["--no-build", "--run-tests"]))

    def test_cli_resolves_no_build_before_product_identity_and_labels_it_truthfully(self) -> None:
        source = build.read_source_text()
        resolve = source.index("no_build_flag = should_skip_product_build(sys.argv[1:])")
        self.assertLess(resolve, source.index("current_build_number = resolve_build_number_for_invocation("))
        self.assertIn("Product build disabled; requested actions build only their own tool targets", source)


if __name__ == "__main__":
    unittest.main()
