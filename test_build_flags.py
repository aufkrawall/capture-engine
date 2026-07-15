import unittest

import build


class BuildFlagPolicyTest(unittest.TestCase):
    def test_testapp_policy_limits_cfg_exception_to_x86(self) -> None:
        x86_arch_flags = [
            "--target=i686-w64-mingw32",
            "--sysroot=" + build.MSYS2_DIR + "\\mingw32",
        ]

        x86_flags = build.make_cpp_cflags(
            build.TESTAPP_OPT_FLAGS_X86,
            arch_flags=x86_arch_flags,
            enable_cfg=False,
        )
        x64_flags = build.make_cpp_cflags(build.TESTAPP_OPT_FLAGS_X64)

        self.assertNotIn(build.CFG_COMPILE_FLAG, x86_flags)
        self.assertIn(build.CFG_COMPILE_FLAG, x64_flags)
        for flags in (x86_flags, x64_flags):
            self.assertIn("-fstack-protector-strong", flags)
            self.assertIn("-D_FORTIFY_SOURCE=2", flags)
        self.assertIn("-fcf-protection=full", x64_flags)
        self.assertNotIn("-fcf-protection=full", x86_flags)

    def test_captureengine_x64_policy_still_has_cfg(self) -> None:
        captureengine_flags = build.make_cpp_cflags(build.OPT_FLAGS_X64)
        self.assertIn(build.CFG_COMPILE_FLAG, captureengine_flags)

    def test_cfg_link_flag_is_x64_only(self) -> None:
        self.assertNotIn(build.CFG_LINK_FLAG, build.LD_OPT_FLAGS)
        self.assertIn(build.CFG_LINK_FLAG, build.LD_OPT_FLAGS_X64)
        self.assertEqual(build.TESTAPP_X86_CFG_LINK_FLAGS, ["-Wl,--no-guard-cf"])

    def test_ffmpeg_policy_includes_stack_and_object_size_hardening(self) -> None:
        with open(build.__file__, encoding="utf-8") as build_file:
            source = build_file.read()
        self.assertIn("-mguard=cf -fstack-protector-strong -D_FORTIFY_SOURCE=2", source)


if __name__ == "__main__":
    unittest.main()
