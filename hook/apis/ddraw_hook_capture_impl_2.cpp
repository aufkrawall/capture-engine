#include "ddraw_hook_internal.h"


void DDrawCapture::CaptureFrame(void* bits,  int pitch) {


        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
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

        // Get timestamp
        static int64_t qpcFreq = 0;
        if (qpcFreq == 0) {
            LARGE_INTEGER f;
            QueryPerformanceFrequency(&f);
            qpcFreq = f.QuadPart;
        }
        LARGE_INTEGER qpc;
        QueryPerformanceCounter(&qpc);
        int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;

        // Map staging texture and copy from DDraw surface
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = d3d11Context->Map(stagingTexture, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr) || !mapped.pData)
            return;

        // Copy row by row (handle different pitches)
        uint8_t* src = (uint8_t*)bits;
        uint8_t* dst = (uint8_t*)mapped.pData;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        int rowSize = width * 4;  // Assuming 32-bit color

        for (uint32_t y = 0; y < height; y++) {
            memcpy(dst, src, rowSize);
            src += pitch;
            dst += mapped.RowPitch;
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
        if (!initialized)
            return;

        HDC hdc = NULL;
        if (FAILED(surface->GetDC(&hdc)) || !hdc)
            return;

        // Create compatible DC and bitmap
        HDC memDC = CreateCompatibleDC(hdc);
        BITMAPINFO bmi = {};
        bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        bmi.bmiHeader.biWidth = width;
        bmi.bmiHeader.biHeight = -(int)height;  // Top-down
        bmi.bmiHeader.biPlanes = 1;
        bmi.bmiHeader.biBitCount = 32;
        bmi.bmiHeader.biCompression = BI_RGB;

        void* bits = nullptr;
        HBITMAP hbm = CreateDIBSection(memDC, &bmi, DIB_RGB_COLORS, &bits, NULL, 0);
        HGDIOBJ oldBm = SelectObject(memDC, hbm);

        // BitBlt from surface DC
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        BitBlt(memDC, 0, 0, width, height, hdc, 0, 0, SRCCOPY);

        // Capture the bits
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        CaptureFrame(bits, width * 4);

        // Cleanup
        SelectObject(memDC, oldBm);
        DeleteObject(hbm);
        DeleteDC(memDC);

        surface->ReleaseDC(hdc);

}
