#include <gtest/gtest.h>

#include "../hook/apis/ffx_hook.h"
#include "../hook/common/streamline_runtime_policy.h"
#include "../hook/wrappers/iat_hook.h"

namespace {

bool StreamlineCoreModuleFilter(const char* moduleBaseName, HMODULE) {
    return ce::streamline_runtime_policy::ShouldHookStreamlineCoreExportsOnLoad(moduleBaseName);
}

bool StreamlineReflexModuleFilter(const char* moduleBaseName, HMODULE) {
    return ce::streamline_runtime_policy::IsStreamlineReflexFeatureModuleName(moduleBaseName);
}

}  // namespace

TEST(IATHookDynamicFilterTest, UnfilteredDynamicHookStillRoutesAllModules) {
    EXPECT_TRUE(IATHook::ShouldApplyDynamicHookForModule(nullptr, "kernel32.dll", nullptr));
    EXPECT_TRUE(IATHook::ShouldApplyDynamicHookForModule(nullptr, "sl.reflex.dll", nullptr));
}

TEST(IATHookTargetFilterTest, WindowsRuntimeDirectoriesAreExcludedCaseInsensitively) {
    EXPECT_TRUE(IATHook::IsWindowsSystemModulePathUnderRoot(L"C:\\Windows\\System32\\KERNELBASE.dll", L"C:\\Windows"));
    EXPECT_TRUE(IATHook::IsWindowsSystemModulePathUnderRoot(L"c:\\windows\\SYSWOW64\\kernel32.dll", L"C:\\WINDOWS\\"));
    EXPECT_FALSE(IATHook::IsWindowsSystemModulePathUnderRoot(L"C:\\Games\\System32\\mod.dll", L"C:\\Windows"));
    EXPECT_FALSE(IATHook::IsWindowsSystemModulePathUnderRoot(L"C:\\Games\\test.exe", L"C:\\Windows"));
    EXPECT_FALSE(IATHook::IsWindowsSystemModulePathUnderRoot(nullptr, L"C:\\Windows"));
    EXPECT_TRUE(IATHook::IsPathUnderDirectoryRoot(L"C:\\Windows\\WinSxS\\runtime.dll", L"c:\\WINDOWS\\"));
    EXPECT_FALSE(IATHook::IsPathUnderDirectoryRoot(L"C:\\WindowsOld\\System32\\kernel32.dll", L"C:\\Windows"));
}

TEST(IATHookDynamicFilterTest, FilteredDynamicHookRoutesOnlyMatchingModules) {
    EXPECT_TRUE(IATHook::ShouldApplyDynamicHookForModule(StreamlineCoreModuleFilter, "sl.interposer.dll", nullptr));
    EXPECT_TRUE(IATHook::ShouldApplyDynamicHookForModule(StreamlineCoreModuleFilter, "SL.COMMON.DLL", nullptr));

    EXPECT_FALSE(IATHook::ShouldApplyDynamicHookForModule(StreamlineCoreModuleFilter, "sl.reflex.dll", nullptr));
    EXPECT_FALSE(IATHook::ShouldApplyDynamicHookForModule(StreamlineCoreModuleFilter, "sl.dlss_g.dll", nullptr));

    EXPECT_TRUE(IATHook::ShouldApplyDynamicHookForModule(StreamlineReflexModuleFilter, "sl.reflex.dll", nullptr));
    EXPECT_FALSE(IATHook::ShouldApplyDynamicHookForModule(StreamlineReflexModuleFilter, "sl.common.dll", nullptr));
}

TEST(IATHookDynamicFilterTest, OverlayCallerBypassKeepsNativeFSRApiHooksVisible) {
    EXPECT_TRUE(IATHook::ShouldBypassDynamicHookForCaller(false, true, false, false, false, false, false, false,
                                                          "D3D12CreateDevice"));
    EXPECT_TRUE(IATHook::ShouldBypassDynamicHookForCaller(false, true, false, false, false, false, false, true,
                                                          "D3D12CreateDevice"));

    EXPECT_FALSE(IATHook::ShouldBypassDynamicHookForCaller(false, true, false, false, false, false, false, true,
                                                           "ffxConfigure"));
    EXPECT_FALSE(IATHook::ShouldBypassDynamicHookForCaller(false, true, false, false, false, false, false, true,
                                                           "ffxCreateContext"));
    EXPECT_FALSE(IATHook::ShouldBypassDynamicHookForCaller(false, true, false, false, false, false, false, true,
                                                           "ffxDestroyContext"));

    EXPECT_TRUE(IATHook::ShouldBypassDynamicHookForCaller(true, false, false, false, false, false, false, true,
                                                          "ffxConfigure"));
    EXPECT_TRUE(IATHook::ShouldBypassDynamicHookForCaller(false, false, false, false, false, true, false, true,
                                                          "ffxConfigure"));
}

TEST(FFXHookPolicyTest, EntryBreakpointHitAcceptsExceptionAddressOrAdvancedInstructionPointer) {
    const uintptr_t target = 0x100000;

    EXPECT_TRUE(FFXHook::detail::IsEntryBreakpointHit(reinterpret_cast<const void*>(target), target + 1,
                                                      reinterpret_cast<const void*>(target)));
    EXPECT_TRUE(FFXHook::detail::IsEntryBreakpointHit(reinterpret_cast<const void*>(target + 1), target + 1,
                                                      reinterpret_cast<const void*>(target)));
    EXPECT_TRUE(FFXHook::detail::IsEntryBreakpointHit(reinterpret_cast<const void*>(0x200000), target + 1,
                                                      reinterpret_cast<const void*>(target)));

    EXPECT_FALSE(FFXHook::detail::IsEntryBreakpointHit(reinterpret_cast<const void*>(0x200000), 0x200001,
                                                       reinterpret_cast<const void*>(target)));
    EXPECT_FALSE(FFXHook::detail::IsEntryBreakpointHit(reinterpret_cast<const void*>(target), target + 1, nullptr));
}
