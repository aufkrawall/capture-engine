#define CINTERFACE 1
#define COBJMACROS 1

#include <windows.h>

#include <d3d8.h>

#include <gtest/gtest.h>

#include "../common/mip_mapping_policy.h"

// D3D8 is wrapped through locally declared ABI-compatible interfaces so the
// production hook can coexist with the D3D9 SDK headers. Keep every numeric
// vtable and texture-stage constant checked against the real D3D8 declaration.
TEST(LegacyD3D8VTableAbiTest, IndicesUsedByTheHookMatchTheInterfaces) {
    EXPECT_EQ(offsetof(IDirect3D8Vtbl, CreateDevice) / sizeof(void*), 15u);
    EXPECT_EQ(offsetof(IDirect3DDevice8Vtbl, GetDeviceCaps) / sizeof(void*), 7u);
    EXPECT_EQ(offsetof(IDirect3DDevice8Vtbl, Reset) / sizeof(void*), 14u);
    EXPECT_EQ(offsetof(IDirect3DDevice8Vtbl, Present) / sizeof(void*), 15u);
    EXPECT_EQ(offsetof(IDirect3DDevice8Vtbl, ApplyStateBlock) / sizeof(void*), 54u);
    EXPECT_EQ(offsetof(IDirect3DDevice8Vtbl, GetTextureStageState) / sizeof(void*), 62u);
    EXPECT_EQ(offsetof(IDirect3DDevice8Vtbl, SetTextureStageState) / sizeof(void*), 63u);
    EXPECT_EQ(offsetof(D3DCAPS8, MaxAnisotropy), 108u);
    EXPECT_EQ(sizeof(D3DCAPS8), 212u);
}

TEST(LegacyD3D8VTableAbiTest, TextureStageEnumsMatchTheHookConstants) {
    EXPECT_EQ(static_cast<DWORD>(D3DTSS_ADDRESSU), 13u);
    EXPECT_EQ(static_cast<DWORD>(D3DTSS_ADDRESSV), 14u);
    EXPECT_EQ(static_cast<DWORD>(D3DTSS_ADDRESSW), 25u);
    EXPECT_EQ(static_cast<DWORD>(D3DTSS_MAGFILTER), 16u);
    EXPECT_EQ(static_cast<DWORD>(D3DTSS_MINFILTER), 17u);
    EXPECT_EQ(static_cast<DWORD>(D3DTSS_MIPFILTER), 18u);
    EXPECT_EQ(static_cast<DWORD>(D3DTSS_MIPMAPLODBIAS), 19u);
    EXPECT_EQ(static_cast<DWORD>(D3DTSS_MAXMIPLEVEL), 20u);
    EXPECT_EQ(static_cast<DWORD>(D3DTSS_MAXANISOTROPY), 21u);
    EXPECT_EQ(static_cast<DWORD>(D3DTEXF_NONE), 0u);
    EXPECT_EQ(static_cast<DWORD>(D3DTEXF_POINT), 1u);
    EXPECT_EQ(static_cast<DWORD>(D3DTEXF_LINEAR), 2u);
    EXPECT_EQ(static_cast<DWORD>(D3DTEXF_ANISOTROPIC), 3u);
}

TEST(LegacyD3D8VTableAbiTest, TextureFilterEnumsProduceBilinearAndTrilinearMappings) {
    constexpr DWORD point = D3DTEXF_POINT;
    constexpr DWORD linear = D3DTEXF_LINEAR;
    DWORD mag = point;
    DWORD min = point;
    DWORD mip = linear;

    ce::mip_mapping::ApplyDiscreteFilters(ce::mip_mapping::Mode::Bilinear, point, linear, point, linear,
                                          point, linear, mag, min, mip);
    EXPECT_EQ(mag, linear);
    EXPECT_EQ(min, linear);
    EXPECT_EQ(mip, point);

    ce::mip_mapping::ApplyDiscreteFilters(ce::mip_mapping::Mode::Trilinear, point, linear, point, linear,
                                          point, linear, mag, min, mip);
    EXPECT_EQ(mag, linear);
    EXPECT_EQ(min, linear);
    EXPECT_EQ(mip, linear);
}
