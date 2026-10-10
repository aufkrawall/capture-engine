#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

TEST(EarlyLoaderBootstrapTest, NativeLoaderInterceptsBeforeConfigAndOptionalPreloads) {
    const auto root = std::filesystem::current_path();
    const auto source = ce::test_source::ReadLogicalSource(root / "hook/runtime/main_hookthread.cpp");
    const auto worker = source.find("DWORD WINAPI HookThread(");
    ASSERT_NE(worker, std::string::npos);
    const auto early = source.find("InstallLowLevelLoaderHook(\"early hook thread\")", worker);
    ASSERT_NE(early, std::string::npos);
    for (const char* work : {"InstallGlobalVTableHooks();", "LoadConfig(configPath", "PreloadConfiguredThirdPartyDlls();"}) {
        const auto position = source.find(work, worker);
        ASSERT_NE(position, std::string::npos) << work;
        EXPECT_LT(early, position) << work;
    }
    const auto full = source.find("void InstallHookThreadHooks()");
    ASSERT_NE(full, std::string::npos);
    const auto retry = source.find("InstallLowLevelLoaderHook(\"hook thread retry\")", full);
    ASSERT_NE(retry, std::string::npos);
    EXPECT_LT(retry, source.find("PreloadConfiguredGraphicsRuntimeDlls();", full));
    const auto dllmain = ce::test_source::ReadLogicalSource(root / "hook/runtime/main_dllmain.cpp");
    EXPECT_EQ(dllmain.find("InstallLowLevelLoaderHook("), std::string::npos);
}
