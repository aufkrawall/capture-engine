#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

// Split out of test_inject_capture_source.cpp to keep both units under the source-size
// ceiling. This half covers third-party coexistence, late hook installation, and dormant
// pass-through source policy.

namespace {

std::string ReadSource(const std::filesystem::path& relativePath) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / relativePath);
}

}  // namespace

TEST(InjectLifecycleSourceTest, OverlayNotificationRegistrationPrecedesSeedSnapshot) {
    const std::string source = ReadSource("hook/main_overlay_detect.cpp");
    ASSERT_FALSE(source.empty());

    const size_t registration = source.find("registerFn(0, &OverlayDllNotificationCallback");
    const size_t seed = source.find("SeedThirdPartyOverlayModuleCacheFromLoader()");
    ASSERT_NE(registration, std::string::npos);
    ASSERT_NE(seed, std::string::npos);
    EXPECT_LT(registration, seed);
}

TEST(InjectLifecycleSourceTest, RenamedThirdPartyProxyIdentityUsesStableProjectMarkers) {
    const std::string source = ReadSource("hook/main_overlay_detect.cpp");
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("GetProcAddress(retained, \"ReShadeVersion\")"), std::string::npos);
    EXPECT_NE(source.find("GetProcAddress(retained, \"ReShadeRegisterAddon\")"), std::string::npos);
    EXPECT_NE(source.find("DllVersionStringContains(path, \"ReShade\")"), std::string::npos);
    EXPECT_NE(source.find("GetProcAddress(retained, \"SK_GetDLL\")"), std::string::npos);
    EXPECT_NE(source.find("GetProcAddress(retained, \"SK_Inject_GetRecord\")"), std::string::npos);
    EXPECT_NE(source.find("DllVersionStringContains(path, \"Special K\")"), std::string::npos);
    EXPECT_NE(source.find("DllVersionStringContains(path, \"OptiScaler\")"), std::string::npos);
}

TEST(InjectLifecycleSourceTest, DXGICoexistenceNeverBlindlyOverwritesForeignVTableOwners) {
    const std::string install = ReadSource("hook/common/dxgi_shared_hooks.cpp");
    // Both halves of the present-hook unit: the install/entry-ownership decision and the
    // vtable-slot ownership functions it was split from (repair, handoff detach, teardown).
    const std::string presentHooks = ReadSource("hook/common/dxgi_shared_hooks_present.cpp") +
                                     ReadSource("hook/common/dxgi_shared_hooks_present_vtable.cpp");
    const std::string original = ReadSource("hook/common/dxgi_shared_original.cpp");
    const std::string steamRouting = ReadSource("hook/common/dxgi_shared_steam_routing.cpp");
    const std::string dx11Present = ReadSource("hook/apis/dx11_hook_present.cpp");
    ASSERT_FALSE(install.empty());
    ASSERT_FALSE(presentHooks.empty());
    ASSERT_FALSE(original.empty());
    ASSERT_FALSE(steamRouting.empty());
    ASSERT_FALSE(dx11Present.empty());

    EXPECT_NE(install.find("InterlockedCompareExchangePointer"), std::string::npos);
    EXPECT_NE(presentHooks.find("Preserving foreign %s vtable replacement"), std::string::npos);
    EXPECT_EQ(presentHooks.find("dxgi_shared_s_hookedVTable[8] ="), std::string::npos);
    EXPECT_EQ(original.find("dxgi_shared_s_hookedVTable[8] ="), std::string::npos);
    EXPECT_EQ(steamRouting.find("dxgi_shared_s_hookedVTable[8] ="), std::string::npos);
    EXPECT_NE(dx11Present.find("InterlockedCompareExchangePointer"), std::string::npos);
    EXPECT_NE(dx11Present.find("preserving foreign VTable[13] follower"), std::string::npos);
    EXPECT_EQ(dx11Present.find("vtable[13] ="), std::string::npos);

    const size_t externalChain = presentHooks.find("prepending CE at the original entry");
    const size_t inlineInstall = presentHooks.find("InlineHook::InstallPublished(presentAddr", externalChain);
    ASSERT_NE(externalChain, std::string::npos);
    ASSERT_NE(inlineInstall, std::string::npos);
    EXPECT_LT(externalChain, inlineInstall);
}

TEST(InjectLifecycleSourceTest, LateDeepHookPatchingUsesQuiescedExactByteOwnership) {
    const std::string inlineHook = ReadSource("hook/wrappers/inline_hook.cpp");
    const std::string deepHook = ReadSource("hook/wrappers/inline_hook_deep.cpp");
    const std::string deepRemove = ReadSource("hook/wrappers/inline_hook_deep_remove.cpp");
    ASSERT_FALSE(inlineHook.empty());
    ASSERT_FALSE(deepHook.empty());
    ASSERT_FALSE(deepRemove.empty());

    EXPECT_NE(inlineHook.find("g_hooks.back().installedBytes"), std::string::npos);
    EXPECT_NE(deepHook.find("ThreadQuiescence quiescence"), std::string::npos);
    EXPECT_NE(deepHook.find("g_deepHooks.back().installedBytes"), std::string::npos);
    EXPECT_NE(deepRemove.find("Preserving foreign replacement"), std::string::npos);
    EXPECT_EQ(deepHook.find("pPatch[0] = 0xCC"), std::string::npos);
}

TEST(InjectLifecycleSourceTest, LateInlineHooksPublishTheirPredecessorsBeforeGoingLive) {
    const std::string installers =
        ReadSource("hook/main_hookthread.cpp") + ReadSource("hook/main_external_dump.cpp") +
        ReadSource("hook/common/dxgi_shared_hooks_present.cpp") + ReadSource("hook/apis/ddraw_hook_install.cpp") +
        ReadSource("hook/apis/dx8_hook_detours.cpp") + ReadSource("hook/apis/dx9_hook.cpp") +
        ReadSource("hook/apis/dx12_hook_hook_install.cpp") + ReadSource("hook/apis/nvngx_hook_feature.cpp") +
        ReadSource("hook/apis/opengl_hook_install.cpp") + ReadSource("hook/apis/ffx_hook_internal.h") +
        ReadSource("hook/apis/streamline_hook_internal.h");
    ASSERT_FALSE(installers.empty());

    EXPECT_EQ(installers.find("InlineHook::Install("), std::string::npos);
    EXPECT_NE(installers.find("InlineHook::InstallPublished("), std::string::npos);
    EXPECT_NE(installers.find("InlineHook::InstallDeepHookPublished("), std::string::npos);
}

TEST(InjectLifecycleSourceTest, GraphicsConfigCachesTreatReplacementSharedMemoryAsANewHostGeneration) {
    const std::string source = ReadSource("hook/common/hook_common.cpp");
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("currentSharedMemory == lastSharedMemory"), std::string::npos);
    EXPECT_NE(source.find("currentSharedMemory != cachedSharedMemory"), std::string::npos);
    EXPECT_NE(source.find("cachedSharedMemory = currentSharedMemory"), std::string::npos);
}

TEST(InjectLifecycleSourceTest, DormantMutationSensitiveCallsForwardBeforeApplyingOverrides) {
    const std::string dx11 = ReadSource("hook/apis/dx11_hook_present.cpp");
    const std::string dx9 = ReadSource("hook/apis/dx9_hook_device.cpp");
    const std::string vulkanHooks = ReadSource("hook/vulkan_layer/vulkan_layer_hooks.cpp");
    const std::string vulkan = ReadSource("hook/vulkan_layer/vulkan_layer_present.cpp");
    ASSERT_FALSE(dx11.empty());
    ASSERT_FALSE(dx9.empty());
    ASSERT_FALSE(vulkanHooks.empty());
    ASSERT_FALSE(vulkan.empty());

    const size_t lodFix = vulkanHooks.find("void ApplyConfiguredNvLodSpreadFix()");
    const size_t lodDormant = vulkanHooks.find("if (!g_LayerState.whitelisted)", lodFix);
    const size_t lodMutation = vulkanHooks.find("ce::nv_lod_spread::Install", lodFix);
    ASSERT_NE(lodFix, std::string::npos);
    ASSERT_NE(lodDormant, std::string::npos);
    ASSERT_NE(lodMutation, std::string::npos);
    EXPECT_LT(lodDormant, lodMutation);

    const size_t dx11Resize = dx11.find("DetourResizeBuffers(");
    const size_t dx11Dormant = dx11.find("HookIsShuttingDown()", dx11Resize);
    const size_t dx11Override = dx11.find("HasBackbufferCountOverride", dx11Resize);
    ASSERT_NE(dx11Resize, std::string::npos);
    ASSERT_NE(dx11Dormant, std::string::npos);
    ASSERT_NE(dx11Override, std::string::npos);
    EXPECT_LT(dx11Dormant, dx11Override);

    const size_t dx9Create = dx9.find("DetourCreateDeviceEx(");
    const size_t dx9Dormant = dx9.find("HookIsShuttingDown()", dx9Create);
    const size_t dx9Override = dx9.find("GetActiveGraphicsConfig()", dx9Create);
    ASSERT_NE(dx9Create, std::string::npos);
    ASSERT_NE(dx9Dormant, std::string::npos);
    ASSERT_NE(dx9Override, std::string::npos);
    EXPECT_LT(dx9Dormant, dx9Override);

    const size_t acquire = vulkan.find("Capture_vkAcquireNextImageKHR(");
    const size_t acquireDormant = vulkan.find("!g_LayerState.whitelisted.load", acquire);
    const size_t acquireTracking = vulkan.find("GetSwapchainData", acquire);
    ASSERT_NE(acquire, std::string::npos);
    ASSERT_NE(acquireDormant, std::string::npos);
    ASSERT_NE(acquireTracking, std::string::npos);
    EXPECT_LT(acquireDormant, acquireTracking);

    const size_t sampler = vulkan.find("Capture_vkCreateSampler(");
    const size_t samplerDormant = vulkan.find("!g_LayerState.whitelisted.load", sampler);
    const size_t samplerCopy = vulkan.find("VkSamplerCreateInfo modified", sampler);
    ASSERT_NE(sampler, std::string::npos);
    ASSERT_NE(samplerDormant, std::string::npos);
    ASSERT_NE(samplerCopy, std::string::npos);
    EXPECT_LT(samplerDormant, samplerCopy);
}

TEST(InjectLifecycleSourceTest, ThirdPartyPreloadPrecedesWrapperAndRuntimePreloads) {
    const std::string source = ReadSource("hook/main_hookthread.cpp");
    ASSERT_FALSE(source.empty());

    const size_t configParse = source.find("LoadConfig(configPath, *g_pLocalConfig);");
    const size_t thirdParty = source.find("PreloadConfiguredThirdPartyDlls();");
    const size_t wrapperLoad = source.find("Load wrapper DLLs for all graphics APIs");
    const size_t runtimePreload = source.find("PreloadConfiguredGraphicsRuntimeDlls();");
    ASSERT_NE(configParse, std::string::npos);
    ASSERT_NE(thirdParty, std::string::npos);
    ASSERT_NE(wrapperLoad, std::string::npos);
    ASSERT_NE(runtimePreload, std::string::npos);
    EXPECT_LT(configParse, thirdParty);
    EXPECT_LT(thirdParty, wrapperLoad);
    EXPECT_LT(thirdParty, runtimePreload);
}

TEST(InjectLifecycleSourceTest, SwapchainWrapperDestructorGuardsTheFinalRealRelease) {
    const std::string source = ReadSource("hook/wrappers/dxgi_swapchain_wrap_lifetime.cpp");
    ASSERT_FALSE(source.empty());

    const size_t guard =
        source.find("ShouldReleaseRealSwapchainWrapperReferenceDuringWrapperDestructor(");
    const size_t release = source.find("pRealToFree->Release();");
    ASSERT_NE(guard, std::string::npos);
    ASSERT_NE(release, std::string::npos);
    EXPECT_LT(guard, release);
}

TEST(InjectLifecycleSourceTest, ThirdPartyExecutorWaitsForLoaderQuiescenceBeforeSubsequentToolLoads) {
    const std::string source = ReadSource("hook/main_thirdparty_load.cpp");
    ASSERT_FALSE(source.empty());

    const size_t optiScalerEntry =
        source.find("{ce::third_party_load::Tool::kOptiScaler, &thirdParty.optiscalerDllPath},");
    const size_t specialKEntry =
        source.find("{ce::third_party_load::Tool::kSpecialK, &thirdParty.specialkDllPath},");
    const size_t quiescenceGate = source.find("ShouldWaitForLoaderQuiescenceBeforeToolLoad(toolIndex)");
    const size_t suspensionGate = source.find("ShouldSuspendPeerThreadsForToolLoad(toolIndex)");
    const size_t loadCall = source.find("LoadRuntimeDllViaOriginal(wide.c_str(), resolved.c_str())");
    ASSERT_NE(specialKEntry, std::string::npos);
    ASSERT_NE(optiScalerEntry, std::string::npos);
    ASSERT_NE(quiescenceGate, std::string::npos);
    ASSERT_NE(suspensionGate, std::string::npos);
    ASSERT_NE(loadCall, std::string::npos);
    EXPECT_LT(specialKEntry, optiScalerEntry);
    EXPECT_LT(quiescenceGate, loadCall);
    EXPECT_LT(suspensionGate, loadCall);
}

TEST(InjectLifecycleSourceTest, Dx11TempDeviceCreationBypassesEntryPatches) {
    const std::string source = ReadSource("hook/apis/dx11_hook.cpp");
    ASSERT_FALSE(source.empty());

    const size_t bypass = source.find("Bypassing entry patch on D3D11CreateDeviceAndSwapChain at %p");
    const size_t tempCreate = source.find("HRESULT hr = pTempCreate(");
    ASSERT_NE(bypass, std::string::npos);
    ASSERT_NE(tempCreate, std::string::npos);
    EXPECT_LT(bypass, tempCreate);
}
