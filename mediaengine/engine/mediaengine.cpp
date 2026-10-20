#include "mediaengine_internal.h"
#include "submission_transaction.h"

#include <libavutil/log.h>

extern "C" {

// Global Logger
static std::atomic<LogCallback> g_LogCallback{nullptr};

#if defined(__GNUC__) || defined(__clang__)
void DLL_Log(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
#endif

void DLL_Log(const char* fmt, ...) {
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

MEDIAENGINE_API bool MediaEngine_GetBuildIdentity(ce::build_identity::Record* identity) {
    return ce::build_identity::Publish(identity);
}

MEDIAENGINE_API bool MediaEngine_Init(const AppConfig* config) {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    DLL_Log("[Media] MediaEngine_Init Called. Version: %s (Built: %s)", GetCaptureVersion(), GetBuildTimestamp());
    // Before anything can touch avformat/avcodec, so no diagnostic is lost to stderr.
    InstallAvLogCallback();
    if (!instance.engine) {
        instance.engine = std::make_unique<MediaEngine>(instance);
    }
    if (instance.pendingAudioOnly) {
        instance.engine->SetAudioOnly(true);
        instance.pendingAudioOnly = false;
        DLL_Log("[Media] MediaEngine_Init: audio-only mode enabled");
    }
    // config is a pointer, pass it directly
    return instance.engine->Init(config);
}

MEDIAENGINE_API bool MediaEngine_StartRecording() {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    if (instance.engine)
        return instance.engine->StartRecording();
    return false;
}

MEDIAENGINE_API void MediaEngine_ReloadConfig(const AppConfig* config) {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    if (instance.engine)
        instance.engine->ReloadConfig(config);
}

MEDIAENGINE_API void MediaEngine_SetActiveScreenGrab(bool activeScreenGrab) {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    if (instance.engine)
        instance.engine->SetActiveScreenGrab(activeScreenGrab);
}

MEDIAENGINE_API void MediaEngine_SetWgcStartupExtraDelayQpc(int64_t delayQpc) {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    if (instance.engine)
        instance.engine->SetWgcStartupExtraDelayQpc(delayQpc);
}

MEDIAENGINE_API void MediaEngine_SetScreenGrabLatencyReductionQpc(int64_t reductionQpc) {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    if (instance.engine)
        instance.engine->SetScreenGrabLatencyReductionQpc(reductionQpc);
}

MEDIAENGINE_API void MediaEngine_SetAudioOnly(bool audioOnly) {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    instance.pendingAudioOnly = audioOnly;
    if (instance.engine)
        instance.engine->SetAudioOnly(audioOnly);
}

MEDIAENGINE_API bool MediaEngine_StopRecording(bool cancelUncommittedVideo) {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    return instance.engine ? instance.engine->StopRecording(cancelUncommittedVideo) : false;
}

// Recording-health bits for the last finalized output: kRecordingHealthFlagVideoDegraded
// and/or kRecordingHealthFlagAudioDegraded, 0 for a clean output.
MEDIAENGINE_API uint32_t MediaEngine_GetLastOutputDegradedFlags() {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    return instance.engine ? instance.engine->GetLastOutputDegradedFlags() : 0u;
}

MEDIAENGINE_API uint32_t MediaEngine_GetLastOutputPath(char* buffer, uint32_t capacity) {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    return instance.engine ? instance.engine->CopyLastOutputPath(buffer, capacity) : 0u;
}

MEDIAENGINE_API void MediaEngine_ReleaseEncoderTextures() {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    if (instance.engine)
        instance.engine->ReleaseEncoderTextures();
}

MEDIAENGINE_API void MediaEngine_SetRenderLatencyChannel(void* channelBlock) {
    // Standalone like the probe itself: this runs before MediaEngine_Init so the very first
    // measurement of a disposable media process can already be served from the session channel.
    DefaultMediaEngineInstance().renderLatencyChannel.store(ce::audio::AcceptRenderLatencyChannel(channelBlock),
                                                            std::memory_order_release);
}

MEDIAENGINE_API bool MediaEngine_MeasureRenderEndpointLatency(const char* cacheDir, bool forceRemeasure,
                                                              double* outLatencyMs) {
    // Standalone WASAPI probe; intentionally NOT guarded by the engine instance (it can run before
    // MediaEngine_Init). The probe itself is fail-safe and logs all components.
    const std::string dir = cacheDir ? cacheDir : "";
    ce::av_sync::LatencyChannelBlock* sessionChannel =
        DefaultMediaEngineInstance().renderLatencyChannel.load(std::memory_order_acquire);
    const ce::audio::RenderLatencyProbeResult r =
        ce::audio::MeasureRenderEndpointLatency(dir, forceRemeasure, sessionChannel);
    if (!r.ok) {
        return false;
    }
    if (outLatencyMs) {
        *outLatencyMs = r.latencyMs;
    }
    return true;
}

MEDIAENGINE_API void MediaEngine_Shutdown() {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    if (instance.engine)
        instance.engine->StopRecording();
    instance.engine.reset();
    // The retained-reservoir converter holds a device reference, so ReleaseSharedDevice releases it first.
    instance.ReleaseSharedDevice();
}

MEDIAENGINE_API bool MediaEngine_CanRepeatLastFrame() {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    if (instance.engine) {
        return instance.engine->CanRepeatLastFrame();
    }
    return false;
}

MEDIAENGINE_API void MediaEngine_ResetRepeatFrameCache() {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    if (instance.engine) {
        instance.engine->ResetRepeatFrameCache();
    }
}

MEDIAENGINE_API bool MediaEngine_PrepareFrameD3D11(void* texture, uint32_t width, uint32_t height, bool isHDR) {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    if (instance.engine) {
        return instance.engine->PrepareFrameD3D11(texture, width, height, isHDR);
    }
    return false;
}

MEDIAENGINE_API bool MediaEngine_PrepareInjectVideo(int32_t luidLow, int32_t luidHigh, uint32_t width,
                                                    uint32_t height, uint32_t format, bool isHDR) {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    if (instance.engine) {
        return instance.engine->PrepareInjectVideo(luidLow, luidHigh, width, height, format, isHDR);
    }
    return false;
}

MEDIAENGINE_API bool MediaEngine_SubmitFrameWithResultV1(const VideoFrameSubmissionDesc* desc,
                                                         ce::media::FrameSubmissionResultV1* result) {
    if (!desc)
        return false;
    return ce::media::detail::PublishSubmissionResult(result, [&] {
        MediaEngineInstance& instance = DefaultMediaEngineInstance();
        std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
        return instance.engine ? instance.engine->SubmitInjectFrame(*desc) : ce::media::UnacceptedSubmission(false);
    });
}

MEDIAENGINE_API bool MediaEngine_SubmitFrameD3D11WithResultV1(const D3D11FrameSubmissionDesc* desc,
                                                              ce::media::FrameSubmissionResultV1* result) {
    if (!desc)
        return false;
    return ce::media::detail::PublishSubmissionResult(result, [&] {
        MediaEngineInstance& instance = DefaultMediaEngineInstance();
        std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
        return instance.engine ? instance.engine->SubmitD3D11Frame(*desc) : ce::media::UnacceptedSubmission(false);
    });
}

MEDIAENGINE_API bool MediaEngine_RepeatLastFrameWithResultV1(int64_t timestamp, int64_t timelineElapsedUs,
    const ce::cursor::CaptureState* cursorState, ce::media::FrameSubmissionResultV1* result) {
    return ce::media::detail::PublishSubmissionResult(result, [&] {
        MediaEngineInstance& instance = DefaultMediaEngineInstance();
        std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
        return instance.engine ? instance.engine->SubmitRepeat(timestamp, timelineElapsedUs, cursorState)
                               : ce::media::UnacceptedSubmission(false, false);
    });
}

MEDIAENGINE_API bool MediaEngine_CreateSharedCaptureTextures(uint32_t width, uint32_t height, uint32_t format,
                                                             SharedMemoryLayout* sharedMem) {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    if (!instance.engine) {
        DLL_Log("[MediaEngine] CreateSharedCaptureTextures: Engine not ready");
        return false;
    }
    return instance.engine->CreateSharedCaptureTextures(width, height, format, sharedMem);
}

MEDIAENGINE_API int64_t MediaEngine_GetLastFrameEncodeTimeUs() {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    if (instance.engine) {
        return instance.engine->GetLastVideoEncodeTimeUs();
    }
    return 0;
}

MEDIAENGINE_API void MediaEngine_SetSharedMem(void* pSharedMem, void* pShmem) {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    if (instance.engine)
        instance.engine->UpdateVideoEncoderSharedMem(pSharedMem, pShmem);
}

MEDIAENGINE_API int64_t MediaEngine_GetLastFrameFenceWaitUs() {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    if (instance.engine) {
        return instance.engine->GetLastFrameFenceWaitUs();
    }
    return 0;
}

MEDIAENGINE_API bool MediaEngine_GetMuxFlowSnapshotV1(ce::media::MuxFlowSnapshotV1* snapshot) {
    if (!snapshot || snapshot->size != sizeof(ce::media::MuxFlowSnapshotV1)) {
        return false;
    }
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    *snapshot = instance.engine ? instance.engine->GetMuxFlowSnapshot() : ce::media::MuxFlowSnapshotV1{};
    return true;
}

MEDIAENGINE_API bool MediaEngine_TakeInjectSourceReadToken(uint64_t* sequence) {
    if (!sequence)
        return false;
    *sequence = 0;
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    return instance.engine && instance.engine->TakeInjectSourceReadToken(sequence);
}

MEDIAENGINE_API bool MediaEngine_IsInjectSourceReadComplete(uint64_t sequence) {
    return DefaultMediaEngineInstance().injectSourceReads.IsComplete(sequence);
}

MEDIAENGINE_API void MediaEngine_SetInjectSourceReadWakeEvent(HANDLE event) {
    DefaultMediaEngineInstance().injectSourceReads.SetWakeEvent(event);
}

MEDIAENGINE_API bool MediaEngine_FinishInjectSourceReads(uint32_t timeoutMs) {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    return !instance.engine || instance.engine->FinishInjectSourceReads(timeoutMs);
}

MEDIAENGINE_API int32_t MediaEngine_QueryInjectFrameCopyCompletion(uint64_t fenceHandle, uint64_t fenceValue,
                                                                   uint32_t sourcePid, uint32_t transportGeneration) {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    if (instance.engine) {
        return instance.engine->QueryInjectFrameCopyCompletion(reinterpret_cast<HANDLE>(fenceHandle), fenceValue,
                                                               sourcePid, transportGeneration);
    }
    return -1;
}

MEDIAENGINE_API void MediaEngine_SetInjectTransportGeneration(uint32_t transportGeneration) {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    if (instance.engine) {
        instance.engine->SetInjectTransportGeneration(transportGeneration);
    }
}

// The shared D3D11 device of the screen-grab path (MediaEngineInstance::GetSharedDevice). The
// encoder and the capture side use the same device so CopyResource works between them.
MEDIAENGINE_API ID3D11Device* MediaEngine_GetD3D11Device() {
    return DefaultMediaEngineInstance().GetSharedDevice();
}

MEDIAENGINE_API void MediaEngine_ReleaseSharedD3D11Device() {
    DefaultMediaEngineInstance().ReleaseSharedDevice();
}

// D3D11 Immediate Context Mutex
// Protects access to the immediate context shared between WGC thread and
// Encoder thread
MEDIAENGINE_API void MediaEngine_LockD3D11() {
    DefaultMediaEngineInstance().d3d11Mutex.lock();
}

MEDIAENGINE_API void MediaEngine_UnlockD3D11() {
    DefaultMediaEngineInstance().d3d11Mutex.unlock();
}

MEDIAENGINE_API void MediaEngine_SetSourcePrefers10Bit(bool prefer10Bit) {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    if (instance.engine)
        instance.engine->SetSourcePrefers10BitHint(prefer10Bit);
}

// Suppress encoder-side cursor composition while the capture source's frames
// already contain the cursor (DXGI duplication reporting a software/composed
// cursor) so the recording does not show a double cursor. Toggled by the
// capture layer on hardware/software cursor-plane transitions.
MEDIAENGINE_API void MediaEngine_SetCursorCompositionSuppressed(bool suppressed) {
    MediaEngineInstance& instance = DefaultMediaEngineInstance();
    std::lock_guard<std::recursive_mutex> apiLock(instance.apiMutex);
    if (instance.engine)
        instance.engine->SetCursorCompositionSuppressedHint(suppressed);
}

}  // extern "C"
