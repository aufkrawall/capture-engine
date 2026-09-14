#define CINTERFACE 1
#define COBJMACROS 1
#include <windows.h>
#include <d3d9.h>
#include <gtest/gtest.h>

TEST(D3D9VTableAbiTest, DeviceExVTableIndicesMatchAbi) {
    EXPECT_EQ(offsetof(IDirect3DDevice9ExVtbl, Present) / sizeof(void*), 17u);
    EXPECT_EQ(offsetof(IDirect3DDevice9ExVtbl, PresentEx) / sizeof(void*), 121u);
    EXPECT_EQ(offsetof(IDirect3DDevice9ExVtbl, ResetEx) / sizeof(void*), 132u);
}

TEST(D3D9VTableAbiTest, SwapChainVTableIndicesMatchAbi) {
    EXPECT_EQ(offsetof(IDirect3DSwapChain9Vtbl, Present) / sizeof(void*), 3u);
}

TEST(D3D9VTableAbiTest, Rgb565And555PixelConversionFormulas) {
    auto to565 = [](uint32_t c) -> uint16_t {
        return static_cast<uint16_t>(((c >> 8) & 0xF800) | ((c >> 5) & 0x07E0) | ((c >> 3) & 0x001F));
    };
    auto to555 = [](uint32_t c) -> uint16_t {
        return static_cast<uint16_t>(((c >> 9) & 0x7C00) | ((c >> 6) & 0x03E0) | ((c >> 3) & 0x001F));
    };

    // Red: 0x00FF0000
    EXPECT_EQ(to565(0x00FF0000), 0xF800);
    EXPECT_EQ(to555(0x00FF0000), 0x7C00);

    // Green: 0x0000FF00
    EXPECT_EQ(to565(0x0000FF00), 0x07E0);
    EXPECT_EQ(to555(0x0000FF00), 0x03E0);

    // Blue: 0x000000FF
    EXPECT_EQ(to565(0x000000FF), 0x001F);
    EXPECT_EQ(to555(0x000000FF), 0x001F);

    // White: 0x00FFFFFF
    EXPECT_EQ(to565(0x00FFFFFF), 0xFFFF);
    EXPECT_EQ(to555(0x00FFFFFF), 0x7FFF);

    // Black: 0x00000000
    EXPECT_EQ(to565(0x00000000), 0x0000);
    EXPECT_EQ(to555(0x00000000), 0x0000);
}
