#include "test_dxgi_shared_shared.h"

TEST(LateGraphicsImports, RepeatedRuntimeLoadsDoNotSweepEveryModuleBeforeReturning) {
    const auto source = ce::test_source::ReadFile(
        std::filesystem::current_path() / "hook/runtime/main_overlay_detect.cpp");
    const auto start = source.find("void NotifyHookModuleLoaded(");
    ASSERT_NE(start, std::string::npos);
    const auto body = source.substr(start);
    EXPECT_NE(body.find("InitializeD3D11Hooks(false)"), std::string::npos);
    EXPECT_EQ(body.find("InitializeGetProcAddressHook()"), std::string::npos);
    const auto loaded = source.find("LDR_DLL_NOTIFICATION_REASON_LOADED");
    ASSERT_NE(loaded, std::string::npos);
    const auto unloaded = source.find("LDR_DLL_NOTIFICATION_REASON_UNLOADED", loaded);
    ASSERT_NE(unloaded, std::string::npos);
    EXPECT_NE(source.substr(loaded, unloaded - loaded).find("PatchDynamicGraphicsImportsForLoadedModule("),
              std::string::npos);
    const auto patch = ce::test_source::ReadFile(
        std::filesystem::current_path() / "hook/hooking/iat_hook_loaded_module.cpp");
    EXPECT_EQ(patch.find("PatchIATAllModules("), std::string::npos);
    EXPECT_EQ(patch.find("EnumerateProcessModules("), std::string::npos);
    for (const char* name : {"GetProcAddress", "D3D11CreateDevice", "D3D11CreateDeviceAndSwapChain"})
        EXPECT_NE(patch.find(name), std::string::npos);
}

TEST(LateGraphicsImports, LoaderTimeImportSlotsRequireResolvedCodeAndShareOverlayExclusions) {
    const auto root = std::filesystem::current_path();
    const auto primitive = ce::test_source::ReadFile(root / "hook/hooking/iat_hook.cpp");
    const auto guard = primitive.find("requireResolvedCode && !ce::module_pin::IsReadableCode(currentFunction, 1)");
    ASSERT_NE(guard, std::string::npos);
    EXPECT_LT(guard, primitive.find("if (currentFunction == hookFunction)", guard));
    EXPECT_NE(primitive.find("IsGraphicsImportHookTargetModulePath(szModName)"), std::string::npos);
    const auto local = ce::test_source::ReadFile(root / "hook/hooking/iat_hook_loaded_module.cpp");
    EXPECT_NE(local.find("IsGraphicsImportHookTargetModulePath(path)"), std::string::npos);
    for (const char* name : {"IsThirdPartyOverlayModulePath", "IsNonSystemGraphicsProxyModulePath",
                             "IsStreamlineFrameGenerationModulePath"})
        EXPECT_NE(local.find(name), std::string::npos);
}

TEST(LateGraphicsImports, NativeImageNotificationsOwnDiscoveryWhileLoaderReturnsKeepTheFallback) {
    const auto source = ce::test_source::ReadFile(
        std::filesystem::current_path() / "hook/runtime/main_overlay_detect.cpp");
    const auto start = source.find("void NotifyHookModuleLoaded(");
    const auto fallback = source.find("if (!g_DllNotificationsActive.load(std::memory_order_acquire))", start);
    ASSERT_NE(start, std::string::npos);
    ASSERT_NE(fallback, std::string::npos);
    const auto synchronous = source.substr(start, fallback - start);
    EXPECT_EQ(synchronous.find("UE5::NotifyModuleLoaded("), std::string::npos);
    EXPECT_EQ(synchronous.find("g_OverlayIdentityRefreshNeeded.store("), std::string::npos);
    EXPECT_EQ(synchronous.find("SetEvent(g_hCheckHooksEvent)"), std::string::npos);
    EXPECT_NE(synchronous.find("InitializeD3D11Hooks(false)"), std::string::npos);
    const auto fallbackEnd = source.find("\n}", fallback);
    ASSERT_NE(fallbackEnd, std::string::npos);
    const auto fallbackBody = source.substr(fallback, fallbackEnd - fallback);
    EXPECT_NE(fallbackBody.find("UE5::NotifyModuleLoaded(module)"), std::string::npos);
    EXPECT_NE(fallbackBody.find("g_OverlayIdentityRefreshNeeded.store(true"), std::string::npos);
    EXPECT_NE(fallbackBody.find("SetEvent(g_hCheckHooksEvent)"), std::string::npos);
    const auto registration = source.find("if (status == 0)");
    ASSERT_NE(registration, std::string::npos);
    EXPECT_NE(source.substr(registration).find("g_DllNotificationsActive.store(true"), std::string::npos);
}

TEST(LateGraphicsImports, FullUEScanPreservesNotificationsAfterItsSnapshotAndRetriesEnumerationFailures) {
    const auto source = ce::test_source::ReadFile(
        std::filesystem::current_path() / "hook/runtime/main_ue5_scan.cpp");
    const auto start = source.find("bool ScanAllLoadedModules()");
    const auto end = source.find("bool ScanPendingModules()", start);
    ASSERT_NE(start, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    const auto body = source.substr(start, end - start);
    const auto clear = body.find("ClearPendingModules()");
    const auto snapshot = body.find("ce::EnumerateProcessModules(");
    ASSERT_NE(clear, std::string::npos);
    ASSERT_NE(snapshot, std::string::npos);
    EXPECT_LT(clear, snapshot);
    EXPECT_EQ(body.find("ClearPendingModules()", clear + 1), std::string::npos);
    EXPECT_NE(body.find("g_fullRescanRequested.store(true"), std::string::npos);
    EXPECT_NE(body.find("enumerationFailureLog.Observe("), std::string::npos);
}
