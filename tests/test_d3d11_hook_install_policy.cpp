#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "hook/present/vulkan_renderer_policy.h"
#include "source_fragment_reader.h"

namespace {

using ce::vulkan_renderer_policy::ShouldInstallD3D11Hooks;

TEST(D3D11HookInstallPolicyTest, NativeLegacyRendererSuppressesSpeculativeDX11Hooks) {
    // In a native legacy game (e.g. D3D9 Borderlands 2 or DirectDraw title),
    // CE loads d3d11.dll on frame 1 for internal shared-texture capture interop.
    // DX11Hook::Init() must NOT be triggered unless an actual D3D11/10 device was created.
    EXPECT_FALSE(ShouldInstallD3D11Hooks(
        /*vulkanActive=*/false,
        /*hookAlreadyInstalled=*/false,
        /*d3d11Or10DllPresent=*/true,
        /*d3d11Or10DeviceCreated=*/false,
        /*d3d12DeviceCreated=*/false,
        /*confirmedNativeLegacyRendererActive=*/true));
}

TEST(D3D11HookInstallPolicyTest, LegacyRendererWithConfirmedD3D11DeviceInstallsHooks) {
    // If a game initially used D3D9 for an intro and then created a D3D11 device,
    // D3D11 hooks must be installed.
    EXPECT_TRUE(ShouldInstallD3D11Hooks(
        /*vulkanActive=*/false,
        /*hookAlreadyInstalled=*/false,
        /*d3d11Or10DllPresent=*/true,
        /*d3d11Or10DeviceCreated=*/true,
        /*d3d12DeviceCreated=*/false,
        /*confirmedNativeLegacyRendererActive=*/true));
}

TEST(D3D11HookInstallPolicyTest, StandardD3D11GameInstallsHooks) {
    EXPECT_TRUE(ShouldInstallD3D11Hooks(
        /*vulkanActive=*/false,
        /*hookAlreadyInstalled=*/false,
        /*d3d11Or10DllPresent=*/true,
        /*d3d11Or10DeviceCreated=*/false,
        /*d3d12DeviceCreated=*/false,
        /*confirmedNativeLegacyRendererActive=*/false));
}

TEST(D3D11HookInstallPolicyTest, VulkanOrAlreadyInstalledSkipsInstall) {
    EXPECT_FALSE(ShouldInstallD3D11Hooks(
        /*vulkanActive=*/true,
        /*hookAlreadyInstalled=*/false,
        /*d3d11Or10DllPresent=*/true,
        /*d3d11Or10DeviceCreated=*/true,
        /*d3d12DeviceCreated=*/false,
        /*confirmedNativeLegacyRendererActive=*/false));

    EXPECT_FALSE(ShouldInstallD3D11Hooks(
        /*vulkanActive=*/false,
        /*hookAlreadyInstalled=*/true,
        /*d3d11Or10DllPresent=*/true,
        /*d3d11Or10DeviceCreated=*/true,
        /*d3d12DeviceCreated=*/false,
        /*confirmedNativeLegacyRendererActive=*/false));
}

TEST(D3D11HookInstallPolicyTest, D3D12WithoutD3D11DeviceSkipsInstall) {
    EXPECT_FALSE(ShouldInstallD3D11Hooks(
        /*vulkanActive=*/false,
        /*hookAlreadyInstalled=*/false,
        /*d3d11Or10DllPresent=*/true,
        /*d3d11Or10DeviceCreated=*/false,
        /*d3d12DeviceCreated=*/true,
        /*confirmedNativeLegacyRendererActive=*/false));
}

TEST(D3D11HookInstallPolicySourceTest, MainInstallEvaluatesLegacyRendererActivity) {
    namespace fs = std::filesystem;
    const fs::path installPath = fs::current_path() / "hook" / "runtime" / "main_install.cpp";
    ASSERT_TRUE(fs::exists(installPath));

    const std::string source = ce::test_source::ReadFile(installPath);
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("confirmedNativeLegacyRendererActive ="), std::string::npos);
    EXPECT_NE(source.find("WasGameD3D9DeviceCreated()"), std::string::npos);
    EXPECT_NE(source.find("WasDirectDrawInterfaceObserved()"), std::string::npos);
    EXPECT_NE(source.find("ce::vulkan_renderer_policy::ShouldInstallD3D11Hooks("), std::string::npos);
}

}  // namespace
