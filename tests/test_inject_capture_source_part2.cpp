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
    const size_t inlineInstall = presentHooks.find("InlineHook::InstallPublishedBatch", externalChain);
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
    EXPECT_NE(deepRemove.find("UnstableSnapshotPolicy::kAcceptSuspendedSet"), std::string::npos);
    EXPECT_EQ(deepHook.find("pPatch[0] = 0xCC"), std::string::npos);
}

TEST(InjectLifecycleSourceTest, RelatedInlineHooksShareOnePeerThreadQuiescenceTransaction) {
    const std::string inlineHook = ReadSource("hook/wrappers/inline_hook.cpp");
    const std::string inlineHookBatch = ReadSource("hook/wrappers/inline_hook_batch.cpp");
    const std::string fatalHooks = ReadSource("hook/main_fatal_hooks.cpp");
    const std::string openGL = ReadSource("hook/apis/opengl_hook_install.cpp");
    const std::string presentHooks = ReadSource("hook/common/dxgi_shared_hooks_present.cpp");
    const std::string dlssIndicator = ReadSource("hook/common/dlss_indicator_spoof.cpp");
    const std::string streamlineBatch = ReadSource("hook/apis/streamline_inline_hook_batch.cpp");
    const std::string streamlineInstall = ReadSource("hook/apis/streamline_hook_install.cpp");
    const std::string nvngx = ReadSource("hook/apis/nvngx_hook_feature.cpp");
    ASSERT_FALSE(inlineHook.empty());
    ASSERT_FALSE(inlineHookBatch.empty());
    ASSERT_FALSE(fatalHooks.empty());
    ASSERT_FALSE(openGL.empty());
    ASSERT_FALSE(presentHooks.empty());
    ASSERT_FALSE(dlssIndicator.empty());
    ASSERT_FALSE(streamlineBatch.empty());
    ASSERT_FALSE(streamlineInstall.empty());
    ASSERT_FALSE(nvngx.empty());

    const size_t prepare = inlineHookBatch.find("PreparePublishedHookLocked");
    const size_t quiesce = inlineHookBatch.find("ThreadQuiescence groupQuiescence", prepare);
    const size_t rangeCheck = inlineHookBatch.find("groupQuiescence.IsRangeSafe", quiesce);
    const size_t groupedCommit = inlineHookBatch.find("CommitPreparedEntryPatchQuiescedLocked", rangeCheck);
    ASSERT_NE(prepare, std::string::npos);
    ASSERT_NE(quiesce, std::string::npos);
    ASSERT_NE(rangeCheck, std::string::npos);
    ASSERT_NE(groupedCommit, std::string::npos);
    EXPECT_LT(prepare, quiesce) << "decoding/allocation/publication must finish before peer suspension";
    EXPECT_LT(quiesce, rangeCheck);
    EXPECT_LT(rangeCheck, groupedCommit);
    EXPECT_NE(inlineHook.find("WriteOwnedEntryPatchQuiesced"), std::string::npos);
    EXPECT_NE(fatalHooks.find("InlineHook::InstallPublishedBatch"), std::string::npos);
    EXPECT_NE(openGL.find("InlineHook::InstallPublishedBatch"), std::string::npos);
    EXPECT_NE(presentHooks.find("InlineHook::InstallPublishedBatch"), std::string::npos);
    EXPECT_NE(dlssIndicator.find("InlineHook::InstallPublishedBatch"), std::string::npos);
    EXPECT_NE(streamlineBatch.find("InlineHook::InstallPublishedBatch"), std::string::npos);
    EXPECT_NE(streamlineInstall.find("coreHookBatch.Commit()"), std::string::npos);
    EXPECT_NE(nvngx.find("InlineHook::InstallPublishedBatch"), std::string::npos);
    EXPECT_NE(nvngx.find("publication->hookForExport[i] == publication->hookIndex"), std::string::npos);
    EXPECT_NE(nvngx.find("InterlockedExchangePointer", nvngx.find("const auto publishTrampoline")),
              std::string::npos);
    EXPECT_EQ(inlineHookBatch.find("InstallBatchScope"), std::string::npos);
    EXPECT_NE(inlineHookBatch.find("ThreadQuiescence fallbackQuiescence"), std::string::npos);
    EXPECT_NE(inlineHookBatch.find("UnstableSnapshotPolicy::kAcceptSuspendedSet"), std::string::npos);
    EXPECT_NE(inlineHook.find("ExecuteWithQuiescenceFallback"), std::string::npos);
    EXPECT_NE(inlineHook.find("UnstableSnapshotPolicy::kAcceptSuspendedSet"), std::string::npos);

    const std::string hookTx = ReadSource("hook/wrappers/hook_patch_transaction.h");
    EXPECT_NE(hookTx.find("explicit ThreadQuiescence(UnstableSnapshotPolicy unstablePolicy"), std::string::npos);
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

TEST(InjectLifecycleSourceTest, GraphicsModuleObserverPrecedesDiagnosticEntryHooks) {
    const std::string hookThread = ReadSource("hook/main_hookthread.cpp");
    ASSERT_FALSE(hookThread.empty());

    const size_t runtimePreload = hookThread.find("PreloadConfiguredGraphicsRuntimeDlls();");
    const size_t loaderObserver = hookThread.find("InlineHook::InstallPublished(pLdrLoadDll");
    const size_t fatalHooks = hookThread.find("TryInstallFatalTerminationDumpHooks();");
    ASSERT_NE(runtimePreload, std::string::npos);
    ASSERT_NE(loaderObserver, std::string::npos);
    ASSERT_NE(fatalHooks, std::string::npos);
    EXPECT_LT(runtimePreload, loaderObserver);
    EXPECT_LT(loaderObserver, fatalHooks);
}

TEST(InjectLifecycleSourceTest, AgilityBootstrapEvidenceSuppressesSpeculativeLegacyHooks) {
    const std::string samplerHooks = ReadSource("hook/apis/dx12_sampler_hooks.cpp");
    const std::string wrapperState = ReadSource("hook/wrappers/wrapper_hooks.cpp");
    const std::string install = ReadSource("hook/main_install.cpp");
    ASSERT_FALSE(samplerHooks.empty());
    ASSERT_FALSE(wrapperState.empty());
    ASSERT_FALSE(install.empty());

    const size_t getInterface = samplerHooks.find("HRESULT WINAPI DetourD3D12GetInterface");
    const size_t bootstrapMark = samplerHooks.find("MarkD3D12RuntimeBootstrapObserved()", getInterface);
    const size_t sdkFactoryHook = samplerHooks.find("DetourSDKCreateDeviceFactory");
    const size_t sdkBootstrapMark = samplerHooks.find("MarkD3D12RuntimeBootstrapObserved()", sdkFactoryHook);
    const size_t sdkFactoryCall = samplerHooks.find("original(configuration, sdkVersion", sdkFactoryHook);
    const size_t factoryCreate = samplerHooks.find("HRESULT STDMETHODCALLTYPE DetourFactoryCreateDevice");
    const size_t deviceMark = samplerHooks.find("MarkD3D12DeviceCreated();", factoryCreate);
    const size_t deviceHook = samplerHooks.find("DX12_HookDeviceVTable(baseDevice);", factoryCreate);
    const size_t samplerHook = samplerHooks.find("bool HookDevice(ID3D12Device* device)");
    const size_t overrideGate = samplerHooks.find("HasSamplerOverride(GetActiveGraphicsConfig())", samplerHook);
    const size_t samplerVtableRead = samplerHooks.find("void** vtable", samplerHook);
    ASSERT_NE(bootstrapMark, std::string::npos);
    ASSERT_NE(sdkFactoryHook, std::string::npos);
    ASSERT_NE(sdkBootstrapMark, std::string::npos);
    ASSERT_NE(sdkFactoryCall, std::string::npos);
    ASSERT_NE(deviceMark, std::string::npos);
    ASSERT_NE(deviceHook, std::string::npos);
    ASSERT_NE(overrideGate, std::string::npos);
    ASSERT_NE(samplerVtableRead, std::string::npos);
    EXPECT_LT(sdkBootstrapMark, sdkFactoryCall);
    EXPECT_LT(deviceMark, deviceHook);
    EXPECT_LT(overrideGate, samplerVtableRead);
    EXPECT_NE(samplerHooks.find("HookSDKConfiguration(sdkConfiguration1)"), std::string::npos);
    EXPECT_NE(samplerHooks.find("reinterpret_cast<void*>(&vtable[4])"), std::string::npos);
    EXPECT_NE(samplerHooks.find("if (hasDeviceFactory && MarkD3D12RuntimeBootstrapObserved())"), std::string::npos);
    EXPECT_NE(wrapperState.find("bool HasD3D12RuntimeUseEvidence()"), std::string::npos);
    EXPECT_NE(install.find("d3d12UseEvidence = HasD3D12RuntimeUseEvidence();"), std::string::npos);
    EXPECT_NE(install.find("OpenGL hooks skipped: D3D12 runtime-use evidence"), std::string::npos);
}

TEST(InjectLifecycleSourceTest, LateAttachPresentDiscoveryUsesWarpInsteadOfTheGameHardwareAdapter) {
    const std::string install = ReadSource("hook/apis/dx12_hook_hook_install.cpp");
    const std::string hookMain = ReadSource("hook/apis/dx12_hook_main.cpp");
    const std::string samplerHooks = ReadSource("hook/apis/dx12_sampler_hooks.cpp");
    ASSERT_FALSE(install.empty());
    ASSERT_FALSE(hookMain.empty());
    ASSERT_FALSE(samplerHooks.empty());

    const size_t tempBootstrap = install.find("void HookSwapchainVTableViaTempSwapchain(");
    const size_t internalScope = install.find("DX12_BeginInternalDXGISwapchainProbe();", tempBootstrap);
    const size_t deviceEntryBypass =
        install.find("Bypassing foreign entry patch on D3D12CreateDevice", tempBootstrap);
    const size_t warpAdapter = install.find("EnumWarpAdapter", tempBootstrap);
    const size_t deviceCreate = install.find("pD3D12CreateDevice(pWarpAdapter", warpAdapter);
    const size_t queueCreate = install.find("pDevice->CreateCommandQueue", deviceCreate);
    ASSERT_NE(tempBootstrap, std::string::npos);
    ASSERT_NE(internalScope, std::string::npos);
    ASSERT_NE(deviceEntryBypass, std::string::npos);
    ASSERT_NE(warpAdapter, std::string::npos);
    ASSERT_NE(deviceCreate, std::string::npos);
    ASSERT_NE(queueCreate, std::string::npos);
    EXPECT_LT(internalScope, warpAdapter);
    EXPECT_LT(deviceEntryBypass, internalScope);
    EXPECT_LT(warpAdapter, deviceCreate);
    EXPECT_LT(deviceCreate, queueCreate);
    EXPECT_EQ(install.find("pD3D12CreateDevice(nullptr", tempBootstrap), std::string::npos);
    EXPECT_EQ(install.find("DX12_HookDeviceVTable(pDevice)", tempBootstrap), std::string::npos);
    EXPECT_EQ(install.find("dx12_hook_g_CreatingTempSwapchain", tempBootstrap), std::string::npos);
    EXPECT_NE(install.find("WARP D3D12 device created; synthetic hardware-adapter creation remains"),
              std::string::npos);

    const size_t rawCreate = samplerHooks.find("HRESULT WINAPI DetourD3D12CreateDeviceRaw");
    const size_t internalProbe = samplerHooks.find("const bool internalProbe = DX12_IsInternalDXGISwapchainProbe()",
                                                   rawCreate);
    const size_t applicationOnly = samplerHooks.find("if (!internalProbe)", internalProbe);
    const size_t deviceHook = samplerHooks.find("DX12_HookDeviceVTable(baseDevice);", applicationOnly);
    ASSERT_NE(internalProbe, std::string::npos);
    ASSERT_NE(applicationOnly, std::string::npos);
    ASSERT_NE(deviceHook, std::string::npos);
    EXPECT_LT(applicationOnly, deviceHook);

    const size_t guardedAttempt =
        hookMain.find("TryInstallPresentHooksViaGuardedTempSwapchain(\"postponed deferral\")");
    const size_t guardedSuccess = hookMain.find("DXGIShared::HasPresentInlineHooks()", guardedAttempt);
    const size_t clearDeferred =
        hookMain.find("dx12_hook_g_EarlyPresentHookInstallDeferred.store(false", guardedSuccess);
    const size_t successReturn = hookMain.find("return;", guardedSuccess);
    ASSERT_NE(guardedAttempt, std::string::npos);
    ASSERT_NE(guardedSuccess, std::string::npos);
    ASSERT_NE(clearDeferred, std::string::npos);
    ASSERT_NE(successReturn, std::string::npos);
    EXPECT_LT(clearDeferred, successReturn);
    EXPECT_NE(hookMain.find("Deferred Present hooks installed through the guarded system-DXGI/WARP bootstrap"),
              std::string::npos);
}

TEST(InjectLifecycleSourceTest, StableDX12OverlayDiagnosticsAvoidPerFrameNoOpSpam) {
    const std::string overlay = ReadSource("hook/common/custom_overlay_dx12.cpp");
    const std::string ownerQueue = ReadSource("hook/apis/dx12_hook_ffx_owner_queue.cpp");
    ASSERT_FALSE(overlay.empty());
    ASSERT_FALSE(ownerQueue.empty());

    EXPECT_EQ(overlay.find("No deferred upload needed"), std::string::npos);
    EXPECT_NE(ownerQueue.find("ResolveSwapchainOutputHDRState(proxy, desc.BufferDesc.Format, nullptr"),
              std::string::npos);
    EXPECT_NE(ownerQueue.find("formatChanged || colorSpaceChanged || hdrChanged || supportedChanged"),
              std::string::npos);
    EXPECT_NE(ownerQueue.find("ShouldLogCadence(probeCount, 3, 600)"), std::string::npos);
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
    // The logical layer source, not one sibling unit: the acquire wrappers moved
    // to vulkan_layer_swapchain.cpp when vkAcquireNextImage2KHR joined them, and
    // this invariant is about the entry points, not about which unit holds them.
    const std::string vulkan = ReadSource("hook/vulkan_layer/vulkan_layer.cpp");
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

    const size_t acquire = vulkan.find("VKAPI_ATTR VkResult VKAPI_CALL Capture_vkAcquireNextImageKHR(");
    const size_t acquireDormant = vulkan.find("!g_LayerState.whitelisted.load", acquire);
    const size_t acquireTracking = vulkan.find("BeginAcquireBoundary(", acquire);
    ASSERT_NE(acquire, std::string::npos);
    ASSERT_NE(acquireDormant, std::string::npos);
    ASSERT_NE(acquireTracking, std::string::npos);
    EXPECT_LT(acquireDormant, acquireTracking);

    // The device-group acquire is the same mutation-sensitive entry point and
    // maintains the same per-image acquire generation, so it forwards dormant
    // before it touches CE's swapchain bookkeeping too.
    const size_t acquire2 = vulkan.find("VKAPI_ATTR VkResult VKAPI_CALL Capture_vkAcquireNextImage2KHR(");
    const size_t acquire2Dormant = vulkan.find("!g_LayerState.whitelisted.load", acquire2);
    const size_t acquire2Tracking = vulkan.find("BeginAcquireBoundary(", acquire2);
    ASSERT_NE(acquire2, std::string::npos);
    ASSERT_NE(acquire2Dormant, std::string::npos);
    ASSERT_NE(acquire2Tracking, std::string::npos);
    EXPECT_LT(acquire2Dormant, acquire2Tracking);

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

TEST(InjectLifecycleSourceTest, HousekeepingThreadDoesNotRaiseProcessSchedulingPressure) {
    const std::string dllMain = ReadSource("hook/main_dllmain.cpp");
    const std::string internal = ReadSource("hook/main_internal.h");
    const std::string hookThread = ReadSource("hook/main_hookthread.cpp");
    ASSERT_FALSE(dllMain.empty());
    ASSERT_FALSE(internal.empty());
    ASSERT_FALSE(hookThread.empty());

    EXPECT_EQ(dllMain.find("SetThreadPriority(hThread"), std::string::npos);
    EXPECT_EQ(dllMain.find("THREAD_PRIORITY_HIGHEST"), std::string::npos);
    EXPECT_EQ(internal.find("timeBeginPeriod(1)"), std::string::npos);
    EXPECT_NE(hookThread.find("[HookThreadStages]"), std::string::npos);
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
    const size_t probeScope = source.find("ScopedInternalDXGISwapchainProbe probeScope", bypass);
    const size_t tempCreate = source.find("hr = pTempCreate(", probeScope);
    ASSERT_NE(bypass, std::string::npos);
    ASSERT_NE(probeScope, std::string::npos);
    ASSERT_NE(tempCreate, std::string::npos);
    EXPECT_LT(bypass, probeScope);
    EXPECT_LT(probeScope, tempCreate);
}
