#include <gtest/gtest.h>

#include <string>

#include "../captureengine/injection_path_policy.h"

// Locks the containment semantics of the injection DLL path gate: a shared
// string prefix alone must never pass, otherwise "C:\appdir2\evil.dll" would be
// accepted for an application directory of "C:\appdir".
TEST(InjectionPathPolicyTest, AcceptsEqualAndChildPathsInsideDirectory) {
    EXPECT_TRUE(ce::injection::IsPathInsideDirectory("C:\\appdir", "C:\\appdir"));
    EXPECT_TRUE(ce::injection::IsPathInsideDirectory("C:\\appdir\\capture_hook_x64.dll", "C:\\appdir"));
    EXPECT_TRUE(ce::injection::IsPathInsideDirectory("C:\\appdir\\sub\\capture_hook_x86.dll", "C:\\appdir"));
}

TEST(InjectionPathPolicyTest, RejectsSiblingPrefixesAndOutsidePaths) {
    EXPECT_FALSE(ce::injection::IsPathInsideDirectory("C:\\appdir2\\capture_hook_x64.dll", "C:\\appdir"));
    EXPECT_FALSE(ce::injection::IsPathInsideDirectory("C:\\apple\\capture_hook_x64.dll", "C:\\app"));
    EXPECT_FALSE(ce::injection::IsPathInsideDirectory("C:\\other\\capture_hook_x64.dll", "C:\\appdir"));
    EXPECT_FALSE(ce::injection::IsPathInsideDirectory("relative\\capture_hook_x64.dll", "C:\\appdir"));
    EXPECT_FALSE(ce::injection::IsPathInsideDirectory("C:\\appdir\\capture_hook_x64.dll", ""));
}

// The wide conversion feeds WinVerifyTrust. ASCII paths must round-trip
// byte-for-byte, and empty input must fail closed (empty output) so callers can
// reject it instead of verifying an arbitrary path.
TEST(InjectionPathPolicyTest, AnsiPathToWideRoundTripsAsciiAndFailsClosedOnEmpty) {
    const std::string asciiPath = "C:\\Programme\\captureengine\\capture_hook_x64.dll";
    const std::wstring wide = ce::injection::AnsiPathToWide(asciiPath);
    ASSERT_EQ(wide.size(), asciiPath.size());
    for (size_t index = 0; index < asciiPath.size(); ++index) {
        ASSERT_EQ(wide[index], static_cast<wchar_t>(static_cast<unsigned char>(asciiPath[index])));
    }

    EXPECT_TRUE(ce::injection::AnsiPathToWide("").empty());
}
