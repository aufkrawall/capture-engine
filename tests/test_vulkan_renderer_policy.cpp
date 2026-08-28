#include <gtest/gtest.h>

#include <filesystem>

#include "../hook/common/dxgi_shared.h"
#include "../hook/common/vulkan_dxgi_fifo_policy.h"
#include "../hook/common/vulkan_renderer_policy.h"
#include "source_fragment_reader.h"

namespace {

using ce::vulkan_renderer_policy::HasD3DUsageEvidence;
using ce::vulkan_renderer_policy::IsPublishedInheritedRenderer;
using ce::vulkan_renderer_policy::ShouldBootstrapD3D9Hooks;
using ce::vulkan_renderer_policy::ShouldApplyProcessLocalRuntimeOverrides;
using ce::vulkan_renderer_policy::ShouldInstallEarlyD3DDXGIHooks;
using ce::vulkan_renderer_policy::ShouldEnableVulkanLayerForProfile;
using ce::vulkan_renderer_policy::ShouldPublishHookAsSource;
using ce::vulkan_renderer_policy::ShouldSuppressSpeculativeDX12Bootstrap;
using ce::vulkan_renderer_policy::ShouldTreatVulkanAsActiveRenderer;
using ce::vulkan_dxgi_fifo_policy::ApplyFinalDxgiFifoParameters;
using ce::vulkan_dxgi_fifo_policy::ShouldArmFinalDxgiPresent;
using ce::vulkan_dxgi_fifo_policy::ShouldForceFinalDxgiFifo;

// RAII restore for the process-global present-path flag.
class ScopedVulkanPresentFlag {
public:
    ScopedVulkanPresentFlag() : previous_(DXGIShared::IsVulkanActive()) {}
    ~ScopedVulkanPresentFlag() { DXGIShared::SetVulkanActiveForDXGIPresentPath(previous_); }

private:
    bool previous_;
};

} // namespace

TEST(VulkanRendererPolicyTest, Dx12Ue5WithVulkanLoaderIsNotVulkanActive) {
    // RoboCop: Rogue City (UE5, DX12) loads vulkan-1.dll as a transitive
    // dependency. d3d12.dll presence is D3D usage evidence, so the DXGI
    // present/overlay path must stay enabled (regression for the session
    // where the overlay never rendered because the present path latched
    // "Vulkan active" from module presence alone).
    const bool d3dEvidence = HasD3DUsageEvidence(
        /*dxvkD3D11WrapperLoaded=*/false,
        /*d3d12DeviceCreated=*/false,
        /*d3d11Or10DeviceCreated=*/false,
        /*legacyD3DLoaded=*/false,
        /*d3d12DllPresent=*/true,
        /*d3d11DllPresent=*/false);
    EXPECT_TRUE(d3dEvidence);
    EXPECT_FALSE(ShouldTreatVulkanAsActiveRenderer(/*vulkanModuleLoaded=*/true,
                                                   /*vulkanLayerOwned=*/false, d3dEvidence));
}

TEST(VulkanRendererPolicyTest, D3d11RuntimePresenceIsD3DEvidenceForDx11Titles) {
    const bool d3dEvidence = HasD3DUsageEvidence(
        false, false, false, false, false, /*d3d11DllPresent=*/true);
    EXPECT_TRUE(d3dEvidence);
    EXPECT_FALSE(ShouldTreatVulkanAsActiveRenderer(true, false, d3dEvidence));
}

TEST(VulkanRendererPolicyTest, PureVulkanWithoutD3DEvidenceIsVulkanActive) {
    const bool noD3dEvidence = HasD3DUsageEvidence(false, false, false, false, false, false);
    EXPECT_FALSE(noD3dEvidence);
    EXPECT_TRUE(ShouldTreatVulkanAsActiveRenderer(true, false, noD3dEvidence));
}

TEST(VulkanRendererPolicyTest, VulkanLayerOwnershipWinsOverD3DEvidence) {
    // A Vulkan-layer-owned session keeps DXGI pass-through even if a D3D
    // module is loaded in the same process.
    const bool d3dEvidence = HasD3DUsageEvidence(false, true, false, false, true, false);
    EXPECT_TRUE(d3dEvidence);
    EXPECT_TRUE(ShouldTreatVulkanAsActiveRenderer(false, true, d3dEvidence));
}

TEST(VulkanRendererPolicyTest, DxvkD3d11IsVulkanBackedWithoutRealD3D12Device) {
    // DXVK's d3d11.dll is a Vulkan front-end: d3d11.dll presence alone is not
    // D3D evidence, so the Vulkan layer keeps ownership.
    const bool d3dEvidence = HasD3DUsageEvidence(
        /*dxvkD3D11WrapperLoaded=*/true,
        /*d3d12DeviceCreated=*/false,
        /*d3d11Or10DeviceCreated=*/false,
        /*legacyD3DLoaded=*/false,
        /*d3d12DllPresent=*/false,
        /*d3d11DllPresent=*/true);
    EXPECT_FALSE(d3dEvidence);
    EXPECT_TRUE(ShouldTreatVulkanAsActiveRenderer(true, false, d3dEvidence));
}

TEST(VulkanRendererPolicyTest, DxvkWithRealD3D12DeviceIsD3DEvidence) {
    const bool d3dEvidence = HasD3DUsageEvidence(
        /*dxvkD3D11WrapperLoaded=*/true,
        /*d3d12DeviceCreated=*/true,
        /*d3d11Or10DeviceCreated=*/false,
        /*legacyD3DLoaded=*/false,
        /*d3d12DllPresent=*/false,
        /*d3d11DllPresent=*/true);
    EXPECT_TRUE(d3dEvidence);
    EXPECT_FALSE(ShouldTreatVulkanAsActiveRenderer(true, false, d3dEvidence));
}

TEST(VulkanRendererPolicyTest, DeviceCreationAndLegacyModulesAreD3DEvidence) {
    EXPECT_TRUE(HasD3DUsageEvidence(false, /*d3d12DeviceCreated=*/true, false, false, false, false));
    EXPECT_TRUE(HasD3DUsageEvidence(false, false, /*d3d11Or10DeviceCreated=*/true, false, false, false));
    EXPECT_TRUE(HasD3DUsageEvidence(false, false, false, /*legacyD3DLoaded=*/true, false, false));
}

TEST(VulkanRendererPolicyTest, NoVulkanModuleMeansNotVulkanActive) {
    const bool noD3dEvidence = HasD3DUsageEvidence(false, false, false, false, false, false);
    EXPECT_FALSE(ShouldTreatVulkanAsActiveRenderer(false, false, noD3dEvidence));
}

TEST(VulkanRendererPolicyTest, ResidentCaptureLayerSuppressesSpeculativeEarlyD3DHooks) {
    EXPECT_FALSE(ShouldInstallEarlyD3DDXGIHooks(/*vulkanLayerModuleLoaded=*/true));
    EXPECT_TRUE(ShouldInstallEarlyD3DDXGIHooks(/*vulkanLayerModuleLoaded=*/false));
}

TEST(VulkanRendererPolicyTest, FinalDxgiPresentArmsOnlyForResidentVulkanFifo) {
    EXPECT_TRUE(ShouldArmFinalDxgiPresent(true, "fifo"));
    EXPECT_FALSE(ShouldArmFinalDxgiPresent(false, "fifo"));
    EXPECT_FALSE(ShouldArmFinalDxgiPresent(true, "adaptive"));
    EXPECT_FALSE(ShouldArmFinalDxgiPresent(true, "mailbox"));
    EXPECT_FALSE(ShouldArmFinalDxgiPresent(true, "off"));
}

TEST(VulkanRendererPolicyTest, FinalDxgiFifoRequiresLiveOwnershipAndLifecycle) {
    EXPECT_TRUE(ShouldForceFinalDxgiFifo(true, true, false, "fifo"));
    EXPECT_FALSE(ShouldForceFinalDxgiFifo(false, true, false, "fifo"));
    EXPECT_FALSE(ShouldForceFinalDxgiFifo(true, false, false, "fifo"));
    EXPECT_FALSE(ShouldForceFinalDxgiFifo(true, true, true, "fifo"));
    EXPECT_FALSE(ShouldForceFinalDxgiFifo(true, true, false, "adaptive"));
}

TEST(VulkanRendererPolicyTest, FinalDxgiFifoUsesVblankAndForbidsTearing) {
    uint32_t syncInterval = 0;
    uint32_t flags = 0x200u | 0x4u;
    EXPECT_TRUE(ApplyFinalDxgiFifoParameters(true, syncInterval, flags));
    EXPECT_EQ(syncInterval, 1u);
    EXPECT_EQ(flags, 0x4u);

    EXPECT_FALSE(ApplyFinalDxgiFifoParameters(true, syncInterval, flags));
    syncInterval = 0;
    flags = 0x200u;
    EXPECT_FALSE(ApplyFinalDxgiFifoParameters(false, syncInterval, flags));
    EXPECT_EQ(syncInterval, 0u);
    EXPECT_EQ(flags, 0x200u);
}

TEST(VulkanRendererPolicySourceTest, FinalDxgiFifoPathIsPresentOnlyAndNonPacing) {
    namespace fs = std::filesystem;
    const std::string hookThread =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "main_hookthread.cpp");
    const std::string factories =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "wrappers" / "wrapper_hooks.cpp");
    const std::string install =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "main_install.cpp");
    const std::string finalPresent = ce::test_source::ReadFile(
        fs::current_path() / "hook" / "wrappers" / "vulkan_dxgi_fifo_present.cpp");
    ASSERT_FALSE(hookThread.empty());
    ASSERT_FALSE(factories.empty());
    ASSERT_FALSE(install.empty());
    ASSERT_FALSE(finalPresent.empty());

    const size_t configLoaded = hookThread.find("GetActiveGraphicsConfig();");
    const size_t registerFactory =
        hookThread.find("RegisterDynamicFactoryHooks(vulkanLayerModuleLoaded)");
    const size_t armRouter = hookThread.find("InitializeGetProcAddressHook();");
    ASSERT_NE(configLoaded, std::string::npos);
    ASSERT_NE(registerFactory, std::string::npos);
    ASSERT_NE(armRouter, std::string::npos);
    EXPECT_LT(configLoaded, registerFactory);
    EXPECT_LT(registerFactory, armRouter);

    EXPECT_NE(factories.find("MaybeInstallFactoryHooks"), std::string::npos);
    EXPECT_NE(install.find("RegisterDynamicFactoryHooks(vulkanLayerModuleLoaded)"),
              std::string::npos);
    EXPECT_NE(finalPresent.find("QueryInterface(IID_PPV_ARGS(&factory0))"),
              std::string::npos);
    EXPECT_NE(finalPresent.find("QueryInterface(IID_PPV_ARGS(&factory2))"),
              std::string::npos);
    EXPECT_NE(finalPresent.find("vtable, 10"), std::string::npos);
    EXPECT_NE(finalPresent.find("vtable, 15"), std::string::npos);
    EXPECT_NE(finalPresent.find("vtable, 16"), std::string::npos);
    EXPECT_NE(finalPresent.find("vtable, 24"), std::string::npos);
    EXPECT_NE(finalPresent.find("vtable, 8"), std::string::npos);
    EXPECT_NE(finalPresent.find("vtable, 22"), std::string::npos);
    EXPECT_NE(finalPresent.find("original(swapchain, syncInterval, flags)"),
              std::string::npos);
    EXPECT_NE(finalPresent.find(
                  "original(swapchain, syncInterval, flags, parameters)"),
              std::string::npos);

    EXPECT_EQ(finalPresent.find("WaitForSingleObject"), std::string::npos);
    EXPECT_EQ(finalPresent.find("SetMaximumFrameLatency"), std::string::npos);
    EXPECT_EQ(finalPresent.find("FRAME_LATENCY_WAITABLE_OBJECT"), std::string::npos);
    EXPECT_EQ(finalPresent.find("FpsLimiter"), std::string::npos);
    EXPECT_EQ(finalPresent.find("Sleep("), std::string::npos);
    EXPECT_EQ(finalPresent.find("Portal"), std::string::npos);
}

TEST(VulkanRendererPolicyTest, D3D9BootstrapRejectsVulkanAndTranslationRuntimes) {
    EXPECT_TRUE(ShouldBootstrapD3D9Hooks(false, false, false, false, false, true));
    EXPECT_FALSE(ShouldBootstrapD3D9Hooks(/*vulkanActive=*/true, false, false, false, false, true));
    EXPECT_FALSE(ShouldBootstrapD3D9Hooks(false, /*nonSystemD3D9Runtime=*/true, false, false, false, true));
    EXPECT_FALSE(ShouldBootstrapD3D9Hooks(false, false, /*hookAlreadyInstalled=*/true, false, false, true));
    EXPECT_FALSE(ShouldBootstrapD3D9Hooks(false, false, false, /*d3d12ActuallyUsed=*/true, false, true));
    EXPECT_FALSE(ShouldBootstrapD3D9Hooks(false, false, false, false, /*d3d11DllLoaded=*/true, true));
    EXPECT_FALSE(ShouldBootstrapD3D9Hooks(false, false, false, false, false, /*d3d9DllLoaded=*/false));
}

TEST(VulkanRendererPolicyTest, TranslationRuntimeSuppressesOnlyUnprovenDX12Bootstrap) {
    EXPECT_TRUE(ShouldSuppressSpeculativeDX12Bootstrap(false, /*nonSystemD3D11Runtime=*/true, false));
    EXPECT_TRUE(ShouldSuppressSpeculativeDX12Bootstrap(false, false, /*nonSystemD3D9Runtime=*/true));
    EXPECT_FALSE(ShouldSuppressSpeculativeDX12Bootstrap(false, false, false));
    EXPECT_FALSE(ShouldSuppressSpeculativeDX12Bootstrap(/*d3d12DeviceCreated=*/true, true, true));
}

TEST(VulkanRendererPolicyTest, DirectChildRendererInheritsActiveProfileEligibility) {
    EXPECT_TRUE(ShouldEnableVulkanLayerForProfile(
        /*currentProcessWhitelisted=*/false, /*currentParentPid=*/42,
        /*activeSourcePid=*/42, /*profileTargetPid=*/0,
        /*parentProcessWhitelisted=*/true));
    EXPECT_TRUE(ShouldEnableVulkanLayerForProfile(
        /*currentProcessWhitelisted=*/false, /*currentParentPid=*/42,
        /*activeSourcePid=*/0, /*profileTargetPid=*/42,
        /*parentProcessWhitelisted=*/true));
    EXPECT_TRUE(ShouldEnableVulkanLayerForProfile(
        /*currentProcessWhitelisted=*/true, 0, 0, 0, false));
}

TEST(VulkanRendererPolicyTest, VulkanEligibilityInheritanceFailsClosed) {
    EXPECT_FALSE(ShouldEnableVulkanLayerForProfile(false, 0, 42, 0, true));
    EXPECT_FALSE(ShouldEnableVulkanLayerForProfile(false, 41, 42, 0, true));
    EXPECT_FALSE(ShouldEnableVulkanLayerForProfile(false, 41, 0, 42, true));
    EXPECT_FALSE(ShouldEnableVulkanLayerForProfile(false, 42, 42, 42,
                                                   /*parentProcessWhitelisted=*/false));
}

TEST(VulkanRendererPolicyTest, RendererHookEligibilityIsExactAndPreservesParentSource) {
    EXPECT_TRUE(IsPublishedInheritedRenderer(42, 42));
    EXPECT_FALSE(IsPublishedInheritedRenderer(0, 0));
    EXPECT_FALSE(IsPublishedInheritedRenderer(41, 42));

    EXPECT_FALSE(ShouldPublishHookAsSource(/*inheritedRenderer=*/true));
    EXPECT_TRUE(ShouldPublishHookAsSource(/*inheritedRenderer=*/false));
}

TEST(VulkanRendererPolicyTest, ProcessLocalOverridesMoveOnlyAfterRendererPublication) {
    EXPECT_TRUE(ShouldApplyProcessLocalRuntimeOverrides(41, 0));
    EXPECT_TRUE(ShouldApplyProcessLocalRuntimeOverrides(42, 42));
    EXPECT_FALSE(ShouldApplyProcessLocalRuntimeOverrides(41, 42));
}

TEST(VulkanRendererPolicyTest, SharedFlagFollowsPublishedDecision) {
    ScopedVulkanPresentFlag restore;
    EXPECT_FALSE(DXGIShared::IsVulkanActive());
    DXGIShared::SetVulkanActiveForDXGIPresentPath(true);
    EXPECT_TRUE(DXGIShared::IsVulkanActive());
    EXPECT_TRUE(DXGIShared::ShouldBypassSwapchainCreateForVulkan("unit test"));
    DXGIShared::SetVulkanActiveForDXGIPresentPath(false);
    EXPECT_FALSE(DXGIShared::IsVulkanActive());
    EXPECT_FALSE(DXGIShared::ShouldBypassSwapchainCreateForVulkan("unit test"));
}

TEST(VulkanRendererPolicySourceTest, HookInstallPublishesTheSharedDecision) {
    namespace fs = std::filesystem;
    const fs::path installSource = fs::current_path() / "hook" / "main_install.cpp";
    ASSERT_TRUE(fs::exists(installSource));
    const std::string source = ce::test_source::ReadFile(installSource);
    ASSERT_FALSE(source.empty());

    const size_t decision = source.find("shouldTreatVulkanActive");
    const size_t publish =
        source.find("DXGIShared::SetVulkanActiveForDXGIPresentPath(s_vulkanActive)");
    ASSERT_NE(decision, std::string::npos);
    ASSERT_NE(publish, std::string::npos);
    EXPECT_LT(decision, publish);
}

TEST(VulkanRendererPolicySourceTest, VulkanOwnershipSuppressesEarlyAndResidualD3DHooks) {
    namespace fs = std::filesystem;
    const std::string dllMain = ce::test_source::ReadFile(fs::current_path() / "hook" / "main_dllmain.cpp");
    const std::string hookThread = ce::test_source::ReadFile(fs::current_path() / "hook" / "main_hookthread.cpp");
    const std::string create =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "apis" / "dx12_hook_swapchain_create.cpp");
    const std::string deep =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "apis" / "dx12_hook_swapchain_tracking.cpp");
    const std::string ecl =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "apis" / "dx12_hook_ecl_install.cpp");
    const std::string dx11 =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "apis" / "dx11_hook_detours.cpp");
    const std::string wrappers =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "wrappers" / "dxgi_factory_wrap.cpp");
    const std::string install =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "main_install.cpp");
    ASSERT_FALSE(dllMain.empty());
    ASSERT_FALSE(hookThread.empty());
    ASSERT_FALSE(create.empty());
    ASSERT_FALSE(deep.empty());
    ASSERT_FALSE(ecl.empty());
    ASSERT_FALSE(dx11.empty());
    ASSERT_FALSE(wrappers.empty());
    ASSERT_FALSE(install.empty());

    EXPECT_NE(dllMain.find("ShouldInstallEarlyD3DDXGIHooks"), std::string::npos);
    const size_t earlyInstall = hookThread.find("InstallGlobalVTableHooks();");
    const size_t earlyPolicy = hookThread.find("ShouldInstallEarlyD3DDXGIHooks");
    ASSERT_NE(earlyInstall, std::string::npos);
    ASSERT_NE(earlyPolicy, std::string::npos);
    EXPECT_LT(earlyPolicy, earlyInstall);
    EXPECT_NE(create.find("ShouldBypassSwapchainCreateForVulkan"), std::string::npos);
    EXPECT_NE(deep.find("ShouldBypassSwapchainCreateForVulkan"), std::string::npos);
    EXPECT_NE(ecl.find("ShouldBypassSwapchainCreateForVulkan"), std::string::npos);
    EXPECT_NE(dx11.find("ShouldBypassSwapchainCreateForVulkan"), std::string::npos);
    EXPECT_NE(wrappers.find("ShouldBypassSwapchainCreateForVulkan"), std::string::npos);
    EXPECT_NE(install.find("if (!s_vulkanActive && !g_DX8Hook"), std::string::npos);
    EXPECT_NE(install.find("if (!s_vulkanActive && !g_OpenGLHook"), std::string::npos);
}

TEST(VulkanRendererPolicySourceTest, VulkanLayerOwnsTranslatedD3D9FinalPresentation) {
    namespace fs = std::filesystem;
    const std::string install = ce::test_source::ReadFile(fs::current_path() / "hook" / "main_install.cpp");
    const std::string present =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "vulkan_layer" / "vulkan_layer_present.cpp");
    const std::string dx9Helpers =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "apis" / "dx9_hook_helpers.cpp");
    const std::string dx9Wrapper =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "wrappers" / "d3d9_device_wrap.cpp");
    ASSERT_FALSE(install.empty());
    ASSERT_FALSE(present.empty());
    ASSERT_FALSE(dx9Helpers.empty());
    ASSERT_FALSE(dx9Wrapper.empty());

    EXPECT_NE(install.find("ShouldBootstrapD3D9Hooks"), std::string::npos);
    EXPECT_NE(install.find("ShouldSuppressSpeculativeDX12Bootstrap"), std::string::npos);
    EXPECT_EQ(present.find("preferDX9Path"), std::string::npos);
    EXPECT_EQ(present.find("skipping Vulkan present-time overlay"), std::string::npos);
    EXPECT_EQ(dx9Helpers.find("keeping DX9 present path active"), std::string::npos);
    EXPECT_NE(dx9Wrapper.find("ShouldSkipDX9PresentForVulkan"), std::string::npos);
}

TEST(VulkanRendererPolicySourceTest, LayerEligibilityFollowsOnlyThePublishedWhitelistedParent) {
    namespace fs = std::filesystem;
    const std::string ipc =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "vulkan_layer" / "layer_ipc.cpp");
    const std::string main =
        ce::test_source::ReadFile(fs::current_path() / "hook" / "vulkan_layer" / "layer_main.cpp");
    const std::string injectHost =
        ce::test_source::ReadFile(fs::current_path() / "captureengine" / "inject_main.cpp");
    ASSERT_FALSE(ipc.empty());
    ASSERT_FALSE(main.empty());
    ASSERT_FALSE(injectHost.empty());

    EXPECT_NE(ipc.find("CreateToolhelp32Snapshot"), std::string::npos);
    EXPECT_NE(ipc.find("sharedMemory->GetSourcePid()"), std::string::npos);
    EXPECT_NE(ipc.find("info->GetProfileTargetPid()"), std::string::npos);
    EXPECT_NE(ipc.find("IsProcessNameWhitelisted(info, parentName)"), std::string::npos);
    EXPECT_NE(ipc.find("ShouldEnableVulkanLayerForProfile"), std::string::npos);
    EXPECT_NE(main.find("LayerIPC_IsProcessEligibleByCurrentHost"), std::string::npos);
    const size_t publishTarget = injectHost.find("SetProfileTargetPid(targetPid)");
    const size_t publishConfig = injectHost.find("PublishResolvedConfigForTarget(pSharedMem, processName");
    ASSERT_NE(publishTarget, std::string::npos);
    ASSERT_NE(publishConfig, std::string::npos);
    EXPECT_LT(publishTarget, publishConfig);
    EXPECT_EQ(ipc.find("NvRemixBridge"), std::string::npos);
    EXPECT_EQ(main.find("NvRemixBridge"), std::string::npos);
    EXPECT_EQ(injectHost.find("NvRemixBridge"), std::string::npos);
}

TEST(VulkanRendererPolicySourceTest, InheritedRendererBootstrapsRuntimeOverridesBeforeVulkan) {
    namespace fs = std::filesystem;
    const fs::path root = fs::current_path();
    const std::string layerIpc =
        ce::test_source::ReadFile(root / "hook" / "vulkan_layer" / "layer_ipc.cpp");
    const std::string layerBootstrap = ce::test_source::ReadFile(
        root / "hook" / "vulkan_layer" / "layer_renderer_bootstrap.cpp");
    const std::string hookBootstrap =
        ce::test_source::ReadFile(root / "hook" / "main_renderer_bootstrap.cpp");
    const std::string hookThread =
        ce::test_source::ReadFile(root / "hook" / "main_hookthread.cpp");
    ASSERT_FALSE(layerIpc.empty());
    ASSERT_FALSE(layerBootstrap.empty());
    ASSERT_FALSE(hookBootstrap.empty());
    ASSERT_FALSE(hookThread.empty());

    const size_t publish = layerIpc.find("inheritedRendererProcessPid.store");
    const size_t bootstrap = layerIpc.find("LayerBootstrapInheritedRendererHook()", publish);
    ASSERT_NE(publish, std::string::npos);
    ASSERT_NE(bootstrap, std::string::npos);
    EXPECT_LT(publish, bootstrap);
    EXPECT_NE(layerBootstrap.find("capture_hook_x64.dll"), std::string::npos);
    EXPECT_NE(layerBootstrap.find("CE_WaitForInheritedRendererBootstrap"), std::string::npos);
    EXPECT_NE(layerBootstrap.find("LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR"), std::string::npos);
    EXPECT_NE(hookBootstrap.find("shared.dlssSrDllPath"), std::string::npos);
    EXPECT_NE(hookBootstrap.find("shared.streamlineDllPath"), std::string::npos);
    EXPECT_NE(hookBootstrap.find("shared.dlssDebugOverlay"), std::string::npos);
    EXPECT_NE(hookThread.find("CompleteInheritedRendererBootstrap(true)"), std::string::npos);
    EXPECT_NE(hookThread.find("ShouldPublishHookAsSource"), std::string::npos);

    EXPECT_EQ(layerIpc.find("NvRemixBridge"), std::string::npos);
    EXPECT_EQ(layerBootstrap.find("NvRemixBridge"), std::string::npos);
    EXPECT_EQ(hookBootstrap.find("NvRemixBridge"), std::string::npos);
}

// Late injection wakes the resident Vulkan layer only after CaptureEngine
// signals its per-PID reactivation event, which lands after the hook thread has
// already weighed D3D evidence. If the "not Vulkan" decision latched
// permanently, the DXGI present/resize path would keep doing CE work for the
// rest of the process while the layer owns presentation. Layer ownership — and
// only layer ownership — must re-open the evaluation; re-opening on plain
// vulkan-1.dll presence would restore the RoboCop DX12 regression.
TEST(VulkanRendererPolicySourceTest, LayerOwnershipReopensTheLatchedVulkanDecision) {
    namespace fs = std::filesystem;
    const fs::path installSource = fs::current_path() / "hook" / "main_install.cpp";
    ASSERT_TRUE(fs::exists(installSource));
    const std::string source = ce::test_source::ReadFile(installSource);
    ASSERT_FALSE(source.empty());

    const size_t ownershipRead = source.find("vulkanLayerOwned =");
    const size_t gate = source.find("if (!s_checkedForVulkan || s_vulkanActive || vulkanLayerOwned)");
    ASSERT_NE(ownershipRead, std::string::npos);
    ASSERT_NE(gate, std::string::npos)
        << "the latch must re-open on established Vulkan layer ownership";
    // Ownership has to be sampled before the gate, otherwise the gate can only
    // ever observe the previous tick's value.
    EXPECT_LT(ownershipRead, gate);

    // Module presence must stay inside the gate, never a re-entry condition.
    const size_t moduleProbe = source.find("GetModuleHandleW(L\"vulkan-1.dll\")");
    ASSERT_NE(moduleProbe, std::string::npos);
    EXPECT_LT(gate, moduleProbe);
}

TEST(VulkanRendererPolicySourceTest, PresentGateConsultsSharedFlagNotModulePresence) {
    namespace fs = std::filesystem;
    const fs::path sharedSource = fs::current_path() / "hook" / "common" / "dxgi_shared.cpp";
    ASSERT_TRUE(fs::exists(sharedSource));
    const std::string source = ce::test_source::ReadFile(sharedSource);
    ASSERT_FALSE(source.empty());

    // The one-shot module-presence latch is gone; IsVulkanActive reads the
    // evidence-based flag published by CheckAndInstallHooks.
    EXPECT_NE(source.find("bool IsVulkanActive()"), std::string::npos);
    EXPECT_NE(source.find("s_dxgiVulkanActive.load"), std::string::npos);
    EXPECT_EQ(source.find("GetModuleHandleW(L\"vulkan-1.dll\")"), std::string::npos);

    const fs::path routingSource =
        fs::current_path() / "hook" / "common" / "dxgi_shared_present_routing.cpp";
    ASSERT_TRUE(fs::exists(routingSource));
    const std::string routing = ce::test_source::ReadFile(routingSource);
    ASSERT_FALSE(routing.empty());
    EXPECT_NE(routing.find("if (IsVulkanActive())"), std::string::npos);

    const fs::path dx12MainSource = fs::current_path() / "hook" / "apis" / "dx12_hook_main.cpp";
    ASSERT_TRUE(fs::exists(dx12MainSource));
    const std::string dx12Main = ce::test_source::ReadFile(dx12MainSource);
    ASSERT_FALSE(dx12Main.empty());
    EXPECT_NE(dx12Main.find("if (DXGIShared::IsVulkanActive())"), std::string::npos);
    EXPECT_EQ(dx12Main.find("GetModuleHandleW(L\"vulkan-1.dll\")"), std::string::npos);

    const fs::path dx11Source = fs::current_path() / "hook" / "apis" / "dx11_hook.cpp";
    ASSERT_TRUE(fs::exists(dx11Source));
    const std::string dx11 = ce::test_source::ReadFile(dx11Source);
    ASSERT_FALSE(dx11.empty());
    EXPECT_NE(dx11.find("if (DXGIShared::IsVulkanActive())"), std::string::npos);
    EXPECT_EQ(dx11.find("GetModuleHandleW(L\"vulkan-1.dll\")"), std::string::npos);
}

TEST(VulkanRendererPolicySourceTest, FifoIsAppliedBeforeStreamlineDlssgSeesSwapchainCreation) {
    namespace fs = std::filesystem;
    const fs::path installSource = fs::current_path() / "hook" / "apis" / "streamline_hook_install.cpp";
    const fs::path layerSource = fs::current_path() / "hook" / "vulkan_layer" / "vulkan_layer_present.cpp";
    ASSERT_TRUE(fs::exists(installSource));
    ASSERT_TRUE(fs::exists(layerSource));

    const std::string install = ce::test_source::ReadFile(installSource);
    const std::string layer = ce::test_source::ReadFile(layerSource);
    ASSERT_FALSE(install.empty());
    ASSERT_FALSE(layer.empty());

    EXPECT_NE(install.find("Hooked_Streamline_vkCreateSwapchainKHR"), std::string::npos);
    EXPECT_NE(install.find("GetProcAddress(module, \"vkCreateSwapchainKHR\")"), std::string::npos);
    EXPECT_NE(install.find("ResolveVulkanFifoPresentModeOverride"), std::string::npos);
    EXPECT_NE(install.find("before Streamline DLSS-G hooks"), std::string::npos);
    EXPECT_NE(install.find("InstallInlineHookOnce("), std::string::npos);
    EXPECT_NE(layer.find("modifiedCI.presentMode = desiredMode"), std::string::npos)
        << "the Streamline proxy hook and the downstream Vulkan layer are both required";
    EXPECT_NE(layer.find("driver returned: %d (presentMode=%d"), std::string::npos)
        << "runtime logs must prove which mode reached the downstream Vulkan call";
}
