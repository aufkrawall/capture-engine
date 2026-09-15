#include "ddraw_hook_internal.h"

#include <utility>

namespace {

constexpr uint32_t kMaximumScreenshotDimension = 16384;

bool IsStandardRgb888(const DDPIXELFORMAT& format) {
    return (format.dwFlags & DDPF_RGB) != 0 && format.dwRGBBitCount == 32 &&
           format.dwRBitMask == 0x00FF0000u && format.dwGBitMask == 0x0000FF00u &&
           format.dwBBitMask == 0x000000FFu;
}

bool IsStandardRgb24(const DDPIXELFORMAT& format) {
    return (format.dwFlags & DDPF_RGB) != 0 && format.dwRGBBitCount == 24 &&
           format.dwRBitMask == 0x00FF0000u && format.dwGBitMask == 0x0000FF00u &&
           format.dwBBitMask == 0x000000FFu;
}

struct DirectDrawRgbFormat {
    uint32_t bitCount = 0;
    bool is565 = false;
    bool supported = false;
};

DirectDrawRgbFormat ClassifyDirectDrawRgbFormat(const DDPIXELFORMAT& format) {
    DirectDrawRgbFormat result;
    result.bitCount = format.dwRGBBitCount;
    result.is565 = (format.dwFlags & DDPF_RGB) != 0 && result.bitCount == 16 &&
                   format.dwRBitMask == 0xF800u && format.dwGBitMask == 0x07E0u &&
                   format.dwBBitMask == 0x001Fu && format.dwRGBAlphaBitMask == 0;
    const bool is555 = (format.dwFlags & DDPF_RGB) != 0 &&
                       (result.bitCount == 15 || result.bitCount == 16) &&
                       format.dwRBitMask == 0x7C00u && format.dwGBitMask == 0x03E0u &&
                       format.dwBBitMask == 0x001Fu && format.dwRGBAlphaBitMask == 0;
    result.supported = IsStandardRgb888(format) || IsStandardRgb24(format) || result.is565 || is555;
    return result;
}

HRESULT LockDirectDrawSurfaceForRead(IDirectDrawSurface7* surface, DDSURFACEDESC2& desc) {
    constexpr DWORD kLockAttempts[] = {
        DDLOCK_WAIT | DDLOCK_READONLY | DDLOCK_SURFACEMEMORYPTR | DDLOCK_NOSYSLOCK,
        DDLOCK_WAIT | DDLOCK_READONLY | DDLOCK_SURFACEMEMORYPTR,
        DDLOCK_WAIT | DDLOCK_SURFACEMEMORYPTR,
    };
    HRESULT hr = DDERR_GENERIC;
    for (DWORD flags : kLockAttempts) {
        desc = {};
        desc.dwSize = sizeof(desc);
        hr = surface->Lock(nullptr, &desc, flags, nullptr);
        if (SUCCEEDED(hr))
            break;
    }
    return hr;
}

bool CopyDirectDrawPixelsToBgra(const void* bits, int pitch, uint32_t width, uint32_t height,
                                const DirectDrawRgbFormat& format, std::vector<uint8_t>& pixels) {
    if (!bits || width == 0 || height == 0 || width > UINT32_MAX / sizeof(uint32_t) || !format.supported)
        return false;

    const uint32_t sourceBytesPerPixel = format.bitCount == 32 ? 4u : format.bitCount == 24 ? 3u : 2u;
    const uint64_t requiredSourcePitch = static_cast<uint64_t>(width) * sourceBytesPerPixel;
    const int64_t signedPitch = pitch;
    const uint64_t absolutePitch =
        signedPitch < 0 ? static_cast<uint64_t>(-signedPitch) : static_cast<uint64_t>(signedPitch);
    if (absolutePitch < requiredSourcePitch)
        return false;

    const size_t rowPitch = static_cast<size_t>(width) * sizeof(uint32_t);
    if (height > SIZE_MAX / rowPitch)
        return false;
    pixels.resize(rowPitch * height);

    const auto* sourceBase = static_cast<const uint8_t*>(bits);
    for (uint32_t y = 0; y < height; ++y) {
        const uint8_t* source = sourceBase + static_cast<ptrdiff_t>(y) * pitch;
        uint8_t* destination = pixels.data() + static_cast<size_t>(y) * rowPitch;
        if (format.bitCount == 32) {
            memcpy(destination, source, rowPitch);
            for (uint32_t x = 0; x < width; ++x)
                destination[static_cast<size_t>(x) * 4 + 3] = 0xFFu;
        } else if (format.bitCount == 24) {
            for (uint32_t x = 0; x < width; ++x) {
                destination[0] = source[0];
                destination[1] = source[1];
                destination[2] = source[2];
                destination[3] = 0xFFu;
                source += 3;
                destination += 4;
            }
        } else {
            const auto* source16 = reinterpret_cast<const uint16_t*>(source);
            for (uint32_t x = 0; x < width; ++x) {
                const uint32_t color = format.is565 ? ce::ddraw_present_policy::ExpandRgb565(source16[x])
                                                    : ce::ddraw_present_policy::ExpandRgb555(source16[x]);
                memcpy(destination + static_cast<size_t>(x) * sizeof(color), &color, sizeof(color));
            }
        }
    }
    return true;
}

bool ReadDirectDrawScreenshotViaGdi(IDirectDrawSurface7* surface, uint32_t width, uint32_t height,
                                    std::vector<uint8_t>& pixels, uint32_t& error) {
    HDC surfaceDc = NULL;
    const HRESULT getDcHr = surface->GetDC(&surfaceDc);
    if (FAILED(getDcHr) || !surfaceDc) {
        HookLogImportant("[Screenshot] DirectDraw GetDC fallback failed (hr=0x%08X)",
                         static_cast<unsigned>(getDcHr));
        error = ERROR_READ_FAULT;
        return false;
    }

    HDC memoryDc = CreateCompatibleDC(surfaceDc);
    HBITMAP bitmap = NULL;
    HGDIOBJ previousBitmap = NULL;
    void* bitmapBits = nullptr;
    bool copied = false;

    if (memoryDc) {
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - DirectDraw dimensions fit the Win32 bitmap contract.
        bmi.bmiHeader.biWidth = static_cast<LONG>(width);
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - DirectDraw dimensions fit the Win32 bitmap contract.
        bmi.bmiHeader.biHeight = -static_cast<LONG>(height);
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;
        bitmap = CreateDIBSection(surfaceDc, &bmi, DIB_RGB_COLORS, &bitmapBits, NULL, 0);
    }
    if (memoryDc && bitmap && bitmapBits) {
        previousBitmap = SelectObject(memoryDc, bitmap);
        if (previousBitmap && previousBitmap != HGDI_ERROR &&
            BitBlt(memoryDc, 0, 0, static_cast<int>(width), static_cast<int>(height), surfaceDc, 0, 0, SRCCOPY)) {
            try {
                pixels.resize(static_cast<size_t>(width) * height * sizeof(uint32_t));
                memcpy(pixels.data(), bitmapBits, pixels.size());
                for (size_t alpha = 3; alpha < pixels.size(); alpha += 4)
                    pixels[alpha] = 0xFFu;
                copied = true;
            } catch (...) {
                error = ERROR_NOT_ENOUGH_MEMORY;
            }
        }
    }

    if (previousBitmap && previousBitmap != HGDI_ERROR)
        SelectObject(memoryDc, previousBitmap);
    if (bitmap)
        DeleteObject(bitmap);
    if (memoryDc)
        DeleteDC(memoryDc);
    const HRESULT releaseHr = surface->ReleaseDC(surfaceDc);
    if (FAILED(releaseHr)) {
        HookLogImportant("[Screenshot] DirectDraw ReleaseDC fallback failed (hr=0x%08X)",
                         static_cast<unsigned>(releaseHr));
    }

    if (!copied && error == ERROR_SUCCESS)
        error = ERROR_READ_FAULT;
    return copied;
}

bool QueueDirectDrawScreenshot(SharedMemoryLayout* sharedMemory, uint64_t requestId, uint32_t width,
                               uint32_t height, std::vector<uint8_t>& pixels, const char* route,
                               bool includeOverlay) {
    const bool queued = QueueOwnedScreenshotPixels(sharedMemory, requestId, std::move(pixels), width, height,
                                                   width * static_cast<uint32_t>(sizeof(uint32_t)),
                                                   ScreenshotPixelFormat::BGRA8, ScreenshotColorEncoding::SRGB);
    if (queued) {
        HookLogImportant("[Screenshot] DirectDraw queued request=%llu size=%ux%u route=%s includeOverlay=%d",
                         static_cast<unsigned long long>(requestId), width, height, route,
                         includeOverlay ? 1 : 0);
    } else {
        HookLogImportant("[Screenshot] DirectDraw request=%llu was not queued (status=%u)",
                         static_cast<unsigned long long>(requestId),
                         sharedMemory ? sharedMemory->runtimeState.screenshotStatus.load(std::memory_order_acquire) : 0);
    }
    return queued;
}

}  // namespace

bool DDrawCapture::CaptureScreenshotFromSurface(IDirectDrawSurface7* surface, SharedMemoryLayout* sharedMemory,
                                                uint64_t requestId, uint32_t screenshotWidth,
                                                uint32_t screenshotHeight, bool includeOverlay) {
    std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
    if (HookIsShuttingDown() || !sharedMemory || requestId == 0 ||
        GetPendingScreenshotRequestId(sharedMemory) != requestId) {
        return false;
    }
    if (!surface || screenshotWidth == 0 || screenshotHeight == 0 ||
        screenshotWidth > kMaximumScreenshotDimension || screenshotHeight > kMaximumScreenshotDimension ||
        screenshotHeight > SIZE_MAX / (static_cast<size_t>(screenshotWidth) * sizeof(uint32_t))) {
        HookLogImportant("[Screenshot] DirectDraw request=%llu has no valid presentation surface (%ux%u)",
                         static_cast<unsigned long long>(requestId), screenshotWidth, screenshotHeight);
        CompleteScreenshotRequest(sharedMemory, requestId, ScreenshotRequestStatus::Failed, ERROR_INVALID_DATA);
        return false;
    }

    DDSURFACEDESC2 desc = {};
    const HRESULT lockHr = LockDirectDrawSurfaceForRead(surface, desc);
    std::vector<uint8_t> screenshotPixels;
    bool copied = false;
    bool allocationFailed = false;
    DirectDrawRgbFormat sourceFormat;
    if (SUCCEEDED(lockHr)) {
        sourceFormat = ClassifyDirectDrawRgbFormat(desc.ddpfPixelFormat);
        if (desc.lpSurface && desc.dwWidth == screenshotWidth && desc.dwHeight == screenshotHeight) {
            try {
                copied = CopyDirectDrawPixelsToBgra(desc.lpSurface, desc.lPitch, screenshotWidth, screenshotHeight,
                                                    sourceFormat, screenshotPixels);
            } catch (...) {
                allocationFailed = true;
            }
        }
        const HRESULT unlockHr = surface->Unlock(nullptr);
        if (FAILED(unlockHr)) {
            HookLogImportant("[Screenshot] DirectDraw surface unlock failed (hr=0x%08X)",
                             static_cast<unsigned>(unlockHr));
        }
    }

    if (copied)
        return QueueDirectDrawScreenshot(sharedMemory, requestId, screenshotWidth, screenshotHeight,
                                         screenshotPixels, "surface-lock", includeOverlay);
    if (allocationFailed) {
        HookLogImportant("[Screenshot] DirectDraw request=%llu pixel allocation failed",
                         static_cast<unsigned long long>(requestId));
        CompleteScreenshotRequest(sharedMemory, requestId, ScreenshotRequestStatus::Failed,
                                  ERROR_NOT_ENOUGH_MEMORY);
        return false;
    }

    uint32_t error = ERROR_SUCCESS;
    if (ReadDirectDrawScreenshotViaGdi(surface, screenshotWidth, screenshotHeight, screenshotPixels, error)) {
        return QueueDirectDrawScreenshot(sharedMemory, requestId, screenshotWidth, screenshotHeight,
                                         screenshotPixels, "gdi-fallback", includeOverlay);
    }

    HookLogImportant(
        "[Screenshot] DirectDraw request=%llu read failed (lockHr=0x%08X size=%ux%u source=%ux%u bits=%u)",
        static_cast<unsigned long long>(requestId), static_cast<unsigned>(lockHr), screenshotWidth,
        screenshotHeight, desc.dwWidth, desc.dwHeight, sourceFormat.bitCount);
    CompleteScreenshotRequest(sharedMemory, requestId, ScreenshotRequestStatus::Failed,
                              error == ERROR_SUCCESS ? ERROR_READ_FAULT : error);
    return false;
}


bool DDrawCapture::CaptureFrameFromSurface(IDirectDrawSurface7* surface) {


        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        if (HookIsShuttingDown())
            return false;
        if (!surface) {
            return false;
        }

        DDSURFACEDESC2 desc = {};
        desc.dwSize = sizeof(desc);
        HRESULT hr = surface->Lock(nullptr, &desc,
                                   DDLOCK_WAIT | DDLOCK_READONLY | DDLOCK_SURFACEMEMORYPTR | DDLOCK_NOSYSLOCK,
                                   nullptr);
        if (FAILED(hr)) {
            desc = {};
            desc.dwSize = sizeof(desc);
            hr = surface->Lock(nullptr, &desc, DDLOCK_WAIT | DDLOCK_READONLY | DDLOCK_SURFACEMEMORYPTR, nullptr);
        }
        if (FAILED(hr)) {
            desc = {};
            desc.dwSize = sizeof(desc);
            hr = surface->Lock(nullptr, &desc, DDLOCK_WAIT | DDLOCK_SURFACEMEMORYPTR, nullptr);
        }
        const DDPIXELFORMAT& sourceFormat = desc.ddpfPixelFormat;
        const uint32_t sourceBits = sourceFormat.dwRGBBitCount;
        const bool sourceIs565 = (sourceFormat.dwFlags & DDPF_RGB) != 0 && sourceBits == 16 &&
                                 sourceFormat.dwRBitMask == 0xF800u && sourceFormat.dwGBitMask == 0x07E0u &&
                                 sourceFormat.dwBBitMask == 0x001Fu && sourceFormat.dwRGBAlphaBitMask == 0;
        const bool sourceIs555 = (sourceFormat.dwFlags & DDPF_RGB) != 0 &&
                                 (sourceBits == 15 || sourceBits == 16) &&
                                 sourceFormat.dwRBitMask == 0x7C00u && sourceFormat.dwGBitMask == 0x03E0u &&
                                 sourceFormat.dwBBitMask == 0x001Fu && sourceFormat.dwRGBAlphaBitMask == 0;
        const bool supportedFormat = IsStandardRgb888(sourceFormat) || IsStandardRgb24(sourceFormat) ||
                                     ((sourceBits == 15 || sourceBits == 16) && (sourceIs565 || sourceIs555));
        if (SUCCEEDED(hr) && desc.lpSurface && supportedFormat && desc.dwWidth == width && desc.dwHeight == height) {
            CaptureFrame(desc.lpSurface, desc.lPitch, sourceBits, sourceIs565);
            const HRESULT unlockHr = surface->Unlock(nullptr);
            if (FAILED(unlockHr)) {
                static std::atomic<int> s_captureUnlockFailureLogs{0};
                if (s_captureUnlockFailureLogs.fetch_add(1, std::memory_order_relaxed) < 4) {
                    HookLogImportant("DDraw: Capture surface unlock failed (hr=0x%08X)",
                                     static_cast<unsigned>(unlockHr));
                }
            }
            return true;
        }

        if (SUCCEEDED(hr)) {
            const HRESULT unlockHr = surface->Unlock(nullptr);
            if (FAILED(unlockHr)) {
                static std::atomic<int> s_fallbackUnlockFailureLogs{0};
                if (s_fallbackUnlockFailureLogs.fetch_add(1, std::memory_order_relaxed) < 4) {
                    HookLogImportant("DDraw: Capture fallback surface unlock failed (hr=0x%08X)",
                                     static_cast<unsigned>(unlockHr));
                }
            }
        }

        CaptureFrameViaGDI(surface);
        return true;

}


void DDrawCapture::CaptureFrame(void* bits, int pitch, uint32_t sourceBitCount, bool sourceIs565) {


        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        if (HookIsShuttingDown())
            return;
        if (!initialized || !bits)
            return;

        // Check if we should throttle capture (encoder is falling behind)
        if (g_IPC && g_IPC->GetSharedMem()) {
            if (g_IPC->GetSharedMem()->throttleCapture.load(std::memory_order_acquire)) {
                return;
            }
        }

        SharedMemoryLayout* captureSharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
        const int idx = FindAvailableCaptureTextureSlot(captureSharedMem, writeIndex.load(std::memory_order_relaxed));
        if (idx < 0) {
            droppedFrames.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        writeIndex.store(idx, std::memory_order_relaxed);

        // Capture timestamps are published as raw QPC values.
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);

        // Map staging texture and copy from DDraw surface
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = d3d11Context->Map(stagingTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr) || !mapped.pData)
            return;

        // Convert directly from the locked surface into the existing D3D11
        // upload texture. Old games commonly present RGB565/RGB555; routing
        // those through GetDC allocated and destroyed a full-frame DIB on every
        // capture and forced a second read of the DirectDraw surface.
        const auto* sourceBase = static_cast<const uint8_t*>(bits);
        auto* destinationBase = static_cast<uint8_t*>(mapped.pData);
        for (uint32_t y = 0; y < height; ++y) {
            const uint8_t* source = sourceBase + static_cast<ptrdiff_t>(y) * pitch;
            auto* destination = reinterpret_cast<uint32_t*>(destinationBase + static_cast<size_t>(y) * mapped.RowPitch);
            if (sourceBitCount == 32) {
                memcpy(destination, source, static_cast<size_t>(width) * sizeof(uint32_t));
            } else if (sourceBitCount == 24) {
                for (uint32_t x = 0; x < width; ++x) {
                    destination[x] = 0xFF000000u | (static_cast<uint32_t>(source[2]) << 16) |
                                     (static_cast<uint32_t>(source[1]) << 8) | source[0];
                    source += 3;
                }
            } else {
                const auto* source16 = reinterpret_cast<const uint16_t*>(source);
                for (uint32_t x = 0; x < width; ++x) {
                    destination[x] = sourceIs565 ? ce::ddraw_present_policy::ExpandRgb565(source16[x])
                                                 : ce::ddraw_present_policy::ExpandRgb555(source16[x]);
                }
            }
        }

        if (sourceBitCount != 32) {
            static std::atomic<int> s_convertedFormatLogs{0};
            if (s_convertedFormatLogs.fetch_add(1, std::memory_order_relaxed) < 4) {
                HookLog("DDraw: Capturing %u-bit surface through direct CPU conversion (rgb565=%d)",
                        sourceBitCount, sourceIs565 ? 1 : 0);
            }
        }

        d3d11Context->Unmap(stagingTexture, 0);

        // Copy staging to shared texture
        d3d11Context->CopyResource(sharedTextures[idx], stagingTexture);

        // Signal fence if available
        uint64_t publishedFenceValue = 0;
        if (useFences && context4 && fence) {
            const uint64_t candidateFenceValue = ++fenceValue;
            const HRESULT signalHr = context4->Signal(fence, candidateFenceValue);
            if (SUCCEEDED(signalHr)) {
                publishedFenceValue = candidateFenceValue;
            } else {
                HookLog("DDraw: Capture fence Signal failed value=%llu hr=0x%08X; using implicit sync later",
                        static_cast<unsigned long long>(candidateFenceValue), signalHr);
                useFences = false;
            }
        }
        if (publishedFenceValue == 0)
            d3d11Context->Flush();

        // PASS RAW QPC
        SignalFrameReady(g_IPC, idx, qpc.QuadPart, publishedFenceValue);
        AdvanceWriteIndex();

}


void DDrawCapture::CaptureFrameViaGDI(IDirectDrawSurface7* surface) {


        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        if (HookIsShuttingDown())
            return;
        if (!initialized)
            return;

        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - DirectDraw dimensions are bounded by the capture texture.
        const int captureWidth = static_cast<int>(width);
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - DirectDraw dimensions are bounded by the capture texture.
        const int captureHeight = static_cast<int>(height);

        HDC hdc = NULL;
        if (FAILED(surface->GetDC(&hdc)) || !hdc)
            return;

        if (!captureGdiDc || !captureGdiBitmap || !captureGdiBits) {
            ReleaseGdiCaptureResources();
            captureGdiDc = CreateCompatibleDC(hdc);
            BITMAPINFO bmi = {};
            bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            bmi.bmiHeader.biWidth = captureWidth;
            bmi.bmiHeader.biHeight = -captureHeight;  // Top-down
            bmi.bmiHeader.biPlanes = 1;
            bmi.bmiHeader.biBitCount = 32;
            bmi.bmiHeader.biCompression = BI_RGB;
            if (captureGdiDc) {
                captureGdiBitmap =
                    CreateDIBSection(captureGdiDc, &bmi, DIB_RGB_COLORS, &captureGdiBits, NULL, 0);
            }
            if (captureGdiDc && captureGdiBitmap && captureGdiBits) {
                const HGDIOBJ previous = SelectObject(captureGdiDc, captureGdiBitmap);
                if (!previous || previous == HGDI_ERROR) {
                    captureGdiPreviousBitmap = NULL;
                    ReleaseGdiCaptureResources();
                } else {
                    captureGdiPreviousBitmap = previous;
                }
            } else {
                ReleaseGdiCaptureResources();
            }
        }

        if (captureGdiDc && captureGdiBitmap && captureGdiBits &&
            BitBlt(captureGdiDc, 0, 0, captureWidth, captureHeight, hdc, 0, 0, SRCCOPY)) {
            CaptureFrame(captureGdiBits, captureWidth * 4, 32, false);
        } else {
            static std::atomic<int> s_gdiCaptureFailureLogs{0};
            if (s_gdiCaptureFailureLogs.fetch_add(1, std::memory_order_relaxed) < 4)
                HookLogImportant("DDraw: GDI capture conversion failed");
        }

        const HRESULT releaseHr = surface->ReleaseDC(hdc);
        if (FAILED(releaseHr)) {
            static std::atomic<int> s_gdiReleaseFailureLogs{0};
            if (s_gdiReleaseFailureLogs.fetch_add(1, std::memory_order_relaxed) < 4) {
                HookLogImportant("DDraw: GDI capture ReleaseDC failed (hr=0x%08X)",
                                 static_cast<unsigned>(releaseHr));
            }
        }

}
