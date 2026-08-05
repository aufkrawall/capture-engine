#include "dx8_hook_internal.h"


void DX8Capture::Init(IDirect3DDevice8* device,  HWND hwnd) {


        std::lock_guard<std::recursive_mutex> captureLock(captureMutex);
        if (initialized)
            return;
        if (generationResetPending && !CleanupDX8(false))
            return;

        d3d8Device = device;
        overlayHwnd = hwnd;

        // Get backbuffer size from HWND
        RECT rect;
        GetClientRect(hwnd, &rect);
        width = rect.right - rect.left;
        height = rect.bottom - rect.top;
        format = DXGI_FORMAT_B8G8R8A8_UNORM;

        if (width == 0 || height == 0) {
            HookLog("DX8: Invalid window size");
            return;
        }

        // Create D3D9Ex wrapper for sharing
        if (!CreateD3D9ExWrapper(hwnd)) {
            CleanupDX8(false);
            return;
        }

        // Create D3D11 device
        if (!CreateD3D11Device()) {
            CleanupDX8(false);
            return;
        }

        // Create shared textures
        if (!CreateSharedTextures()) {
            CleanupDX8(false);
            return;
        }

        // Create D3D9Ex shared surface
        if (!CreateD3D9ExSharedSurface()) {
            CleanupDX8(false);
            return;
        }

        // Publish to shared memory
        if (g_IPC) {
            PublishToSharedMemory(g_IPC);
        }

        initialized = true;
        HookLog("DX8 Capture Initialized: %dx%d", width, height);

}

void DX8Capture::CaptureFrame(IDirect3DDevice8* device,  bool useFrontBuffer) {


        std::unique_lock<std::recursive_mutex> captureLock(captureMutex, std::try_to_lock);
        if (!captureLock.owns_lock())
            return;
        if (!initialized || !d3d9DeviceEx || !d3d9SharedSurface)
            return;

        HWND hwnd = overlayHwnd;
        RECT rect = {};
        if (hwnd && GetClientRect(hwnd, &rect)) {
            const uint32_t currentWidth = static_cast<uint32_t>(std::max<LONG>(0, rect.right - rect.left));
            const uint32_t currentHeight = static_cast<uint32_t>(std::max<LONG>(0, rect.bottom - rect.top));
            if (currentWidth > 0 && currentHeight > 0 && (currentWidth != width || currentHeight != height)) {
                HookLog("DX8: Capture resize detected (%ux%u -> %ux%u); rebuilding shared transport", width, height,
                        currentWidth, currentHeight);
                if (!CleanupDX8(false))
                    return;
                Init(device, hwnd);
                if (!initialized)
                    return;
            }
        }

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

        const bool copied = useFrontBuffer ? CopyFrontBufferToSurface9(device, d3d9SharedSurface)
                                           : CopyBackBufferToSurface9(device, d3d9SharedSurface);
        if (!copied) {
            return;
        }

        D3DLOCKED_RECT lockedRect = {};
        const HRESULT lockHr = d3d9SharedSurface->LockRect(&lockedRect, NULL, D3DLOCK_READONLY);
        if (FAILED(lockHr) || !lockedRect.pBits)
            return;
        d3d11Context->UpdateSubresource(sharedTextures[idx], 0, NULL, lockedRect.pBits, lockedRect.Pitch, 0);
        d3d9SharedSurface->UnlockRect();

        // Signal fence if available
        uint64_t publishedFenceValue = 0;
        if (useFences && context4 && fence) {
            const uint64_t candidateFenceValue = ++fenceValue;
            const HRESULT signalHr = context4->Signal(fence, candidateFenceValue);
            if (SUCCEEDED(signalHr)) {
                publishedFenceValue = candidateFenceValue;
            } else {
                HookLog("DX8: Capture fence Signal failed value=%llu hr=0x%08X; using implicit sync later",
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
