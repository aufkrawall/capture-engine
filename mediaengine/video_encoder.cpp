#include "video_encoder_internal.h"

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

static void FreeScopedAvFrame(AVFrame** frame) {
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
           (repeatFrameTexture != nullptr || (repeatSourceNeedsCursorRecompose && repeatSourceFrameTexture != nullptr));
}

void VideoEncoder::ResetRepeatFrameCache() {
    const bool hadCachedContent = repeatFrameTexture != nullptr || repeatSourceFrameTexture != nullptr;
    if (repeatFrameTexture) {
        repeatFrameTexture->Release();
        repeatFrameTexture = nullptr;
    }
    InvalidateRepeatSourceFrameTexture();
    repeatSourceCacheFailureLogged = false;
    repeatCursorRecomposeFallbackLogged = false;
    repeatSourceCacheKeyedMutexLogged = false;
    repeatSourceCacheKeyedAcquireFailCount = 0;
    if (hadCachedContent) {
        DLL_Log("[VideoEncoder] Repeat-frame cache invalidated for capture-source transition");
    }
}

bool VideoEncoder::WasLastFrameDeferred() const {
    return lastFrameDeferred.load(std::memory_order_relaxed);
}
