#include <gtest/gtest.h>

#include "../hook/common/dxgi_shared.h"

TEST(DXGISharedTest, ExternalPresentDetourPathRequiresBypassSupport) {
    EXPECT_TRUE(DXGIShared::CanSafelyInstallExternalPresentDetourPath(false, false));
    EXPECT_TRUE(DXGIShared::CanSafelyInstallExternalPresentDetourPath(false, true));
    EXPECT_FALSE(DXGIShared::CanSafelyInstallExternalPresentDetourPath(true, false));
    EXPECT_TRUE(DXGIShared::CanSafelyInstallExternalPresentDetourPath(true, true));
}

TEST(DXGISharedTest, SelectTranslatedGraphicsAPINamePrefersDXVKD3D11OverDXVKD3D9) {
    EXPECT_STREQ(DXGIShared::SelectTranslatedGraphicsAPIName(true, true, false, false), "DX11 (DXVK)");
    EXPECT_STREQ(DXGIShared::SelectTranslatedGraphicsAPIName(true, true, false, true), "DX10 (DXVK)");
    EXPECT_STREQ(DXGIShared::SelectTranslatedGraphicsAPIName(false, true, false, false), "DX9 (DXVK)");
    EXPECT_STREQ(DXGIShared::SelectTranslatedGraphicsAPIName(false, false, true, false), "DX12 (VKD3D-Proton)");
    EXPECT_STREQ(DXGIShared::SelectTranslatedGraphicsAPIName(false, false, false, false), "Vulkan");
}

TEST(DXGISharedTest, SelectPrimarySwapChainAPITypePrefersHighestDeviceVersion) {
    EXPECT_EQ(DXGIShared::APIType::D3D12, DXGIShared::SelectPrimarySwapChainAPIType(true, true, true));
    EXPECT_EQ(DXGIShared::APIType::D3D11, DXGIShared::SelectPrimarySwapChainAPIType(false, true, true));
    EXPECT_EQ(DXGIShared::APIType::D3D10, DXGIShared::SelectPrimarySwapChainAPIType(false, false, true));
    EXPECT_EQ(DXGIShared::APIType::Unknown, DXGIShared::SelectPrimarySwapChainAPIType(false, false, false));
}
