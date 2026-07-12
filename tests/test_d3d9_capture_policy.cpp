#include <gtest/gtest.h>

#include "../hook/common/d3d9_capture_policy.h"

TEST(D3D9CapturePolicyTest, ClassicDevicesAreNeverPromotedToEx) {
    EXPECT_FALSE(ShouldPromoteClassicD3D9Device());
}

TEST(D3D9CapturePolicyTest, AlphaBackbufferUsesExactSharedFormat) {
    const auto selection = SelectD3D9SharedCaptureFormat(D3DFMT_A8R8G8B8);
    ASSERT_TRUE(selection);
    EXPECT_EQ(selection.resourceFormat, D3DFMT_A8R8G8B8);
    EXPECT_EQ(selection.transportFormat, DXGI_FORMAT_B8G8R8A8_UNORM);
    EXPECT_FALSE(selection.requiresConversion);
}

TEST(D3D9CapturePolicyTest, XrgbBackbufferUsesShareableAlphaTransport) {
    const auto selection = SelectD3D9SharedCaptureFormat(D3DFMT_X8R8G8B8);
    ASSERT_TRUE(selection);
    EXPECT_EQ(selection.resourceFormat, D3DFMT_A8R8G8B8);
    EXPECT_EQ(selection.transportFormat, DXGI_FORMAT_B8G8R8A8_UNORM);
    EXPECT_TRUE(selection.requiresConversion);
}

TEST(D3D9CapturePolicyTest, TenBitBackbufferPreservesTenBitTransport) {
    const auto selection = SelectD3D9SharedCaptureFormat(D3DFMT_A2B10G10R10);
    ASSERT_TRUE(selection);
    EXPECT_EQ(selection.resourceFormat, D3DFMT_A2B10G10R10);
    EXPECT_EQ(selection.transportFormat, DXGI_FORMAT_R10G10B10A2_UNORM);
    EXPECT_FALSE(selection.requiresConversion);
}

TEST(D3D9CapturePolicyTest, UnsupportedBackbufferDoesNotGuessTransportLayout) {
    const auto selection = SelectD3D9SharedCaptureFormat(D3DFMT_R5G6B5);
    EXPECT_FALSE(selection);
    EXPECT_EQ(selection.resourceFormat, D3DFMT_UNKNOWN);
    EXPECT_EQ(selection.transportFormat, DXGI_FORMAT_UNKNOWN);
}
