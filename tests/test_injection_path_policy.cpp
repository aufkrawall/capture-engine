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

TEST(InjectionPathPolicyTest, WideContainmentMatchesNarrowSemantics) {
    EXPECT_TRUE(ce::injection::IsPathInsideDirectory(L"C:\\appdir\\capture_hook_x64.dll", L"C:\\appdir"));
    EXPECT_FALSE(ce::injection::IsPathInsideDirectory(L"C:\\appdir2\\capture_hook_x64.dll", L"C:\\appdir"));
    EXPECT_FALSE(ce::injection::IsPathInsideDirectory(L"C:\\appdir\\capture_hook_x64.dll", L""));
}

// Regression: the injector used GetModuleFileNameA + LoadLibraryA, so an install
// directory containing characters outside the system ANSI code page became
// '?'-mangled and no process could be injected. The path now stays UTF-16 up to
// the remote LoadLibraryW; this pins that a non-ANSI directory survives intact.
TEST(InjectionPathPolicyTest, NonAnsiInstallDirectoryRoundTripsLosslessly) {
    const std::wstring path =
        L"C:\\Users\\\u042E\u043B\u0438\u044F\\\u30B2\u30FC\u30E0\\capture_hook_x64.dll";
    const std::string utf8 = ce::injection::WidePathToUtf8(path);
    ASSERT_FALSE(utf8.empty());
    EXPECT_EQ(utf8.find('?'), std::string::npos);
    const int wideLength =
        MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, utf8.c_str(), static_cast<int>(utf8.size()), nullptr, 0);
    ASSERT_EQ(wideLength, static_cast<int>(path.size()));
    std::wstring roundTrip(static_cast<size_t>(wideLength), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), static_cast<int>(utf8.size()), roundTrip.data(), wideLength);
    EXPECT_EQ(roundTrip, path);

    EXPECT_EQ(ce::injection::RemoteWidePathBytes(path), (path.size() + 1) * sizeof(wchar_t));
    EXPECT_TRUE(ce::injection::WidePathToUtf8(L"").empty());
}
