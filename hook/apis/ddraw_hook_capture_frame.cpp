#include "ddraw_hook_internal.h"

namespace {

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

}  // namespace


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
