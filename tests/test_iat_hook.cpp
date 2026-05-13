#include <gtest/gtest.h>

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

TEST(IATHookDynamicFilterTest, FilteredDynamicHookRoutesOnlyMatchingModules) {
    EXPECT_TRUE(IATHook::ShouldApplyDynamicHookForModule(StreamlineCoreModuleFilter, "sl.interposer.dll", nullptr));
    EXPECT_TRUE(IATHook::ShouldApplyDynamicHookForModule(StreamlineCoreModuleFilter, "SL.COMMON.DLL", nullptr));

    EXPECT_FALSE(IATHook::ShouldApplyDynamicHookForModule(StreamlineCoreModuleFilter, "sl.reflex.dll", nullptr));
    EXPECT_FALSE(IATHook::ShouldApplyDynamicHookForModule(StreamlineCoreModuleFilter, "sl.dlss_g.dll", nullptr));

    EXPECT_TRUE(IATHook::ShouldApplyDynamicHookForModule(StreamlineReflexModuleFilter, "sl.reflex.dll", nullptr));
    EXPECT_FALSE(IATHook::ShouldApplyDynamicHookForModule(StreamlineReflexModuleFilter, "sl.common.dll", nullptr));
}
