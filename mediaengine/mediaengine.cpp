#include "mediaengine_internal.h"

extern "C" {

// Global Logger
static std::atomic<LogCallback> g_LogCallback{nullptr};
static void ReleaseSharedD3D11DeviceGlobals();

MEDIAENGINE_API void DLL_Log(const char* fmt, ...) {
    LogCallback callback = g_LogCallback.load(std::memory_order_acquire);
    if (!callback)
        return;
    char buffer[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(buffer, sizeof(buffer), fmt, args);
    va_end(args);
    callback(buffer);
}

MEDIAENGINE_API void MediaEngine_SetLogCallback(LogCallback callback) {
    g_LogCallback.store(callback, std::memory_order_release);
    DLL_Log("MediaEngine Logging Initialized");
}

MEDIAENGINE_API bool MediaEngine_Init(const AppConfig* config) {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    DLL_Log("[Media] MediaEngine_Init Called. Version: %s (Built: %s)", GetCaptureVersion(), GetBuildTimestamp());
    if (!mediaengine_g_Engine) {
        mediaengine_g_Engine = std::make_unique<MediaEngine>();
    }
    if (mediaengine_g_PendingAudioOnly) {
        mediaengine_g_Engine->SetAudioOnly(true);
        mediaengine_g_PendingAudioOnly = false;
        DLL_Log("[Media] MediaEngine_Init: audio-only mode enabled");
    }
    // config is a pointer, pass it directly
    return mediaengine_g_Engine->Init(config);
}

MEDIAENGINE_API bool MediaEngine_StartRecording() {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine)
        return mediaengine_g_Engine->StartRecording();
    return false;
}

MEDIAENGINE_API void MediaEngine_ReloadConfig(const AppConfig* config) {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine)
        mediaengine_g_Engine->ReloadConfig(config);
}

MEDIAENGINE_API void MediaEngine_SetActiveScreenGrab(bool activeScreenGrab) {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine)
        mediaengine_g_Engine->SetActiveScreenGrab(activeScreenGrab);
}

MEDIAENGINE_API void MediaEngine_SetWgcStartupExtraDelayQpc(int64_t delayQpc) {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine)
        mediaengine_g_Engine->SetWgcStartupExtraDelayQpc(delayQpc);
}

MEDIAENGINE_API void MediaEngine_SetAudioOnly(bool audioOnly) {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    mediaengine_g_PendingAudioOnly = audioOnly;
    if (mediaengine_g_Engine)
        mediaengine_g_Engine->SetAudioOnly(audioOnly);
}

MEDIAENGINE_API bool MediaEngine_StopRecording(bool cancelUncommittedVideo) {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    return mediaengine_g_Engine ? mediaengine_g_Engine->StopRecording(cancelUncommittedVideo) : false;
}

MEDIAENGINE_API void MediaEngine_ReleaseEncoderTextures() {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine)
        mediaengine_g_Engine->ReleaseEncoderTextures();
}

MEDIAENGINE_API bool MediaEngine_MeasureRenderEndpointLatency(const char* cacheDir, bool forceRemeasure,
                                                              double* outLatencyMs) {
    // Standalone WASAPI probe; intentionally NOT guarded by the engine instance (it can run before
    // MediaEngine_Init). The probe itself is fail-safe and logs all components.
    const std::string dir = cacheDir ? cacheDir : "";
    const ce::audio::RenderLatencyProbeResult r = ce::audio::MeasureRenderEndpointLatency(dir, forceRemeasure);
    if (!r.ok) {
        return false;
    }
    if (outLatencyMs) {
        *outLatencyMs = r.latencyMs;
    }
    return true;
}

MEDIAENGINE_API void MediaEngine_Shutdown() {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine)
        mediaengine_g_Engine->StopRecording();
    mediaengine_g_Engine.reset();
    ReleaseSharedD3D11DeviceGlobals();
}

MEDIAENGINE_API bool MediaEngine_ProcessFrame(uint64_t textureHandle, uint64_t fenceHandle, uint64_t fenceValue,
                                              int64_t timestamp, int32_t luidLow, int32_t luidHigh, uint32_t sourcePid,
                                              uint32_t width, uint32_t height, uint32_t format, bool isHDR,
                                              bool isShmem, int shmemSlot,
                                              const ce::cursor::CaptureState* cursorState) {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine) {
        return mediaengine_g_Engine->ProcessFrame(textureHandle, fenceHandle, fenceValue, timestamp, luidLow, luidHigh, sourcePid,
                                      width, height, format, isHDR, isShmem, shmemSlot, cursorState);
    }
    return false;
}

MEDIAENGINE_API bool MediaEngine_RepeatLastFrame(int64_t timestamp, const ce::cursor::CaptureState* cursorState) {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine) {
        return mediaengine_g_Engine->RepeatLastFrame(timestamp, cursorState);
    }
    return false;
}

MEDIAENGINE_API bool MediaEngine_RepeatLastFrameWithTimeline(int64_t timestamp, int64_t timelineElapsedUs,
                                                             const ce::cursor::CaptureState* cursorState) {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine) {
        return mediaengine_g_Engine->RepeatLastFrame(timestamp, timelineElapsedUs, cursorState);
    }
    return false;
}

MEDIAENGINE_API bool MediaEngine_CanRepeatLastFrame() {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine) {
        return mediaengine_g_Engine->CanRepeatLastFrame();
    }
    return false;
}

MEDIAENGINE_API void MediaEngine_ResetRepeatFrameCache() {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine) {
        mediaengine_g_Engine->ResetRepeatFrameCache();
    }
}

MEDIAENGINE_API bool MediaEngine_PrepareFrameD3D11(void* texture, uint32_t width, uint32_t height, bool isHDR) {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine) {
        return mediaengine_g_Engine->PrepareFrameD3D11(texture, width, height, isHDR);
    }
    return false;
}

MEDIAENGINE_API bool MediaEngine_ProcessFrameD3D11(void* texture, int64_t timestamp, uint32_t width, uint32_t height,
                                                   bool isHDR, int32_t captureLeft, int32_t captureTop,
                                                   int64_t timelineElapsedUs,
                                                   const ce::cursor::CaptureState* cursorState) {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine) {
        return mediaengine_g_Engine->ProcessFrameD3D11(texture, timestamp, width, height, isHDR, captureLeft, captureTop,
                                           timelineElapsedUs, cursorState);
    }
    return false;
}

MEDIAENGINE_API bool MediaEngine_CreateSharedCaptureTextures(uint32_t width, uint32_t height, uint32_t format,
                                                             SharedMemoryLayout* sharedMem) {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (!mediaengine_g_Engine) {
        DLL_Log("[MediaEngine] CreateSharedCaptureTextures: Engine not ready");
        return false;
    }
    return mediaengine_g_Engine->CreateSharedCaptureTextures(width, height, format, sharedMem);
}

MEDIAENGINE_API int64_t MediaEngine_GetLastFrameEncodeTimeUs() {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine) {
        return mediaengine_g_Engine->GetLastVideoEncodeTimeUs();
    }
    return 0;
}

MEDIAENGINE_API void MediaEngine_SetSharedMem(void* pSharedMem, void* pShmem) {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine)
        mediaengine_g_Engine->UpdateVideoEncoderSharedMem(pSharedMem, pShmem);
}

MEDIAENGINE_API int64_t MediaEngine_GetLastFrameFenceWaitUs() {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine) {
        return mediaengine_g_Engine->GetLastFrameFenceWaitUs();
    }
    return 0;
}

MEDIAENGINE_API bool MediaEngine_WasLastFrameDeferred() {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine) {
        return mediaengine_g_Engine->WasLastFrameDeferred();
    }
    return false;
}

// Shared D3D11 device for screengrab mode - ensures ScreenCapture and
// VideoEncoder use same device
ID3D11Device* g_SharedD3D11Device = nullptr;
ID3D11DeviceContext* g_SharedD3D11Context = nullptr;

static void ReleaseSharedD3D11DeviceGlobals() {
    if (g_SharedD3D11Context) {
        g_SharedD3D11Context->ClearState();
        g_SharedD3D11Context->Flush();
    }
    if (g_SharedD3D11Device) {
        IDXGIDevice3* dxgiDevice3 = nullptr;
        if (SUCCEEDED(g_SharedD3D11Device->QueryInterface(IID_PPV_ARGS(&dxgiDevice3))) && dxgiDevice3) {
            dxgiDevice3->Trim();
            dxgiDevice3->Release();
            DLL_Log("[MediaEngine] Trimmed shared D3D11 device residency");
        }
    }
    if (g_SharedD3D11Context) {
        g_SharedD3D11Context->Release();
        g_SharedD3D11Context = nullptr;
    }
    if (g_SharedD3D11Device) {
        g_SharedD3D11Device->Release();
        g_SharedD3D11Device = nullptr;
    }
}

MEDIAENGINE_API ID3D11Device* MediaEngine_GetD3D11Device() {
    if (g_SharedD3D11Device)
        return g_SharedD3D11Device;

    // Create D3D11 device with video support
    D3D_FEATURE_LEVEL featureLevels[] = {D3D_FEATURE_LEVEL_11_1, D3D_FEATURE_LEVEL_11_0};
    D3D_FEATURE_LEVEL featureLevel;

    UINT createFlags = D3D11_CREATE_DEVICE_VIDEO_SUPPORT | D3D11_CREATE_DEVICE_BGRA_SUPPORT;
#ifdef _DEBUG
    createFlags |= D3D11_CREATE_DEVICE_DEBUG;
#endif

    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, createFlags, featureLevels, 2,
                                   D3D11_SDK_VERSION, &g_SharedD3D11Device, &featureLevel, &g_SharedD3D11Context);

    if (FAILED(hr)) {
        DLL_Log("[MediaEngine] Failed to create shared D3D11 device: HR=0x%x", hr);
        return nullptr;
    }

    DLL_Log("[MediaEngine] Created shared D3D11 device (Feature Level: 0x%x)", featureLevel);
    // Enable Multithreaded protection for D3D11 device
    ID3D11Multithread* pMultithread = nullptr;
    if (SUCCEEDED(g_SharedD3D11Device->QueryInterface(__uuidof(ID3D11Multithread), (void**)&pMultithread))) {
        pMultithread->SetMultithreadProtected(TRUE);
        pMultithread->Release();
        DLL_Log("[Media] D3D11 Multithread protection ENABLED");
    }

    return g_SharedD3D11Device;
}

MEDIAENGINE_API void MediaEngine_ReleaseSharedD3D11Device() {
    ReleaseSharedD3D11DeviceGlobals();
}

// D3D11 Immediate Context Mutex
// Protects access to the immediate context shared between WGC thread and
// Encoder thread
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
std::recursive_mutex g_D3D11Mutex;

MEDIAENGINE_API void MediaEngine_LockD3D11() {
    g_D3D11Mutex.lock();
}

MEDIAENGINE_API void MediaEngine_UnlockD3D11() {
    g_D3D11Mutex.unlock();
}

MEDIAENGINE_API void MediaEngine_SetSourcePrefers10Bit(bool prefer10Bit) {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine)
        mediaengine_g_Engine->SetSourcePrefers10BitHint(prefer10Bit);
}

// Suppress encoder-side cursor composition while the capture source's frames
// already contain the cursor (DXGI duplication reporting a software/composed
// cursor) so the recording does not show a double cursor. Toggled by the
// capture layer on hardware/software cursor-plane transitions.
MEDIAENGINE_API void MediaEngine_SetCursorCompositionSuppressed(bool suppressed) {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine)
        mediaengine_g_Engine->SetCursorCompositionSuppressedHint(suppressed);
}

}  // extern "C"
