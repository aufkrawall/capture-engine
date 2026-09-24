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

#include "../common/mip_mapping_policy.h"

// The legacy Direct3D headers redefine enumerators that `d3d9.h` also defines,
// so `ddraw_hook_internal.h` cannot include them and reaches the application's
// IDirect3DDevice7 by vtable index instead. Those indices are ABI, and nothing
// in the hook build can check them - this translation unit is deliberately the
// only one that sees the real interface declaration.
TEST(LegacyD3D7VTableAbiTest, DeviceIndicesUsedByTheHookMatchTheInterface) {
    EXPECT_EQ(offsetof(IDirect3DDevice7Vtbl, Release) / sizeof(void*), 2u);
    EXPECT_EQ(offsetof(IDirect3DDevice7Vtbl, GetRenderTarget) / sizeof(void*), 9u);
    EXPECT_EQ(offsetof(IDirect3DDevice7Vtbl, EndScene) / sizeof(void*), 6u);
    EXPECT_EQ(offsetof(IDirect3DDevice7Vtbl, SetRenderState) / sizeof(void*), 20u);
    EXPECT_EQ(offsetof(IDirect3DDevice7Vtbl, GetTextureStageState) / sizeof(void*), 36u);
    EXPECT_EQ(offsetof(IDirect3DDevice7Vtbl, SetTextureStageState) / sizeof(void*), 37u);
    EXPECT_EQ(offsetof(IDirect3DDevice7Vtbl, SetTexture) / sizeof(void*), 35u);
    EXPECT_EQ(offsetof(IDirect3DDevice7Vtbl, ApplyStateBlock) / sizeof(void*), 39u);
    EXPECT_EQ(offsetof(IDirect3DDevice7Vtbl, BeginStateBlock) / sizeof(void*), 22u);
    EXPECT_EQ(offsetof(IDirect3DDevice7Vtbl, EndStateBlock) / sizeof(void*), 23u);
    EXPECT_EQ(offsetof(IDirect3DDevice7Vtbl, CaptureStateBlock) / sizeof(void*), 40u);
    EXPECT_EQ(offsetof(IDirect3DDevice7Vtbl, DeleteStateBlock) / sizeof(void*), 41u);
    EXPECT_EQ(offsetof(IDirect3DDevice7Vtbl, CreateStateBlock) / sizeof(void*), 42u);
    // legacy_d3d_state_block_policy.h relies on these values.
    EXPECT_EQ(static_cast<DWORD>(D3DSBT_ALL), 1u);
    EXPECT_EQ(static_cast<DWORD>(D3DSBT_PIXELSTATE), 2u);
    EXPECT_EQ(static_cast<DWORD>(D3DSBT_VERTEXSTATE), 3u);
    EXPECT_EQ(offsetof(IDirect3DDevice7Vtbl, GetCaps) / sizeof(void*), 3u);
}

TEST(LegacyD3D7VTableAbiTest, Direct3D7CreateDeviceIndexMatchesTheInterface) {
    EXPECT_EQ(offsetof(IDirect3D7Vtbl, CreateDevice) / sizeof(void*), 4u);
}

TEST(LegacyD3D7VTableAbiTest, Direct3D6IndicesUsedByTheHookMatchTheInterfaces) {
    EXPECT_EQ(offsetof(IDirect3D3Vtbl, CreateDevice) / sizeof(void*), 8u);
    EXPECT_EQ(offsetof(IDirect3DDevice3Vtbl, EndScene) / sizeof(void*), 10u);
    EXPECT_EQ(offsetof(IDirect3DDevice3Vtbl, GetTextureStageState) / sizeof(void*), 39u);
    EXPECT_EQ(offsetof(IDirect3DDevice3Vtbl, SetTextureStageState) / sizeof(void*), 40u);
}

TEST(LegacyD3D7VTableAbiTest, LegacyFilterEnumsProduceNearestBilinearAndTrilinearMappings) {
    EXPECT_EQ(static_cast<DWORD>(D3DTSS_ADDRESS), 12u);
    EXPECT_EQ(static_cast<DWORD>(D3DTSS_ADDRESSU), 13u);
    EXPECT_EQ(static_cast<DWORD>(D3DTSS_ADDRESSV), 14u);
    EXPECT_EQ(static_cast<DWORD>(D3DTSS_MAGFILTER), 16u);
    EXPECT_EQ(static_cast<DWORD>(D3DTSS_MINFILTER), 17u);
    EXPECT_EQ(static_cast<DWORD>(D3DTSS_MIPFILTER), 18u);
    EXPECT_EQ(static_cast<DWORD>(D3DTSS_MIPMAPLODBIAS), 19u);
    EXPECT_EQ(static_cast<DWORD>(D3DTSS_MAXMIPLEVEL), 20u);
    EXPECT_EQ(static_cast<DWORD>(D3DTSS_MAXANISOTROPY), 21u);
    EXPECT_EQ(static_cast<DWORD>(D3DTFG_POINT), 1u);
    EXPECT_EQ(static_cast<DWORD>(D3DTFG_LINEAR), 2u);
    EXPECT_EQ(static_cast<DWORD>(D3DTFG_ANISOTROPIC), 5u);
    EXPECT_EQ(static_cast<DWORD>(D3DTFN_POINT), 1u);
    EXPECT_EQ(static_cast<DWORD>(D3DTFN_LINEAR), 2u);
    EXPECT_EQ(static_cast<DWORD>(D3DTFN_ANISOTROPIC), 3u);
    EXPECT_EQ(static_cast<DWORD>(D3DTFP_NONE), 1u);
    EXPECT_EQ(static_cast<DWORD>(D3DTFP_POINT), 2u);
    EXPECT_EQ(static_cast<DWORD>(D3DTFP_LINEAR), 3u);

    constexpr DWORD pointMag = D3DTFG_POINT;
    constexpr DWORD linearMag = D3DTFG_LINEAR;
    constexpr DWORD pointMin = D3DTFN_POINT;
    constexpr DWORD linearMin = D3DTFN_LINEAR;
    constexpr DWORD pointMip = D3DTFP_POINT;
    constexpr DWORD linearMip = D3DTFP_LINEAR;
    DWORD mag = linearMag;
    DWORD min = linearMin;
    DWORD mip = linearMip;
    ce::mip_mapping::ApplyDiscreteFilters(ce::mip_mapping::Mode::Nearest, pointMag, linearMag,
                                          pointMin, linearMin, pointMip, linearMip,
                                          mag, min, mip);
    EXPECT_EQ(mag, pointMag);
    EXPECT_EQ(min, pointMin);
    EXPECT_EQ(mip, pointMip);

    ce::mip_mapping::ApplyDiscreteFilters(ce::mip_mapping::Mode::Bilinear, pointMag, linearMag,
                                          pointMin, linearMin, pointMip, linearMip,
                                          mag, min, mip);
    EXPECT_EQ(mag, linearMag);
    EXPECT_EQ(min, linearMin);
    EXPECT_EQ(mip, pointMip);

    ce::mip_mapping::ApplyDiscreteFilters(ce::mip_mapping::Mode::Trilinear, pointMag, linearMag,
                                          pointMin, linearMin, pointMip, linearMip,
                                          mag, min, mip);
    EXPECT_EQ(mag, linearMag);
    EXPECT_EQ(min, linearMin);
    EXPECT_EQ(mip, linearMip);
}

TEST(LegacyD3D7VTableAbiTest, DirectDrawPresentationIndicesMatchEveryUsedInterface) {
    EXPECT_EQ(offsetof(IDirectDrawVtbl, WaitForVerticalBlank) / sizeof(void*), 22u);
    EXPECT_EQ(offsetof(IDirectDraw2Vtbl, WaitForVerticalBlank) / sizeof(void*), 22u);
    EXPECT_EQ(offsetof(IDirectDraw3Vtbl, WaitForVerticalBlank) / sizeof(void*), 22u);
    EXPECT_EQ(offsetof(IDirectDraw4Vtbl, WaitForVerticalBlank) / sizeof(void*), 22u);
    EXPECT_EQ(offsetof(IDirectDraw7Vtbl, WaitForVerticalBlank) / sizeof(void*), 22u);
    EXPECT_EQ(offsetof(IDirectDrawSurfaceVtbl, GetBltStatus) / sizeof(void*), 13u);
    EXPECT_EQ(offsetof(IDirectDrawSurfaceVtbl, GetFlipStatus) / sizeof(void*), 18u);
    EXPECT_EQ(offsetof(IDirectDrawSurface4Vtbl, GetBltStatus) / sizeof(void*), 13u);
    EXPECT_EQ(offsetof(IDirectDrawSurface4Vtbl, GetFlipStatus) / sizeof(void*), 18u);
    EXPECT_EQ(offsetof(IDirectDrawSurface4Vtbl, GetDDInterface) / sizeof(void*), 36u);
    EXPECT_EQ(offsetof(IDirectDrawSurface7Vtbl, GetBltStatus) / sizeof(void*), 13u);
    EXPECT_EQ(offsetof(IDirectDrawSurface7Vtbl, GetFlipStatus) / sizeof(void*), 18u);
    EXPECT_EQ(offsetof(IDirectDrawSurface7Vtbl, GetDDInterface) / sizeof(void*), 36u);
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
