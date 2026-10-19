// Cursor bitmap acquisition for CursorRenderer: the cursor image as straight-alpha BGRA, read
// from GDI at the requested size (resource-sized when the cursor has one, scaled otherwise).
// Split from cursor_renderer.cpp by size; the D3D11 composite stays there.

#include "cursor_renderer.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <memory>

#include "cursor_bitmap_utils.h"

std::unique_ptr<uint8_t[]> ScaleBitmapNearestNeighbor(const uint8_t* src, uint32_t srcW, uint32_t srcH, uint32_t dstW,
                                                      uint32_t dstH) {
    if (!src || srcW == 0 || srcH == 0 || dstW == 0 || dstH == 0) {
        return nullptr;
    }
    constexpr uint64_t kMaxPixels = static_cast<uint64_t>((std::numeric_limits<size_t>::max)()) / 4u;
    const uint64_t srcPixels = static_cast<uint64_t>(srcW) * srcH;
    const uint64_t dstPixels = static_cast<uint64_t>(dstW) * dstH;
    if (srcPixels > kMaxPixels || dstPixels > kMaxPixels) {
        return nullptr;
    }
    const size_t srcStride = static_cast<size_t>(srcW) * 4u, dstStride = static_cast<size_t>(dstW) * 4u;
    const size_t dstSize = static_cast<size_t>(dstPixels) * 4u;
    if (srcW == dstW && srcH == dstH) {
        // No scaling needed - return a copy
        auto result = std::make_unique<uint8_t[]>(dstSize);
        memcpy(result.get(), src, dstSize);
        return result;
    }
    auto result = std::make_unique<uint8_t[]>(dstSize);
    uint8_t* dst = result.get();

    for (uint32_t dy = 0; dy < dstH; dy++) {
        const uint32_t sy = static_cast<uint32_t>(static_cast<uint64_t>(dy) * srcH / dstH);
        const uint8_t* srcRow = src + static_cast<size_t>(sy) * srcStride;
        uint8_t* dstRow = dst + static_cast<size_t>(dy) * dstStride;
        for (uint32_t dx = 0; dx < dstW; dx++) {
            const uint32_t sx = static_cast<uint32_t>(static_cast<uint64_t>(dx) * srcW / dstW);
            const uint8_t* srcPixel = srcRow + static_cast<size_t>(sx) * 4u;
            uint8_t* dstPixel = dstRow + static_cast<size_t>(dx) * 4u;
            dstPixel[0] = srcPixel[0];  // B
            dstPixel[1] = srcPixel[1];  // G
            dstPixel[2] = srcPixel[2];  // R
            dstPixel[3] = srcPixel[3];  // A
        }
    }
    return result;
}

bool CursorRenderer::ExtractCursorBitmap(HICON icon, uint8_t** outBitmap, uint32_t* outWidth, uint32_t* outHeight,
                                         bool* outIsMonochrome) {
    if (!outBitmap || !outWidth || !outHeight || !outIsMonochrome) {
        return false;
    }

    *outBitmap = nullptr;
    *outWidth = 0;
    *outHeight = 0;
    *outIsMonochrome = false;

    ICONINFO ii = {};
    if (!GetIconInfo(icon, &ii)) {
        return false;
    }

    *outIsMonochrome = ii.hbmColor == nullptr;
    BITMAP nativeBitmap = {};
    const HBITMAP dimensionBitmap = ii.hbmColor ? ii.hbmColor : ii.hbmMask;
    const bool dimensionsValid =
        dimensionBitmap && GetObject(dimensionBitmap, sizeof(nativeBitmap), &nativeBitmap) != 0;
    DeleteObject(ii.hbmColor);
    DeleteObject(ii.hbmMask);
    if (!dimensionsValid || nativeBitmap.bmWidth <= 0 || nativeBitmap.bmHeight == 0) {
        return false;
    }

    const uint32_t width = static_cast<uint32_t>(nativeBitmap.bmWidth);
    const uint32_t bitmapHeight = static_cast<uint32_t>(std::abs(nativeBitmap.bmHeight));
    const uint32_t height = *outIsMonochrome ? bitmapHeight / 2 : bitmapHeight;
    if (width == 0 || height == 0) {
        return false;
    }

    BITMAPINFO dibInfo = {};
    dibInfo.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    dibInfo.bmiHeader.biWidth = static_cast<LONG>(width);
    dibInfo.bmiHeader.biHeight = -static_cast<LONG>(height);  // Top-down, matching D3D texture rows.
    dibInfo.bmiHeader.biPlanes = 1;
    dibInfo.bmiHeader.biBitCount = 32;
    dibInfo.bmiHeader.biCompression = BI_RGB;

    HDC dc = CreateCompatibleDC(nullptr);
    void* dibPixels = nullptr;
    HBITMAP dib = dc ? CreateDIBSection(dc, &dibInfo, DIB_RGB_COLORS, &dibPixels, nullptr, 0) : nullptr;
    HGDIOBJ previousBitmap = dib ? SelectObject(dc, dib) : nullptr;
    if (!dc || !dib || !dibPixels || !previousBitmap || previousBitmap == HGDI_ERROR) {
        if (dib)
            DeleteObject(dib);
        if (dc)
            DeleteDC(dc);
        return false;
    }

    const size_t pixelCount = static_cast<size_t>(width) * height;
    auto overBlack = std::make_unique<uint8_t[]>(pixelCount * 4);
    auto result = std::make_unique<uint8_t[]>(pixelCount * 4);
    auto drawOver = [&](uint32_t background) {
        std::fill_n(static_cast<uint32_t*>(dibPixels), pixelCount, background);
        if (DrawIconEx(dc, 0, 0, icon, static_cast<int>(width), static_cast<int>(height), 0, nullptr, DI_NORMAL) ==
            FALSE) {
            return false;
        }
        // DrawIconEx may be held in GDI's per-thread command batch. Flush before
        // reading the DIB section through its directly mapped CPU pointer.
        return GdiFlush() != FALSE;
    };

    bool rendered = drawOver(0xFF000000u);
    if (rendered) {
        memcpy(overBlack.get(), dibPixels, pixelCount * 4);
        rendered = drawOver(0xFFFFFFFFu);
    }
    if (rendered) {
        const auto* overWhite = static_cast<const uint8_t*>(dibPixels);
        for (size_t i = 0; i < pixelCount; ++i) {
            const ce::cursor_bitmap::Bgra8 black = {
                overBlack[i * 4 + 0],
                overBlack[i * 4 + 1],
                overBlack[i * 4 + 2],
                overBlack[i * 4 + 3],
            };
            const ce::cursor_bitmap::Bgra8 white = {
                overWhite[i * 4 + 0],
                overWhite[i * 4 + 1],
                overWhite[i * 4 + 2],
                overWhite[i * 4 + 3],
            };
            const auto straight = ce::cursor_bitmap::ReconstructStraightAlpha(black, white);
            result[i * 4 + 0] = straight.b;
            result[i * 4 + 1] = straight.g;
            result[i * 4 + 2] = straight.r;
            result[i * 4 + 3] = straight.a;
        }
    }

    SelectObject(dc, previousBitmap);
    DeleteObject(dib);
    DeleteDC(dc);
    if (!rendered) {
        return false;
    }

    *outBitmap = result.release();
    *outWidth = width;
    *outHeight = height;
    return true;
}

bool CursorRenderer::LoadCursorBitmap(HCURSOR cursor, uint32_t requestedWidth, uint32_t requestedHeight,
                                      CursorBitmapData* result) {
    if (!cursor || !result) {
        return false;
    }

    *result = {};
    HICON icon = CopyIcon(cursor);
    if (!icon) {
        return false;
    }

    auto readHotspot = [](HICON source, int32_t* x, int32_t* y) {
        ICONINFO info = {};
        if (!GetIconInfo(source, &info)) {
            return false;
        }
        *x = static_cast<int32_t>(info.xHotspot);
        *y = static_cast<int32_t>(info.yHotspot);
        DeleteObject(info.hbmColor);
        DeleteObject(info.hbmMask);
        return true;
    };

    uint8_t* rawBitmap = nullptr;
    if (!readHotspot(icon, &result->hotspotX, &result->hotspotY) ||
        !ExtractCursorBitmap(icon, &rawBitmap, &result->width, &result->height, &result->isMonochrome)) {
        DestroyIcon(icon);
        return false;
    }
    result->pixels.reset(rawBitmap);

    const uint32_t targetWidth = std::max(result->width, requestedWidth);
    const uint32_t targetHeight = std::max(result->height, requestedHeight);
    if (targetWidth != result->width || targetHeight != result->height) {
        HICON resourceSized = reinterpret_cast<HICON>(CopyImage(cursor, IMAGE_CURSOR, static_cast<int>(targetWidth),
                                                                static_cast<int>(targetHeight), LR_COPYFROMRESOURCE));
        if (resourceSized) {
            CursorBitmapData resource;
            uint8_t* resourcePixels = nullptr;
            if (readHotspot(resourceSized, &resource.hotspotX, &resource.hotspotY) &&
                ExtractCursorBitmap(resourceSized, &resourcePixels, &resource.width, &resource.height,
                                    &resource.isMonochrome)) {
                resource.pixels.reset(resourcePixels);
                if (resource.width >= result->width && resource.height >= result->height) {
                    *result = std::move(resource);
                }
            }
            DestroyIcon(resourceSized);
        }
    }

    const uint32_t finalWidth = std::max(result->width, targetWidth);
    const uint32_t finalHeight = std::max(result->height, targetHeight);
    if (finalWidth != result->width || finalHeight != result->height) {
        const uint32_t sourceWidth = result->width;
        const uint32_t sourceHeight = result->height;
        auto scaled =
            ScaleBitmapNearestNeighbor(result->pixels.get(), sourceWidth, sourceHeight, finalWidth, finalHeight);
        if (scaled) {
            result->hotspotX = static_cast<int32_t>(
                (static_cast<int64_t>(result->hotspotX) * finalWidth + sourceWidth / 2) / sourceWidth);
            result->hotspotY = static_cast<int32_t>(
                (static_cast<int64_t>(result->hotspotY) * finalHeight + sourceHeight / 2) / sourceHeight);
            result->pixels = std::move(scaled);
            result->width = finalWidth;
            result->height = finalHeight;
        }
    }
    DestroyIcon(icon);
    return result->pixels != nullptr && result->width != 0 && result->height != 0;
}
