#include "mediaengine_internal.h"

#include <libavutil/log.h>

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

}  // extern "C"

namespace {

// Route libav* diagnostics into CaptureEngine's own log.
//
// Without this, every message libavformat/libavcodec/libavutil produces goes to the
// process's stderr, which a windowless CaptureEngine process discards: encoder rejections,
// muxer timestamp complaints and RTMP transport errors were all invisible in session logs,
// leaving only CE's own view of a failed recording. Nothing in the tree redirects stderr, so
// the callback is the only way to keep them.
//
// It also removes the reason the live path had to silence libav entirely. FFmpeg composes
// its diagnostics from the URL it was handed, and for a live stream that URL ends in the
// stream key, so the level was pinned to AV_LOG_QUIET for live output. Redacting here means
// the endpoint can never reach the log and the diagnostics can stay on.
void CaptureEngineAvLogCallback(void* avcl, int level, const char* fmt, va_list args) {
    if (level > av_log_get_level() || level == AV_LOG_QUIET)
        return;

    // Fixed stack buffer: this runs on the encode path and must not allocate.
    char line[1024];
    int printPrefix = 1;
    const int written = av_log_format_line2(avcl, level, fmt, args, line, static_cast<int>(sizeof(line)), &printPrefix);
    if (written <= 0)
        return;
    line[sizeof(line) - 1] = '\0';

    // libav repeats identical messages per frame on a sustained fault; keep the first burst
    // and then sample, so one bad stream cannot flood a session log.
    static std::atomic<uint32_t> s_count{0};
    const uint32_t seen = s_count.fetch_add(1, std::memory_order_relaxed) + 1;
    if (seen > 200 && (seen % 500) != 0)
        return;

    std::string message(line);
    while (!message.empty() && (message.back() == '\n' || message.back() == '\r'))
        message.pop_back();
    if (message.empty())
        return;

    DLL_Log("[ffmpeg] %s", ce::privacy::RedactStreamEndpointsForLog(message).c_str());
}

void InstallAvLogCallback() {
    static std::once_flag once;
    std::call_once(once, [] {
        av_log_set_callback(CaptureEngineAvLogCallback);
        DLL_Log("[Media] libav diagnostics routed into the session log (endpoints redacted)");
    });
}

}  // namespace

extern "C" {

MEDIAENGINE_API void MediaEngine_SetLogCallback(LogCallback callback) {
    g_LogCallback.store(callback, std::memory_order_release);
    DLL_Log("MediaEngine Logging Initialized");
}

MEDIAENGINE_API bool MediaEngine_Init(const AppConfig* config) {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    DLL_Log("[Media] MediaEngine_Init Called. Version: %s (Built: %s)", GetCaptureVersion(), GetBuildTimestamp());
    // Before anything can touch avformat/avcodec, so no diagnostic is lost to stderr.
    InstallAvLogCallback();
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

// Recording-health bits for the last finalized output: kRecordingHealthFlagVideoDegraded
// and/or kRecordingHealthFlagAudioDegraded, 0 for a clean output.
MEDIAENGINE_API uint32_t MediaEngine_GetLastOutputDegradedFlags() {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    return mediaengine_g_Engine ? mediaengine_g_Engine->GetLastOutputDegradedFlags() : 0u;
}

MEDIAENGINE_API void MediaEngine_ReleaseEncoderTextures() {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine)
        mediaengine_g_Engine->ReleaseEncoderTextures();
}

MEDIAENGINE_API void MediaEngine_SetRenderLatencyChannel(void* channelBlock) {
    // Standalone like the probe itself: this runs before MediaEngine_Init so the very first
    // measurement of a disposable media process can already be served from the session channel.
    ce::audio::SetRenderLatencyChannel(channelBlock);
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

MEDIAENGINE_API int32_t MediaEngine_QueryInjectFrameCopyCompletion(uint64_t fenceHandle, uint64_t fenceValue,
                                                                   uint32_t sourcePid, uint32_t transportGeneration) {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine) {
        return mediaengine_g_Engine->QueryInjectFrameCopyCompletion(reinterpret_cast<HANDLE>(fenceHandle), fenceValue,
                                                                    sourcePid, transportGeneration);
    }
    return -1;
}

MEDIAENGINE_API void MediaEngine_SetInjectTransportGeneration(uint32_t transportGeneration) {
    std::lock_guard<std::recursive_mutex> apiLock(mediaengine_g_EngineApiMutex);
    if (mediaengine_g_Engine) {
        mediaengine_g_Engine->SetInjectTransportGeneration(transportGeneration);
    }
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
