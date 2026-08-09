#include <gtest/gtest.h>

#include <filesystem>

#include "../hook/common/dxgi_shared.h"
#include "../hook/common/vulkan_renderer_policy.h"
#include "source_fragment_reader.h"

namespace {

using ce::vulkan_renderer_policy::HasD3DUsageEvidence;
using ce::vulkan_renderer_policy::ShouldTreatVulkanAsActiveRenderer;

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

TEST(VulkanRendererPolicyTest, SharedFlagFollowsPublishedDecision) {
    ScopedVulkanPresentFlag restore;
    EXPECT_FALSE(DXGIShared::IsVulkanActive());
    DXGIShared::SetVulkanActiveForDXGIPresentPath(true);
    EXPECT_TRUE(DXGIShared::IsVulkanActive());
    DXGIShared::SetVulkanActiveForDXGIPresentPath(false);
    EXPECT_FALSE(DXGIShared::IsVulkanActive());
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
