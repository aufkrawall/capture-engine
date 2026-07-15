import unittest

import build


class BuildFlagPolicyTest(unittest.TestCase):
    def test_testapp_policy_omits_cfg_for_both_architectures(self) -> None:
        x86_arch_flags = [
            "--target=i686-w64-mingw32",
            "--sysroot=" + build.MSYS2_DIR + "\\mingw32",
        ]

        x86_flags = build.make_cpp_cflags(
            build.TESTAPP_OPT_FLAGS_X86,
            arch_flags=x86_arch_flags,
            enable_cfg=False,
        )
        x64_flags = build.make_cpp_cflags(build.TESTAPP_OPT_FLAGS_X64, enable_cfg=False)

        self.assertNotIn(build.CFG_COMPILE_FLAG, x86_flags)
        self.assertNotIn(build.CFG_COMPILE_FLAG, x64_flags)
        for flags in (x86_flags, x64_flags):
            self.assertNotIn("-fcf-protection=full", flags)
            self.assertNotIn("-fstack-protector-strong", flags)
            self.assertNotIn("-D_FORTIFY_SOURCE=2", flags)

    def test_captureengine_x64_policy_still_has_cfg(self) -> None:
        captureengine_flags = build.make_cpp_cflags(build.OPT_FLAGS_X64)
        self.assertIn(build.CFG_COMPILE_FLAG, captureengine_flags)

    def test_cfg_link_flag_is_x64_only(self) -> None:
        self.assertNotIn(build.CFG_LINK_FLAG, build.LD_OPT_FLAGS)
        self.assertIn(build.CFG_LINK_FLAG, build.LD_OPT_FLAGS_X64)
        self.assertEqual(build.TESTAPP_NO_SECURITY_LINK_FLAGS, ["-Wl,--no-guard-cf"])


if __name__ == "__main__":
    unittest.main()
