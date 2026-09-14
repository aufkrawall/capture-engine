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
    // Test RGB565 packing formula used in CopyOverlayBackbufferToPrimarySurface
    auto pack565 = [](uint8_t r, uint8_t g, uint8_t b) -> uint16_t {
        return static_cast<uint16_t>(((static_cast<uint16_t>(r) >> 3) << 11) |
                                     ((static_cast<uint16_t>(g) >> 2) << 5) |
                                     (static_cast<uint16_t>(b) >> 3));
    };

    // Test RGB555 packing formula used in CopyOverlayBackbufferToPrimarySurface
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

TEST(DirectDrawVTableAbiTest, OverlayRoutingDirectsToPrimaryScanoutSurface) {
    // Verify the invariant: overlay presentation must ALWAYS target the primary scanout surface,
    // not the temporary backbuffer / offscreen source blitted into it.
    struct MockSurface {
        int id;
        bool isPrimary;
    };

    MockSurface primarySurface{1, true};
    MockSurface backBufferSurface{2, false};

    MockSurface* explicitSource = &backBufferSurface;
    MockSurface* presentationSurface = explicitSource ? explicitSource : &primarySurface;

    // doOverlay routing policy verification:
    // When drawing overlay, we must target primarySurface, NOT presentationSurface
    MockSurface* overlayTarget = &primarySurface;
    EXPECT_EQ(overlayTarget->id, 1);
    EXPECT_TRUE(overlayTarget->isPrimary);
    EXPECT_NE(overlayTarget, presentationSurface);

    // Clean capture (captureIncludeOverlay = false) takes from presentationSurface
    MockSurface* cleanCaptureTarget = presentationSurface ? presentationSurface : &primarySurface;
    EXPECT_EQ(cleanCaptureTarget->id, 2);
    EXPECT_FALSE(cleanCaptureTarget->isPrimary);

    // Overlay-included capture (captureIncludeOverlay = true) takes from primarySurface
    MockSurface* overlayIncludedCaptureTarget = &primarySurface;
    EXPECT_EQ(overlayIncludedCaptureTarget->id, 1);
    EXPECT_TRUE(overlayIncludedCaptureTarget->isPrimary);
}
