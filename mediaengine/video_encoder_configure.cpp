#include "video_encoder_internal.h"

namespace {
bool InitializeLiveNetwork() {
    static std::once_flag once;
    static int result = AVERROR_UNKNOWN;
    std::call_once(once, [] { result = avformat_network_init(); });
    return result >= 0;
}
}  // namespace

bool VideoEncoder::Init(const VideoConfig& config, int width, int height, int fps,
                        std::function<void(AVPacket*)> packetCallback) {
    // Clear handle failure cache from previous recording session
    video_encoder_g_HandleFailureCache.Clear();
    DLL_Log("[VideoEncoder] Init Entry - config.encoder=%s w=%d h=%d fps=%d", config.encoder.c_str(), width, height,
            fps);

    // Disable buffering to see logs immediately
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    DLL_Log("[VideoEncoder] Step 1: Setting member variables");
    this->width = width;
    this->height = height;
    this->captureCursor = config.captureCursor;
    this->gpuPriority = config.gpuPriority;
    this->onPacket = std::move(packetCallback);
    savedConfig = config;
    liveOutput = ce::live_stream::IsLiveStreamTarget(config.outputDir);
    liveOutputFailed.store(false, std::memory_order_relaxed);
    outputIoAbort.store(false, std::memory_order_relaxed);
    outputIoDeadlineMs.store(0, std::memory_order_relaxed);
    liveQueueLimitBytes = ce::live_stream::ComputeQueueBudgetBytes(0);
    if (liveOutput && ce::live_stream::IsValidLiveStreamTarget(config.outputDir)) {
        if (!InitializeLiveNetwork()) {
            DLL_Log("[LiveStream] Failed to initialize FFmpeg network support");
            return false;
        }
        int64_t videoBitsPerSecond = 0;
        std::string bitrateError;
        if (ce::video::ParseBitrateString(config.bitrate, &videoBitsPerSecond, &bitrateError)) {
            constexpr int64_t kAudioAndMuxOverheadBitsPerSecond = 512000;
            const int64_t totalBitsPerSecond =
                videoBitsPerSecond <= INT64_MAX - kAudioAndMuxOverheadBitsPerSecond
                    ? videoBitsPerSecond + kAudioAndMuxOverheadBitsPerSecond
                    : INT64_MAX;
            liveQueueLimitBytes = ce::live_stream::ComputeQueueBudgetBytes(totalBitsPerSecond);
        }
        DLL_Log("[LiveStream] Network output initialized endpoint=<redacted> queueBudget=%zuKB",
                liveQueueLimitBytes / 1024);
    }
    hdrPacketMetadataLogged = false;

    // Initialize cursor renderer if cursor capture enabled
    if (captureCursor) {
        cursorRenderer = std::make_unique<CursorRenderer>();
        DLL_Log("[VideoEncoder] Cursor capture enabled (renderer created)");
    }
    if (config.faceCamera.enabled) {
        faceCameraRenderer = std::make_unique<FaceCameraRenderer>(config.faceCamera);
        DLL_Log("[FaceCamera] Enabled for recording output; capture starts asynchronously with the session");
    } else {
        faceCameraRenderer.reset();
    }

    DLL_Log("[VideoEncoder] Step 2: Setting av_log level");
    // Native RTMP diagnostics can include the publish playpath (normally the stream key), and
    // provider-generated error text is not under our control. Live failures are reported through
    // the redacted operation/error-code path instead. Local recordings retain FFmpeg warnings.
    av_log_set_level(liveOutput ? AV_LOG_QUIET : AV_LOG_WARNING);

    DLL_Log("[VideoEncoder] Step 3: Deferring staging output reservation until recording start");
    outputReservation.CleanupOwnedFile();
    outputFilename.clear();

    DLL_Log("[VideoEncoder] Step 4: Calling avformat_alloc_output_context2");
    if (AllocateOutputContextForContainer(&fmtCtx, config) < 0) {
        DLL_Log("[VideoEncoder] Failed to alloc output context");
        return false;
    }
    DLL_Log("[VideoEncoder] Step 4 done, fmtCtx=%p", (void*)fmtCtx);

    DLL_Log("[VideoEncoder] Step 5: Finding encoder: %s", config.encoder.c_str());
    const AVCodec* codec = avcodec_find_encoder_by_name(config.encoder.c_str());
    if (!codec) {
        DLL_Log("[VideoEncoder] Codec not found: %s", config.encoder.c_str());
        return false;
    }
    DLL_Log("[VideoEncoder] Step 5 done, codec=%p name=%s", (void*)codec, codec->name);

    DLL_Log("[VideoEncoder] Step 6: Allocating codec context");
    codecCtx = avcodec_alloc_context3(codec);
    if (!codecCtx) {
        DLL_Log("[VideoEncoder] Failed to alloc codec context");
        return false;
    }
    DLL_Log("[VideoEncoder] Step 6 done, codecCtx=%p", (void*)codecCtx);

    DLL_Log("[VideoEncoder] Init Complete - returning true");
    // Defer device creation to EnsureDevice()
    return true;
}

void VideoEncoder::SetAdapterLUID(int32_t low, int32_t high) {
    this->luidLow = low;
    this->luidHigh = high;
}

void VideoEncoder::SetDimensions(uint32_t w, uint32_t h) {
    if (w > 0 && h > 0) {
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        this->width = w;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        this->height = h;
        DLL_Log("[VideoEncoder] SetDimensions: %dx%d", w, h);
    }
}

bool VideoEncoder::AdoptTextureDevice(ID3D11Texture2D* texture) {
    if (!texture) {
        return false;
    }

    ID3D11Device* texDevice = nullptr;
    texture->GetDevice(&texDevice);
    if (!texDevice) {
        DLL_Log("[VideoEncoder] Framegrab: Failed to get D3D11 device from texture");
        return false;
    }

    ID3D11Device5* adoptedDevice = nullptr;
    HRESULT hr = texDevice->QueryInterface(__uuidof(ID3D11Device5), (void**)&adoptedDevice);
    if (FAILED(hr) || !adoptedDevice) {
        DLL_Log("[VideoEncoder] Framegrab: Failed to query ID3D11Device5 from texture device. HR=%x", hr);
        texDevice->Release();
        return false;
    }

    ID3D11DeviceContext* immediateContext = nullptr;
    texDevice->GetImmediateContext(&immediateContext);
    ID3D11DeviceContext4* adoptedContext = nullptr;
    if (immediateContext) {
        hr = immediateContext->QueryInterface(__uuidof(ID3D11DeviceContext4), (void**)&adoptedContext);
        immediateContext->Release();
    } else {
        hr = E_NOINTERFACE;
    }
    texDevice->Release();

    if (FAILED(hr) || !adoptedContext) {
        DLL_Log("[VideoEncoder] Framegrab: Failed to query ID3D11DeviceContext4 from texture device. HR=%x", hr);
        adoptedDevice->Release();
        return false;
    }

    if (d3d11Context) {
        d3d11Context->Release();
    }
    if (d3d11Device) {
        d3d11Device->Release();
    }

    d3d11Device = adoptedDevice;
    d3d11Context = adoptedContext;
    return true;
}

void VideoEncoder::ReleaseInjectDeviceStateForScreenGrab() {
    const bool hadInjectLuid = (luidLow != 0 || luidHigh != 0);
    const bool hadSharedCapture = sharedCaptureTexturesCreated;
    if (!hadInjectLuid && !hadSharedCapture) {
        return;
    }

    DLL_Log("[VideoEncoder] ScreenGrab: Releasing inject device state (luid=%08x %08x shared=%d)", luidLow, luidHigh,
            hadSharedCapture ? 1 : 0);
    luidLow = 0;
    luidHigh = 0;

    if (pSharedMem) {
        pSharedMem->useEncoderTextures.store(false, std::memory_order_release);
        pSharedMem->encoderTextures.ready.store(false, std::memory_order_release);
        pSharedMem->encoderTextures.kmtReady.store(false, std::memory_order_release);
    }

    if (hadSharedCapture) {
        ReleasePreservedEncoderTextures();
        return;
    }

    for (int i = 0; i < SHARED_TEXTURE_SLOT_COUNT; i++) {
        if (cachedSharedTextures[i]) {
            cachedSharedTextures[i]->Release();
            cachedSharedTextures[i] = nullptr;
        }
        cachedTextureHandles[i] = nullptr;
    }

    if (cachedD3D11Fence) {
        cachedD3D11Fence->Release();
        cachedD3D11Fence = nullptr;
    }
    cachedFenceHandle = nullptr;
    cachedSourcePid = 0;

    if (bgraStagingTexture) {
        bgraStagingTexture->Release();
        bgraStagingTexture = nullptr;
    }

    StopFaceCamera();
    CleanupVideoProcessor();
    if (cursorRenderer) {
        cursorRenderer->Cleanup();
    }

    TrimD3D11Residency(d3d11Device, d3d11Context, "screen-grab-switch");
    if (d3d11Context) {
        d3d11Context->Release();
        d3d11Context = nullptr;
    }
    if (d3d11Device) {
        d3d11Device->Release();
        d3d11Device = nullptr;
    }
    if (hwFramesCtx) {
        av_buffer_unref(&hwFramesCtx);
    }
    if (hwDeviceCtx) {
        av_buffer_unref(&hwDeviceCtx);
    }
    if (d3d11DeviceCtx) {
        av_buffer_unref(&d3d11DeviceCtx);
    }
    if (d3d11FramesCtx) {
        av_buffer_unref(&d3d11FramesCtx);
    }
    initDone = false;
}

AVPixelFormat VideoEncoder::GetActiveD3D11SwFormat() const {
    if (!d3d11FramesCtx) {
        return AV_PIX_FMT_NONE;
    }

    const auto* framesCtx = reinterpret_cast<const AVHWFramesContext*>(d3d11FramesCtx->data);
    if (!framesCtx) {
        return AV_PIX_FMT_NONE;
    }
    return framesCtx->sw_format;
}
