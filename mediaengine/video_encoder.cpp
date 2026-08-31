#include "video_encoder_internal.h"

HandleFailureCache video_encoder_g_HandleFailureCache;

// Forward declarations - dllexport forces LTO to keep these functions
// D3D11 OpenShared* calls can throw SEH exceptions (0xE06D7363) for invalid handles.
// On MinGW, catch(...) CANNOT catch SEH exceptions. The failure cache pre-validates
// handles that have previously failed to prevent repeated crashes.
// CRITICAL: OpenSharedFence MUST only be called with handles that are known to be valid.
// The caller must use DuplicateHandle first - if it succeeds, the handle is valid.
// We also use the failure cache as a second line of defense.
extern "C" __declspec(dllexport) HRESULT __cdecl CallOpenSharedFence(ID3D11Device5* dev, HANDLE h, ID3D11Fence** out) {
    if (video_encoder_g_HandleFailureCache.ShouldSkipFence(h)) {
        return E_INVALIDARG;
    }
    // CRITICAL: MinGW catch(...) CANNOT catch D3D11's SEH exceptions (0xE06D7363).
    // D3D11's OpenSharedFence calls __fastfail on invalid handles, killing the process.
    // We use DuplicateHandle to validate the handle BEFORE calling D3D11.
    // If DuplicateHandle fails, the handle is invalid and we skip the call entirely.
    // If it succeeds, the handle is valid in this process and D3D11 should accept it.
    if (h == NULL || h == INVALID_HANDLE_VALUE) {
        return E_INVALIDARG;
    }

    HRESULT hr = E_FAIL;
    try {
        hr = dev->OpenSharedFence(h, IID_PPV_ARGS(out));

    } catch (...) {
        hr = E_FAIL;
    }
    if (FAILED(hr)) {
        video_encoder_g_HandleFailureCache.RecordFenceFailure(h);
    }
    return hr;
}

extern "C" __declspec(dllexport) HRESULT __cdecl CallOpenSharedResource(ID3D11Device5* dev, HANDLE h, REFIID riid,
                                                                        void** out) {
    if (h == NULL || h == INVALID_HANDLE_VALUE) {
        return E_INVALIDARG;
    }
    HRESULT hr = E_FAIL;
    try {
        hr = dev->OpenSharedResource(h, riid, out);
    } catch (...) {
        hr = E_FAIL;
    }
    if (FAILED(hr)) {
        video_encoder_g_HandleFailureCache.RecordTextureFailure(h);
    }
    return hr;
}

extern "C" __declspec(dllexport) HRESULT __cdecl CallOpenSharedResource1(ID3D11Device5* dev, HANDLE h, REFIID riid,
                                                                         void** out) {
    if (h == NULL || h == INVALID_HANDLE_VALUE) {
        return E_INVALIDARG;
    }
    HRESULT hr = E_FAIL;
    try {
        hr = dev->OpenSharedResource1(h, riid, out);
    } catch (...) {
        hr = E_FAIL;
    }
    if (FAILED(hr)) {
        video_encoder_g_HandleFailureCache.RecordTextureFailure(h);
    }
    return hr;
}

HANDLE NormalizeSourceHandleForWow64(HANDLE handle, uint32_t sourcePid) {
#if defined(_WIN64)
    if (!handle || sourcePid == 0) {
        return handle;
    }

    static std::mutex s_bitnessMutex;
    static std::unordered_map<uint32_t, bool> s_isWow64ByPid;

    bool isWow64Source = false;
    {
        std::lock_guard<std::mutex> lock(s_bitnessMutex);
        auto it = s_isWow64ByPid.find(sourcePid);
        if (it != s_isWow64ByPid.end()) {
            isWow64Source = it->second;
        } else {
            ce::HandleGuard hProcess(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, sourcePid));
            if (hProcess) {
                BOOL wow64 = FALSE;
                if (IsWow64Process(hProcess.get(), &wow64)) {
                    isWow64Source = (wow64 == TRUE);
                }
            }
            s_isWow64ByPid[sourcePid] = isWow64Source;
        }
    }

    const uint64_t rawHandle = static_cast<uint64_t>(reinterpret_cast<uintptr_t>(handle));
    if (!isWow64Source) {
        // Some drivers publish KMT handles with bit31 set but without canonical
        // sign-extension in 64-bit IPC transport.
        if ((rawHandle >> 32) == 0 && (rawHandle & 0x80000000ull) != 0) {
            const int64_t signExtended = static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(rawHandle)));
            if (signExtended != static_cast<int64_t>(rawHandle)) {
                static std::atomic<int> s_canonicalizeLogCount{0};
                if (s_canonicalizeLogCount.fetch_add(1, std::memory_order_relaxed) < 6) {
                    DLL_Log("[VideoEncoder] Canonicalizing shared handle for PID %u: %p -> %p", sourcePid,
                            (HANDLE)(uintptr_t)rawHandle, (HANDLE)(uint64_t)signExtended);
                }
                return reinterpret_cast<HANDLE>(static_cast<uint64_t>(signExtended));
            }
        }
        return handle;
    }

    const int64_t signExtended = static_cast<int64_t>(static_cast<int32_t>(static_cast<uint32_t>(rawHandle)));
    if (signExtended != static_cast<int64_t>(rawHandle)) {
        static std::atomic<int> s_normalizeLogCount{0};
        if (s_normalizeLogCount.fetch_add(1, std::memory_order_relaxed) < 6) {
            DLL_Log("[VideoEncoder] WOW64 handle normalized for PID %u: %p -> %p", sourcePid,
                    (HANDLE)(uintptr_t)rawHandle, (HANDLE)(uint64_t)signExtended);
        }
    }
    return reinterpret_cast<HANDLE>(static_cast<uint64_t>(signExtended));
#else
    (void)sourcePid;
    return handle;
#endif
}

ce::capture_output::ReservedCaptureOutput ReserveOutputStagingFile(const VideoConfig& config) {
    const fs::path exeDir = ce::capture_output::GetExecutableDirectory();
    const fs::path outDir = ce::capture_output::ResolveCaptureDirectory(config.outputDir, exeDir);
    const std::wstring ext(config.container.begin(), config.container.end());
    auto reservation = ce::capture_output::ReservedCaptureOutput::Reserve(outDir, L"capture_stage", ext);
    if (reservation) {
        DLL_Log("[VideoEncoder] Reserved unpublished staging output: %s",
                ce::privacy::CollapsePathForLog(reservation.Utf8Path()).c_str());
    } else {
        DLL_Log("[VideoEncoder] ERROR: Could not reserve a collision-safe staging output in: %s",
                ce::privacy::CollapsePathForLog(outDir.string()).c_str());
    }
    return reservation;
}

int AllocateOutputContextForContainer(AVFormatContext** formatContext, const VideoConfig& config) {
    if (ce::live_stream::IsLiveStreamTarget(config.outputDir))
        return avformat_alloc_output_context2(formatContext, nullptr, "flv", nullptr);
    const std::string formatHint = "capture." + config.container;
    return avformat_alloc_output_context2(formatContext, nullptr, nullptr, formatHint.c_str());
}

int64_t RoundUsToMs(int64_t valueUs) {
    if (valueUs >= 0) {
        return (valueUs + 500) / 1000;
    }
    return (valueUs - 500) / 1000;
}

// Global stats for frame analysis
int64_t video_encoder_g_lastFramePts = -1;

int64_t video_encoder_g_framesEncoded = 0;

// static int64_t g_framesDropped = 0;
double video_encoder_g_totalFenceWait = 0;

double video_encoder_g_totalColorConvert = 0;

double video_encoder_g_totalEncode = 0;

double video_encoder_g_maxFrameTime = 0;

int video_encoder_g_slowFrameCount = 0;  // Frames taking > 2x expected time

void FreeScopedAvFrame(AVFrame** frame) {
    if (frame && *frame) {
        av_frame_free(frame);
    }
}

VideoEncoder::VideoEncoder()
    : fmtCtx(nullptr),
      codecCtx(nullptr),
      stream(nullptr),
      hwDeviceCtx(nullptr),
      hwFramesCtx(nullptr),
      d3d11DeviceCtx(nullptr),
      d3d11FramesCtx(nullptr),
      d3d11Device(nullptr),
      d3d11Context(nullptr),
      luidLow(0),
      luidHigh(0),
      initDone(false),
      currentIsHDR(false),
      currentUse10BitInput(false),
      fileOpened(false),
      recordingRequested(false),
      isStopping(false),
      flushRequested(false),
      codecOpenFailed(false),
      startPts(-1),
      width(0),
      height(0),
      cachedSourcePid(0),
      lastEncodeTimeUs(0),
      fenceEvent(nullptr),
      videoDevice(nullptr),
      videoContext(nullptr),
      videoProcessor(nullptr),
      videoProcessorEnum(nullptr),
      inputView(nullptr),
      videoProcessorInit(false) {}

VideoEncoder::~VideoEncoder() {
    try {
    Stop();  // Triiger async stop

    // Destructor MUST be synchronous to ensure no threads are running
    // and all resources are safely released.
    if (writerThread.joinable()) {
        DLL_Log("[VideoEncoder] Destructor: Waiting for async writer to finish...");
        writerThread.join();
    }

    if (fenceEvent) {
        CloseHandle(fenceEvent);
        fenceEvent = nullptr;
        }
    } catch (...) {
        DLL_Log("[VideoEncoder] Suppressed exception during destruction");
    }
}

int64_t VideoEncoder::GetExpectedFinalDurationUs() const {
    if (lastAssignedVideoPts < 0)
        return 0;

    if (savedConfig.useVFR) {
        return lastAssignedVideoPts + (1000000 / (savedConfig.fps > 0 ? savedConfig.fps : 60));
    } else {
        int fps = (codecCtx && codecCtx->framerate.num > 0) ? codecCtx->framerate.num : savedConfig.fps;
        if (fps <= 0)
            fps = 60;
        return av_rescale(lastAssignedVideoPts + 1, 1000000, fps);
    }
}

int64_t VideoEncoder::GetAssignedCfrFrameCount() const {
    return !savedConfig.useVFR && lastAssignedVideoPts >= 0 ? lastAssignedVideoPts + 1 : 0;
}

int VideoEncoder::GetConfiguredFps() const {
    return savedConfig.fps > 0 ? savedConfig.fps : 0;
}

int64_t VideoEncoder::GetEncodedDurationUs() const {
    int64_t encodedUs = encodedDurationUs.load(std::memory_order_relaxed);
    if (encodedUs > 0) {
        return encodedUs;
    }

    if (!codecCtx || codecCtx->framerate.num == 0)
        return 0;

    // Fallback for early startup before first packet is emitted.
    if (lastAssignedVideoPts >= 0) {
        return GetExpectedFinalDurationUs();
    }
    return av_rescale(encodeFrameCounter, 1000000 * (int64_t)codecCtx->framerate.den, codecCtx->framerate.num);
}

int64_t VideoEncoder::GetLastFrameEncodeTimeUs() const {
    return lastEncodeTimeUs;
}

int64_t VideoEncoder::GetLastFrameFenceWaitUs() const {
    return lastFenceWaitUs;
}

bool VideoEncoder::CanRepeatLastFrame() const {
    return recordingRequested &&
           (repeatFrameTexture != nullptr ||
            (repeatSourceNeedsOverlayRecompose && repeatSourceFrameTexture != nullptr));
}

void VideoEncoder::ResetRepeatFrameCache() {
    const bool hadCachedContent = repeatFrameTexture != nullptr || repeatSourceFrameTexture != nullptr;
    if (repeatFrameTexture) {
        repeatFrameTexture->Release();
        repeatFrameTexture = nullptr;
    }
    InvalidateRepeatSourceFrameTexture();
    repeatSourceCacheFailureLogged = false;
    repeatOverlayRecomposeFallbackLogged = false;
    repeatSourceCacheKeyedMutexLogged = false;
    repeatSourceCacheKeyedAcquireFailCount = 0;
    if (hadCachedContent) {
        DLL_Log("[VideoEncoder] Repeat-frame cache invalidated for capture-source transition");
    }
}

bool VideoEncoder::WasLastFrameDeferred() const {
    return lastFrameDeferred.load(std::memory_order_relaxed);
}
