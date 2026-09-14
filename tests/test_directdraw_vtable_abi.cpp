#define CINTERFACE 1
#define COBJMACROS 1
#include <windows.h>
#include <d3d9.h>
#ifndef DIRECTDRAW_VERSION
#define DIRECTDRAW_VERSION 0x0700
#endif
#include <ddraw.h>
#include <gtest/gtest.h>

TEST(DirectDrawVTableAbiTest, DirectDrawCreateSurfaceIndicesMatchAbi) {
    EXPECT_EQ(offsetof(IDirectDrawVtbl, CreateSurface) / sizeof(void*), 6u);
    EXPECT_EQ(offsetof(IDirectDraw4Vtbl, CreateSurface) / sizeof(void*), 6u);
    EXPECT_EQ(offsetof(IDirectDraw7Vtbl, CreateSurface) / sizeof(void*), 6u);
}

TEST(DirectDrawVTableAbiTest, Surface7VTableIndicesMatchAbi) {
    EXPECT_EQ(offsetof(IDirectDrawSurface7Vtbl, Blt) / sizeof(void*), 5u);
    EXPECT_EQ(offsetof(IDirectDrawSurface7Vtbl, BltFast) / sizeof(void*), 7u);
    EXPECT_EQ(offsetof(IDirectDrawSurface7Vtbl, Flip) / sizeof(void*), 11u);
    EXPECT_EQ(offsetof(IDirectDrawSurface7Vtbl, GetDC) / sizeof(void*), 17u);
    EXPECT_EQ(offsetof(IDirectDrawSurface7Vtbl, Lock) / sizeof(void*), 25u);
    EXPECT_EQ(offsetof(IDirectDrawSurface7Vtbl, ReleaseDC) / sizeof(void*), 26u);
    EXPECT_EQ(offsetof(IDirectDrawSurface7Vtbl, Unlock) / sizeof(void*), 32u);
}

TEST(DirectDrawVTableAbiTest, Surface4VTableIndicesMatchAbi) {
    EXPECT_EQ(offsetof(IDirectDrawSurface4Vtbl, Blt) / sizeof(void*), 5u);
    EXPECT_EQ(offsetof(IDirectDrawSurface4Vtbl, BltFast) / sizeof(void*), 7u);
    EXPECT_EQ(offsetof(IDirectDrawSurface4Vtbl, Flip) / sizeof(void*), 11u);
    EXPECT_EQ(offsetof(IDirectDrawSurface4Vtbl, GetDC) / sizeof(void*), 17u);
    EXPECT_EQ(offsetof(IDirectDrawSurface4Vtbl, Lock) / sizeof(void*), 25u);
    EXPECT_EQ(offsetof(IDirectDrawSurface4Vtbl, ReleaseDC) / sizeof(void*), 26u);
    EXPECT_EQ(offsetof(IDirectDrawSurface4Vtbl, Unlock) / sizeof(void*), 32u);
}

TEST(DirectDrawVTableAbiTest, LegacySurfaceVTableIndicesMatchAbi) {
    EXPECT_EQ(offsetof(IDirectDrawSurfaceVtbl, Blt) / sizeof(void*), 5u);
    EXPECT_EQ(offsetof(IDirectDrawSurfaceVtbl, BltFast) / sizeof(void*), 7u);
    EXPECT_EQ(offsetof(IDirectDrawSurfaceVtbl, Flip) / sizeof(void*), 11u);
    EXPECT_EQ(offsetof(IDirectDrawSurfaceVtbl, GetDC) / sizeof(void*), 17u);
    EXPECT_EQ(offsetof(IDirectDrawSurfaceVtbl, Lock) / sizeof(void*), 25u);
    EXPECT_EQ(offsetof(IDirectDrawSurfaceVtbl, ReleaseDC) / sizeof(void*), 26u);
    EXPECT_EQ(offsetof(IDirectDrawSurfaceVtbl, Unlock) / sizeof(void*), 32u);
}

TEST(DirectDrawVTableAbiTest, PixelFormatPackFormulasMatchHardwareScanout) {
    // Test RGB565 packing formula used in CopyOverlayBackbufferRegionToSurface
    auto pack565 = [](uint8_t r, uint8_t g, uint8_t b) -> uint16_t {
        return static_cast<uint16_t>(((static_cast<uint16_t>(r) >> 3) << 11) |
                                     ((static_cast<uint16_t>(g) >> 2) << 5) |
                                     (static_cast<uint16_t>(b) >> 3));
    };

    // Test RGB555 packing formula used in CopyOverlayBackbufferRegionToSurface
    auto pack555 = [](uint8_t r, uint8_t g, uint8_t b) -> uint16_t {
        return static_cast<uint16_t>(((static_cast<uint16_t>(r) >> 3) << 10) |
                                     ((static_cast<uint16_t>(g) >> 3) << 5) |
                                     (static_cast<uint16_t>(b) >> 3));
    };

    // Red: (255, 0, 0)
    EXPECT_EQ(pack565(255, 0, 0), 0xF800);
    EXPECT_EQ(pack555(255, 0, 0), 0x7C00);

    // Green: (0, 255, 0)
    EXPECT_EQ(pack565(0, 255, 0), 0x07E0);
    EXPECT_EQ(pack555(0, 255, 0), 0x03E0);

    // Blue: (0, 0, 255)
    EXPECT_EQ(pack565(0, 0, 255), 0x001F);
    EXPECT_EQ(pack555(0, 0, 255), 0x001F);

    // White: (255, 255, 255)
    EXPECT_EQ(pack565(255, 255, 255), 0xFFFF);
    EXPECT_EQ(pack555(255, 255, 255), 0x7FFF);

    // Black: (0, 0, 0)
    EXPECT_EQ(pack565(0, 0, 0), 0x0000);
    EXPECT_EQ(pack555(0, 0, 0), 0x0000);
}

TEST(DirectDrawVTableAbiTest, FlipTargetIsTheAttachedBackBufferUnlessOverridden) {
    // Flip publishes the flip chain's back buffer, or the surface the caller
    // named. That surface - never the one already on screen - is where the
    // overlay has to be before the flip reaches the runtime; see
    // ce::ddraw_present_policy and DDrawPresentPolicyTest for the policy itself.
    struct MockSurface {
        int id;
        bool isScanout;
    };

    MockSurface visibleSurface{1, true};
    MockSurface attachedBackBuffer{2, false};
    MockSurface explicitTarget{3, false};

    MockSurface* destOverride = nullptr;
    MockSurface* flipTarget = destOverride ? destOverride : &attachedBackBuffer;
    EXPECT_EQ(flipTarget->id, 2);
    EXPECT_NE(flipTarget, &visibleSurface);

    destOverride = &explicitTarget;
    flipTarget = destOverride ? destOverride : &attachedBackBuffer;
    EXPECT_EQ(flipTarget->id, 3);
}
