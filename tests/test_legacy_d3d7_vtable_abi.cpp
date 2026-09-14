#define CINTERFACE 1
#define COBJMACROS 1

#ifndef DIRECTDRAW_VERSION
#define DIRECTDRAW_VERSION 0x0700
#endif

#ifndef DIRECT3D_VERSION
#define DIRECT3D_VERSION 0x0700
#endif

#include <windows.h>

#include <ddraw.h>

#include <d3d.h>

#include <gtest/gtest.h>

// The legacy Direct3D headers redefine enumerators that `d3d9.h` also defines,
// so `ddraw_hook_internal.h` cannot include them and reaches the application's
// IDirect3DDevice7 by vtable index instead. Those indices are ABI, and nothing
// in the hook build can check them - this translation unit is deliberately the
// only one that sees the real interface declaration.
TEST(LegacyD3D7VTableAbiTest, DeviceIndicesUsedByTheHookMatchTheInterface) {
    EXPECT_EQ(offsetof(IDirect3DDevice7Vtbl, GetRenderTarget) / sizeof(void*), 9u);
    EXPECT_EQ(offsetof(IDirect3DDevice7Vtbl, EndScene) / sizeof(void*), 6u);
    EXPECT_EQ(offsetof(IDirect3DDevice7Vtbl, SetRenderState) / sizeof(void*), 20u);
    EXPECT_EQ(offsetof(IDirect3DDevice7Vtbl, GetTextureStageState) / sizeof(void*), 36u);
    EXPECT_EQ(offsetof(IDirect3DDevice7Vtbl, SetTextureStageState) / sizeof(void*), 37u);
    EXPECT_EQ(offsetof(IDirect3DDevice7Vtbl, ApplyStateBlock) / sizeof(void*), 39u);
    EXPECT_EQ(offsetof(IDirect3DDevice7Vtbl, GetCaps) / sizeof(void*), 3u);
}

TEST(LegacyD3D7VTableAbiTest, Direct3D7CreateDeviceIndexMatchesTheInterface) {
    EXPECT_EQ(offsetof(IDirect3D7Vtbl, CreateDevice) / sizeof(void*), 4u);
}

TEST(LegacyD3D7VTableAbiTest, OverlayVertexFormatIsTransformedAndLit) {
    // The overlay hands DrawIndexedPrimitive user memory in this exact layout;
    // a mismatch would be read as garbage geometry rather than rejected.
    EXPECT_EQ(sizeof(D3DTLVERTEX), 32u);
    EXPECT_EQ(offsetof(D3DTLVERTEX, sx), 0u);
    EXPECT_EQ(offsetof(D3DTLVERTEX, sy), 4u);
    EXPECT_EQ(offsetof(D3DTLVERTEX, sz), 8u);
    EXPECT_EQ(offsetof(D3DTLVERTEX, rhw), 12u);
    EXPECT_EQ(offsetof(D3DTLVERTEX, color), 16u);
    EXPECT_EQ(offsetof(D3DTLVERTEX, specular), 20u);
    EXPECT_EQ(offsetof(D3DTLVERTEX, tu), 24u);
    EXPECT_EQ(offsetof(D3DTLVERTEX, tv), 28u);
    EXPECT_EQ(D3DFVF_TLVERTEX, (DWORD)(D3DFVF_XYZRHW | D3DFVF_DIFFUSE | D3DFVF_SPECULAR | D3DFVF_TEX1));
}

TEST(LegacyD3D7VTableAbiTest, OverlayColorConversionMatchesD3DCOLOR) {
    // The shared draw list stores ABGR; D3DCOLOR is ARGB. Red and blue swap,
    // alpha and green stay put.
    auto toD3DColor = [](uint32_t abgr) -> uint32_t {
        return (abgr & 0xFF00FF00u) | ((abgr & 0x00FF0000u) >> 16) | ((abgr & 0x000000FFu) << 16);
    };

    // Opaque red in ABGR is 0xFF0000FF and must become ARGB 0xFFFF0000.
    EXPECT_EQ(toD3DColor(0xFF0000FFu), 0xFFFF0000u);
    // Opaque blue in ABGR is 0xFFFF0000 and must become ARGB 0xFF0000FF.
    EXPECT_EQ(toD3DColor(0xFFFF0000u), 0xFF0000FFu);
    // Green and alpha are untouched.
    EXPECT_EQ(toD3DColor(0xFF00FF00u), 0xFF00FF00u);
    EXPECT_EQ(toD3DColor(0x8000FF00u), 0x8000FF00u);
    // The conversion is its own inverse.
    EXPECT_EQ(toD3DColor(toD3DColor(0x12345678u)), 0x12345678u);
}
