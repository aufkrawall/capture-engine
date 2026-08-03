    Impl()
        : itemCallbackState_(ce::CallbackEpoch<Impl>::Create()),
          frameCallbackState_(ce::CallbackEpoch<Impl>::Create()) {}

    ~Impl() {
        alive_.store(false, std::memory_order_release);
        StopCapture();
        UnsubscribeItemClosed();
        itemCallbackState_->DetachAndDrain();
        ReleaseTexturePool();
        SafeRelease(latestFrame_);
        SafeRelease(cachedTexture_);
        SafeRelease(d3dContext_);
        if (usingDedicatedCaptureDevice_) {
            SafeRelease(d3dDevice_);
        } else {
            d3dDevice_ = nullptr;
        }
        if (frameArrivedEvent_) {
            CloseHandle(frameArrivedEvent_);
            frameArrivedEvent_ = NULL;
        }
    }

    std::atomic<bool> alive_{true};
    std::shared_ptr<ce::CallbackEpoch<Impl>> itemCallbackState_;
    std::shared_ptr<ce::CallbackEpoch<Impl>> frameCallbackState_;
    winrt::GraphicsCaptureItem item_{nullptr};
    winrt::Direct3D11CaptureFramePool framePool_{nullptr};
    winrt::GraphicsCaptureSession session_{nullptr};
    winrt::IDirect3DDevice winrtDevice_{nullptr};

    ID3D11Texture2D* latestFrame_ = nullptr;
    std::deque<WGCCapturedFrame> pendingFrames_;
    static constexpr size_t kMaxPendingFrames = 48;

    // Texture pool for zero-copy pipeline: each frame gets its own texture
    // so the encoder can consume frame N while frame N+1 is being copied. The
    // size is budgeted at capture start; VRR/CFR smoothness needs more than the
    // old fixed 12 slots at high refresh, but must stay bounded by VRAM.
    std::vector<ID3D11Texture2D*> texturePool_;         // Encoder-device textures
    std::vector<ID3D11Texture2D*> captureTexturePool_;  // Capture-device views when split
    std::vector<IDXGIKeyedMutex*> captureTextureMutexPool_;
    std::vector<ID3D11RenderTargetView*> poolRenderTargetViews_;
    int32_t poolWidth_ = 0;
    int32_t poolHeight_ = 0;
    std::atomic<uint32_t> poolWriteIndex_{0};

    // Legacy single-texture for GetNextFrame (pull mode)
    ID3D11Texture2D* cachedTexture_ = nullptr;
    int32_t cachedWidth_ = 0;
    int32_t cachedHeight_ = 0;

    std::mutex frameMutex_;
    std::mutex frameProcessingMutex_;
    std::mutex callbackDrainMutex_;
    int64_t lastFrameTime_ = 0;
    bool frameReady_ = false;
    uint32_t frameWidth_ = 0;
    uint32_t frameHeight_ = 0;
    int64_t frameTimestamp_ = 0;

    ID3D11Device* encoderDevice_ = nullptr;  // Non-owning MediaEngine device
    ID3D11Device* d3dDevice_ = nullptr;      // Capture device (dedicated when split succeeds)
    ID3D11DeviceContext* d3dContext_ = nullptr;
    bool usingDedicatedCaptureDevice_ = false;

    winrt::event_token frameArrivedToken_;

    // Event signaled on frame arrival for efficient waiting
    HANDLE frameArrivedEvent_ = NULL;

    // QPC frequency cached for timestamp conversion
    int64_t qpcFreq_ = 0;
    std::atomic<int64_t> lastDeliveredSourceQpc_{0};
    std::atomic<int64_t> lastDeliveredRawSourceQpc_{0};
    std::atomic<int64_t> lastObservedRawSourceQpc_{0};
    std::atomic<int64_t> lastAssignedSourceQpc_{0};
    std::atomic<uint64_t> sourceEpoch_{0};

    // Callback function for direct frame processing.
    // Atomic raw function pointer: only static functions (or nullptr) are ever
    // stored, so std::function overhead and its non-atomic nature are avoided.
    // This eliminates the data race between the WinRT callback thread (reader)
    // and the main thread (writer during start/stop recording).
    using DirectFrameCallbackFn = void (*)(ID3D11Texture2D*, uint32_t, uint32_t, int64_t, int64_t, bool, bool, bool,
                                           const ce::cursor::SourcePointerObservation&, int32_t, int32_t, uint64_t,
                                           WgcPoolSlotLease&&);
    std::atomic<DirectFrameCallbackFn> frameCallback_{nullptr};
    using DirectCursorCallbackFn = void (*)(const ce::cursor::SourcePointerObservation&, int32_t, int32_t, uint32_t,
                                            uint32_t, uint64_t);
    std::atomic<DirectCursorCallbackFn> cursorCallback_{nullptr};
    std::atomic<uint32_t> callbackFrameCount_{0};
    std::atomic<uint32_t> inputFrameCount_{0};
    std::atomic<int64_t> lastCallbackStartQpc_{0};
    std::atomic<int64_t> callbackGapAvgUs_{0};
    std::atomic<int64_t> callbackGapMaxUs_{0};
    std::atomic<int64_t> callbackProcessAvgUs_{0};
    std::atomic<int64_t> callbackProcessMaxUs_{0};
    std::atomic<uint32_t> callbackDrainMaxCount_{0};
    std::atomic<int64_t> sourceIntervalAvgUs_{0};
    std::atomic<int64_t> sourceJitterAvgUs_{0};
    std::atomic<int64_t> sourceJitterMaxUs_{0};
    std::atomic<int64_t> sourceToCopyLatencyAvgUs_{0};
    std::atomic<int64_t> sourceToCopyLatencyMaxUs_{0};
    std::atomic<uint32_t> deliveredRatePerSec_{0};
    std::atomic<uint32_t> deliveredMin250Fps_{0};
    std::atomic<uint32_t> deliveredMin500Fps_{0};
    std::atomic<uint32_t> inputMin250Fps_{0};
    std::atomic<uint32_t> inputMin500Fps_{0};
    std::atomic<uint64_t> lastDeliveredRateSampleTickMs_{0};
    std::atomic<uint64_t> lastInputRateSampleTickMs_{0};

    // Frame throttle: skip CopyResource if we're ahead of target FPS
    uint32_t targetFps_ = 0;
    int64_t targetIntervalQPC_ = 0;   // Minimum QPC ticks between captured frames (0 = no throttle)
    int64_t lastCapturedQPC_ = 0;     // QPC of last frame we actually copied
    int64_t nextCaptureQPC_ = 0;      // Next QPC deadline that is allowed to perform a GPU copy
    uint32_t producerTargetFps_ = 0;  // WGC MinUpdateInterval target (0 = max-rate)
    int64_t producerIntervalQPC_ = 0;
    std::atomic<uint32_t> skippedFrameCount_{0};
    std::atomic<uint32_t> pacingSkipCount_{0};
    std::atomic<uint32_t> throttleSkipCount_{0};
    std::atomic<uint32_t> staleSkipCount_{0};
    std::atomic<uint32_t> staleDuplicateTimestampCount_{0};
    std::atomic<uint32_t> staleOutOfOrderTimestampCount_{0};
    std::atomic<uint32_t> normalizedDuplicateTimestampCount_{0};
    std::atomic<uint32_t> duplicateTimestampSkipCount_{0};
    std::atomic<uint32_t> cursorOnlySkipCount_{0};
    std::atomic<uint32_t> poolDropCount_{0};
    std::atomic<uint32_t> keyedMutexAcquireFailCount_{0};
    std::atomic<uint32_t> keyedMutexReleaseFailCount_{0};
    std::atomic<uint32_t> keyedMutexAbandonedReclaimCount_{0};
    std::atomic<uint32_t> splitDeviceFlushCount_{0};
    std::atomic<uint32_t> splitDeviceFlushSkippedCount_{0};
    std::atomic<uint32_t> poolSlotFastRewriteCount_{0};
    std::atomic<int64_t> lastPoolSlotRewriteUs_{0};
    std::vector<int64_t> poolSlotLastWriteQpc_;

    // External throttle flag (e.g., encoder bottlenecked)
    const std::atomic<bool>* throttleFlag_ = nullptr;

    // Dynamic format detection
    bool formatDetected_ = false;  // True after first frame format is checked

    // Perf tracking
    std::atomic<int64_t> lastCopyUs_{0};
    int64_t lastObservedSourceQpc_ = 0;
    int64_t smoothedSourceIntervalQpc_ = 0;
    uint64_t sourceIntervalSamples_ = 0;
    uint64_t sourceIntervalAccumUs_ = 0;
    uint64_t sourceJitterAccumUs_ = 0;
    uint32_t sourceJitterMaxUsValue_ = 0;
    uint64_t sourceToCopyLatencySamples_ = 0;
    uint64_t sourceToCopyLatencyAccumUs_ = 0;
    uint32_t sourceToCopyLatencyMaxUsValue_ = 0;
    ce::rate_window::SlidingRateWindow<> deliveredRateWindow_;
    ce::rate_window::SlidingRateWindow<> inputRateWindow_;

    DXGI_FORMAT poolSourceFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    DXGI_FORMAT poolFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    DXGI_FORMAT smoothnessSourceFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    DXGI_FORMAT smoothnessCopyFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    uint64_t smoothnessSourceBytesPerSurface_ = 0;
    uint64_t smoothnessCopyBytesPerSurface_ = 0;
    uint64_t smoothnessSourceEstimatedVramBytes_ = 0;
    uint64_t smoothnessCopyEstimatedVramBytes_ = 0;
    bool compactRetainedCopyActive_ = false;
    ID3D11VertexShader* poolCopyVS_ = nullptr;
    ID3D11PixelShader* poolCopyPS_ = nullptr;
    ID3D11SamplerState* poolCopySampler_ = nullptr;
    ID3D11Buffer* poolCopyCB_ = nullptr;
    ID3D11Query* gpuTimingDisjoint_ = nullptr;
    ID3D11Query* gpuTimingStart_ = nullptr;
    ID3D11Query* gpuTimingEnd_ = nullptr;
    bool gpuTimingPending_ = false;
    bool gpuTimingActive_ = false;
    ULONGLONG lastGpuTimingSampleTick_ = 0;
    int64_t gpuTimingSubmitQpc_ = 0;
    ID3D11Texture2D* poolCopyStagingTexture_ = nullptr;
    ID3D11ShaderResourceView* poolCopyStagingSrv_ = nullptr;
    uint32_t poolCopyStagingWidth_ = 0;
    uint32_t poolCopyStagingHeight_ = 0;
    DXGI_FORMAT poolCopyStagingFormat_ = DXGI_FORMAT_UNKNOWN;
    std::atomic<int64_t> lastPoolConvertUs_{0};
    winrt::DirectXPixelFormat capturePixelFormat_ = winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized;
    DXGI_FORMAT captureDxgiFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
    HWND targetWindow_ = nullptr;
    HMONITOR targetMonitor_ = nullptr;
    WgcItemCreationMethod itemCreationMethod_ = WgcItemCreationMethod::kNone;
    uint64_t itemCreationIdValue_ = 0;
    // DXGI Desktop Duplication backend: an alternative monitor-scope frame
    // source behind the same pool/ingress/CFR engine. When the preference is
    // set, StartCapture tries duplication first. Auto/8-bit operation may
    // fall back to WGC; explicit DXGI + 10-bit operation is strict.
    std::unique_ptr<DxgiDuplicationSource> dupSource_;
    bool useDuplicationBackend_ = false;
    std::string dupInitFailureReason_;
    bool useHighPrecisionCapture_ = false;
    bool requireHighPrecisionCapture_ = false;
    bool allowDuplicationFallback_ = true;
    // Deferred output rechecks run on the consumer thread while frame delivery
    // reads this flag on the WinRT/duplication callback thread.
    std::atomic<bool> captureIsHDR_{false};
    bool borderlessCapture_ = false;
    UINT outputBitsPerColor_ = 8;
    DXGI_COLOR_SPACE_TYPE outputColorSpace_ = DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709;
    std::atomic<bool> resetNeeded_{false};
    bool skipSplitDeviceFlush_ = false;
    bool sameDeviceCapture_ = false;
    bool allowLossyBgra8Pool_ = false;
    bool smoothnessBufferEnabled_ = true;
    uint32_t smoothnessOutputFps_ = 0;
    uint32_t smoothnessMaxMs_ = ce::capture_policy::kWgcSmoothnessBufferDefaultMaxMs;
    uint32_t smoothnessVramBudgetMb_ = ce::capture_policy::kWgcSmoothnessBufferDefaultVramBudgetMb;
    std::atomic<int> desiredGpuPriority_{0};
    uint32_t smoothnessSyncDelayFrames_ = 0;
    uint32_t smoothnessRetainedFrames_ = 0;
    uint32_t smoothnessRetainedFrameCap_ = 0;
    uint32_t sourceFramePoolBufferCount_ = ce::capture_policy::kWgcSmoothnessSourceFramePoolMinBuffers;
    uint32_t smoothnessBudgetSurfaceCount_ = 0;
    uint32_t smoothnessSafetySlots_ = ce::capture_policy::kWgcSmoothnessBufferPoolSafetyFrames;
    uint32_t smoothnessReservedFreeSlots_ = ce::capture_policy::GetWgcSmoothnessReservedFreeCopySlots();
    uint32_t texturePoolSlotCount_ = ce::capture_policy::kWgcSmoothnessBufferMinPoolFrames;
    uint64_t smoothnessEstimatedVramBytes_ = 0;
    bool smoothnessBudgetExhausted_ = false;
    uint32_t allocationLimitedPoolSlots_ = 0;
    uint32_t allocationLimitedSourceBuffers_ = 0;
    uint32_t allocationLimitWidth_ = 0;
    uint32_t allocationLimitHeight_ = 0;
    DXGI_FORMAT allocationLimitSourceFormat_ = DXGI_FORMAT_UNKNOWN;
    std::atomic<ULONGLONG> lastVideoMemoryLogTick_{0};
    std::atomic<uint32_t> videoMemoryOverBudgetEpisodes_{0};
    enum class VideoMemoryReservationMode : uint8_t { kOff, kMandatory, kFull };
    VideoMemoryReservationMode videoMemoryReservationMode_ = VideoMemoryReservationMode::kOff;
    uint64_t activeVideoMemoryReservationBytes_ = 0;
    std::shared_ptr<WgcPoolLeaseState> poolLeaseState_;
    uint64_t poolGeneration_ = 0;
    std::atomic<uint32_t> poolSlotOverwritePreventedCount_{0};
    std::atomic<uint32_t> poolSaturatedDropCount_{0};
    std::atomic<uint32_t> ingressAcceptedCount_{0};
    std::atomic<uint32_t> ingressDecimatedCount_{0};
    std::atomic<uint32_t> ingressAcceptedLowWaterCount_{0};
    std::atomic<uint32_t> ingressAcceptedRecoveryCount_{0};
    std::atomic<uint32_t> ingressAcceptedSourceBelowCount_{0};
    std::atomic<uint32_t> ingressAcceptedHealthyCount_{0};
    std::atomic<uint32_t> ingressAcceptedUniformPlayoutSoftReserveCount_{0};
    std::atomic<uint32_t> ingressAcceptedUniformPlayoutCreditCount_{0};
    std::atomic<uint32_t> ingressDecimatedSoftReserveCount_{0};
    std::atomic<uint32_t> ingressDecimatedHardReserveCount_{0};
    std::atomic<uint32_t> ingressDecimatedCreditCount_{0};
    std::atomic<uint32_t> ingressSoftReservePressureCount_{0};
    std::atomic<uint32_t> ingressHardReservePressureCount_{0};
    std::atomic<uint32_t> ingressRetainedFrames_{0};
    std::atomic<uint32_t> ingressRetainedFrameCap_{0};
    std::atomic<uint32_t> ingressLowWaterFrames_{0};
    std::atomic<bool> ingressRecovering_{false};
    std::atomic<bool> ingressUniformPlayoutOwnsSurplus_{false};
    std::atomic<uint32_t> ingressLastReason_{0};
    std::mutex ingressAdmissionMutex_;
    int64_t ingressCreditLastQpc_ = 0;
    double ingressCreditFrames_ = 1.0;
    std::mutex resetReasonMutex_;
    std::string resetReason_;
    winrt::event_token itemClosedToken_{};
    std::atomic<ULONGLONG> lastHDRCheckTick_{0};
    std::atomic<bool> hdrRecheckPending_{false};

    void FlagResetNeeded(const char* reason) {
        resetNeeded_.store(true, std::memory_order_release);
        if (reason && *reason) {
            std::lock_guard<std::mutex> lock(resetReasonMutex_);
            if (resetReason_.empty()) {
                resetReason_ = reason;
            }
        }
    }

    bool NeedsReset() const {
        return resetNeeded_.load(std::memory_order_acquire);
    }

    std::string ConsumeResetReason() {
        resetNeeded_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(resetReasonMutex_);
        std::string reason = resetReason_;
        resetReason_.clear();
        return reason;
    }

    void PerformHDRRecheck() {
        const ULONGLONG now = GetTickCount64();
        lastHDRCheckTick_.store(now, std::memory_order_relaxed);

        const HMONITOR monitor = ResolveTargetMonitor();
        if (!monitor)
            return;

        DXGI_OUTPUT_DESC1 desc1 = {};
        if (QueryOutputDesc1ForMonitor(monitor, desc1)) {
            bool newHDR = ::IsHdrOutputColorSpace(desc1.ColorSpace);
            if (newHDR != captureIsHDR_) {
                // The WGC frame-pool pixel format is immutable. Merely changing
                // the metadata flag would mislabel frames and apply the wrong
                // color conversion; recreate the source/pool on the new mode.
                LogInfo("[WGC] HDR state changed mid-capture: %s -> %s; requesting capture recreation",
                        captureIsHDR_ ? "HDR" : "SDR", newHDR ? "HDR" : "SDR");
                FlagResetNeeded("HDR output state changed");
            }
        }
    }

    // Periodically re-check HDR state (handles mid-capture HDR toggle in Windows settings)
    // but keep the DXGI probe off the WinRT callback hot path.
    void RequestHDRRecheckIfDue() {
        const ULONGLONG now = GetTickCount64();
        const ULONGLONG lastCheckTick = lastHDRCheckTick_.load(std::memory_order_relaxed);
        if (now - lastCheckTick < 2000) {
            return;
        }

        hdrRecheckPending_.store(true, std::memory_order_relaxed);
    }

    void MaybePerformDeferredHDRRecheck() {
        if (!hdrRecheckPending_.exchange(false, std::memory_order_relaxed)) {
            return;
        }

        const ULONGLONG now = GetTickCount64();
        const ULONGLONG lastCheckTick = lastHDRCheckTick_.load(std::memory_order_relaxed);
        if (now - lastCheckTick < 2000) {
            return;
        }

        PerformHDRRecheck();
    }

    const char* DescribeCaptureFormat() const {
        switch (captureDxgiFormat_) {
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                return "R16G16B16A16_FLOAT";
            case DXGI_FORMAT_R10G10B10A2_UNORM:
                return "R10G10B10A2_UNORM";
            case DXGI_FORMAT_B8G8R8A8_UNORM:
                return "B8G8R8A8_UNORM";
            default:
                return "UNKNOWN";
        }
    }

    uint32_t BytesPerPixelForFormat(DXGI_FORMAT format) const {
        switch (format) {
            case DXGI_FORMAT_R16G16B16A16_FLOAT:
                return 8;
            case DXGI_FORMAT_R10G10B10A2_UNORM:
            case DXGI_FORMAT_B8G8R8A8_UNORM:
            default:
                return 4;
        }
    }

    DXGI_FORMAT GetRetainedPoolFormat(DXGI_FORMAT sourceFormat) const {
        if (ce::video_format::ShouldApplySdrLinearToSrgbBeforeRgb10(sourceFormat, captureIsHDR_)) {
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        }
        return sourceFormat;
    }

    bool IsCompactRetainedCopy(DXGI_FORMAT sourceFormat, DXGI_FORMAT retainedFormat) const {
        return retainedFormat != sourceFormat;
    }

    void ReleasePoolConversionResources() {
        for (auto* rtv : poolRenderTargetViews_) {
            SafeRelease(rtv);
        }
        poolRenderTargetViews_.clear();
        SafeRelease(poolCopyStagingSrv_);
        SafeRelease(poolCopyStagingTexture_);
        poolCopyStagingWidth_ = 0;
        poolCopyStagingHeight_ = 0;
        poolCopyStagingFormat_ = DXGI_FORMAT_UNKNOWN;
        SafeRelease(poolCopyCB_);
        SafeRelease(poolCopySampler_);
        SafeRelease(poolCopyPS_);
        SafeRelease(poolCopyVS_);
        lastPoolConvertUs_.store(0, std::memory_order_relaxed);
    }

    void ReleaseGpuTimingResources() {
        SafeRelease(gpuTimingEnd_);
        SafeRelease(gpuTimingStart_);
        SafeRelease(gpuTimingDisjoint_);
        gpuTimingPending_ = false;
        gpuTimingActive_ = false;
        gpuTimingSubmitQpc_ = 0;
    }

    bool EnsurePoolCopyShader() {
        if (poolCopyVS_ && poolCopyPS_ && poolCopySampler_ && poolCopyCB_) {
            return true;
        }
        if (!d3dDevice_) {
            return false;
        }

        HMODULE d3dCompiler = ce::security::LoadSystemLibrary(L"d3dcompiler_47.dll");
        if (!d3dCompiler) {
            LogError("[WGC] Failed to load d3dcompiler_47.dll for retained-copy conversion");
            return false;
        }

        typedef HRESULT(WINAPI * PFN_D3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                                 LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        auto d3dCompile = reinterpret_cast<PFN_D3DCompile>(GetProcAddress(d3dCompiler, "D3DCompile"));
        if (!d3dCompile) {
            LogError("[WGC] Failed to resolve D3DCompile for retained-copy conversion");
            FreeLibrary(d3dCompiler);
            return false;
        }

        ID3DBlob* vsBlob = nullptr;
        ID3DBlob* psBlob = nullptr;
        ID3DBlob* errBlob = nullptr;
        HRESULT hr = d3dCompile(WGC_POOL_COPY_SHADER_SRC, strlen(WGC_POOL_COPY_SHADER_SRC), nullptr, nullptr, nullptr,
                                "VS_Main", "vs_4_0", 0, 0, &vsBlob, &errBlob);
        if (FAILED(hr)) {
            if (errBlob) {
                LogError("[WGC] Retained-copy VS compile failed: %s",
                         static_cast<const char*>(errBlob->GetBufferPointer()));
                errBlob->Release();
            }
            FreeLibrary(d3dCompiler);
            return false;
        }
        SafeRelease(errBlob);

        hr = d3dCompile(WGC_POOL_COPY_SHADER_SRC, strlen(WGC_POOL_COPY_SHADER_SRC), nullptr, nullptr, nullptr,
                        "PS_Main", "ps_4_0", 0, 0, &psBlob, &errBlob);
        if (FAILED(hr)) {
            if (errBlob) {
                LogError("[WGC] Retained-copy PS compile failed: %s",
                         static_cast<const char*>(errBlob->GetBufferPointer()));
                errBlob->Release();
            }
            SafeRelease(vsBlob);
            FreeLibrary(d3dCompiler);
            return false;
        }
        SafeRelease(errBlob);

        hr = d3dDevice_->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr, &poolCopyVS_);
        SafeRelease(vsBlob);
        if (FAILED(hr)) {
            LogError("[WGC] Retained-copy CreateVertexShader failed: 0x%08lX", (unsigned long)hr);
            SafeRelease(psBlob);
            FreeLibrary(d3dCompiler);
            return false;
        }

        hr = d3dDevice_->CreatePixelShader(psBlob->GetBufferPointer(), psBlob->GetBufferSize(), nullptr, &poolCopyPS_);
        SafeRelease(psBlob);
        if (FAILED(hr)) {
            LogError("[WGC] Retained-copy CreatePixelShader failed: 0x%08lX", (unsigned long)hr);
            SafeRelease(poolCopyVS_);
            FreeLibrary(d3dCompiler);
            return false;
        }
        FreeLibrary(d3dCompiler);

        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3D11_SAMPLER_DESC samplerDesc = {};
        samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
        samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        hr = d3dDevice_->CreateSamplerState(&samplerDesc, &poolCopySampler_);
        if (FAILED(hr)) {
            LogError("[WGC] Retained-copy CreateSamplerState failed: 0x%08lX", (unsigned long)hr);
            SafeRelease(poolCopyPS_);
            SafeRelease(poolCopyVS_);
            return false;
        }

        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = 16;
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        hr = d3dDevice_->CreateBuffer(&cbDesc, nullptr, &poolCopyCB_);
        if (FAILED(hr)) {
            LogError("[WGC] Retained-copy CreateBuffer failed: 0x%08lX", (unsigned long)hr);
            SafeRelease(poolCopySampler_);
            SafeRelease(poolCopyPS_);
            SafeRelease(poolCopyVS_);
            return false;
        }

        LogInfo("[WGC] Retained-copy conversion shader created");
        return true;
    }

    bool CreatePoolCopySourceSrv(ID3D11Texture2D* sourceTexture, const D3D11_TEXTURE2D_DESC& sourceDesc,
                                 DXGI_FORMAT inputSrvFormat, ID3D11ShaderResourceView** outSrv, bool* usedStaging) {
        if (!sourceTexture || !outSrv) {
            return false;
        }
        *outSrv = nullptr;
        if (usedStaging) {
            *usedStaging = false;
        }

        D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
        srvDesc.Format = inputSrvFormat;
        srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        srvDesc.Texture2D.MipLevels = 1;

        HRESULT hr = d3dDevice_->CreateShaderResourceView(sourceTexture, &srvDesc, outSrv);
        if (SUCCEEDED(hr) && *outSrv) {
            return true;
        }

        static std::atomic<uint32_t> directSrvFailLogCount{0};
        const uint32_t failLog = directSrvFailLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failLog <= 4) {
            LogInfo(
                "[WGC] Direct WGC source SRV unavailable for retained-copy conversion; using reusable staging "
                "(srcFmt=%s srvFmt=%s bind=0x%X misc=0x%X hr=0x%08lX)",
                DxgiFormatName(sourceDesc.Format), DxgiFormatName(inputSrvFormat), sourceDesc.BindFlags,
                sourceDesc.MiscFlags, (unsigned long)hr);
        }

        if (!poolCopyStagingTexture_ || poolCopyStagingWidth_ != sourceDesc.Width ||
            poolCopyStagingHeight_ != sourceDesc.Height || poolCopyStagingFormat_ != sourceDesc.Format) {
            SafeRelease(poolCopyStagingSrv_);
            SafeRelease(poolCopyStagingTexture_);

            D3D11_TEXTURE2D_DESC stagingDesc = {};
            stagingDesc.Width = sourceDesc.Width;
            stagingDesc.Height = sourceDesc.Height;
            stagingDesc.MipLevels = 1;
            stagingDesc.ArraySize = 1;
            stagingDesc.Format = sourceDesc.Format;
            stagingDesc.SampleDesc.Count = 1;
            stagingDesc.Usage = D3D11_USAGE_DEFAULT;
            stagingDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
            hr = d3dDevice_->CreateTexture2D(&stagingDesc, nullptr, &poolCopyStagingTexture_);
            if (FAILED(hr) || !poolCopyStagingTexture_) {
                LogError("[WGC] Failed to create retained-copy source staging texture: 0x%08lX", (unsigned long)hr);
                return false;
            }

            hr = d3dDevice_->CreateShaderResourceView(poolCopyStagingTexture_, &srvDesc, &poolCopyStagingSrv_);
            if (FAILED(hr) || !poolCopyStagingSrv_) {
                LogError("[WGC] Failed to create retained-copy source staging SRV: 0x%08lX", (unsigned long)hr);
                SafeRelease(poolCopyStagingTexture_);
                return false;
            }

            poolCopyStagingWidth_ = sourceDesc.Width;
            poolCopyStagingHeight_ = sourceDesc.Height;
            poolCopyStagingFormat_ = sourceDesc.Format;
            LogInfo("[WGC] Retained-copy source staging created: %ux%u fmt=%s bytes=%lluMB", sourceDesc.Width,
                    sourceDesc.Height, DxgiFormatName(sourceDesc.Format),
                    static_cast<unsigned long long>(
                        (ce::capture_policy::EstimateWgcSurfaceBytes(sourceDesc.Width, sourceDesc.Height,
                                                                     BytesPerPixelForFormat(sourceDesc.Format)) +
                         1024ull * 1024ull - 1ull) /
                        (1024ull * 1024ull)));
        }

        d3dContext_->CopyResource(poolCopyStagingTexture_, sourceTexture);
        poolCopyStagingSrv_->AddRef();
        *outSrv = poolCopyStagingSrv_;
        if (usedStaging) {
            *usedStaging = true;
        }
        return true;
    }

    bool RenderFrameToPoolSlot(ID3D11Texture2D* sourceTexture, const D3D11_TEXTURE2D_DESC& sourceDesc,
                               ID3D11RenderTargetView* targetRtv, bool linearToSrgb, bool* usedStaging) {
        if (!sourceTexture || !targetRtv || !d3dContext_ || !d3dDevice_) {
            return false;
        }
        if (!EnsurePoolCopyShader()) {
            return false;
        }

        const DXGI_FORMAT inputSrvFormat = ce::video_format::GetRgbShaderResourceViewFormat(sourceDesc.Format);
        if (inputSrvFormat == DXGI_FORMAT_UNKNOWN) {
            LogError("[WGC] Unsupported retained-copy source format for shader conversion: %s",
                     DxgiFormatName(sourceDesc.Format));
            return false;
        }

        ID3D11ShaderResourceView* sourceSrv = nullptr;
        bool staging = false;
        if (!CreatePoolCopySourceSrv(sourceTexture, sourceDesc, inputSrvFormat, &sourceSrv, &staging)) {
            return false;
        }
        if (usedStaging) {
            *usedStaging = staging;
        }

        D3D11_MAPPED_SUBRESOURCE mapped = {};
        HRESULT hr = d3dContext_->Map(poolCopyCB_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
        if (FAILED(hr) || !mapped.pData) {
            LogError("[WGC] Failed to map retained-copy constant buffer: 0x%08lX", (unsigned long)hr);
            SafeRelease(sourceSrv);
            return false;
        }
        uint32_t* cbData = static_cast<uint32_t*>(mapped.pData);
        cbData[0] = linearToSrgb ? 1u : 0u;
        cbData[1] = 0;
        cbData[2] = 0;
        cbData[3] = 0;
        d3dContext_->Unmap(poolCopyCB_, 0);

        D3D11ContextStateGuard stateGuard(d3dContext_);
        D3D11_VIEWPORT viewport = {};
        viewport.Width = static_cast<float>(sourceDesc.Width);
        viewport.Height = static_cast<float>(sourceDesc.Height);
        viewport.MaxDepth = 1.0f;
        d3dContext_->RSSetViewports(1, &viewport);
        d3dContext_->OMSetRenderTargets(1, &targetRtv, nullptr);
        d3dContext_->VSSetShader(poolCopyVS_, nullptr, 0);
        d3dContext_->PSSetShader(poolCopyPS_, nullptr, 0);
        d3dContext_->PSSetShaderResources(0, 1, &sourceSrv);
        d3dContext_->PSSetSamplers(0, 1, &poolCopySampler_);
        d3dContext_->PSSetConstantBuffers(0, 1, &poolCopyCB_);
        d3dContext_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        d3dContext_->IASetInputLayout(nullptr);
        d3dContext_->Draw(3, 0);

        ID3D11RenderTargetView* nullRtv = nullptr;
        ID3D11ShaderResourceView* nullSrv = nullptr;
        d3dContext_->OMSetRenderTargets(1, &nullRtv, nullptr);
        d3dContext_->PSSetShaderResources(0, 1, &nullSrv);
        SafeRelease(sourceSrv);
        return true;
    }

    ce::capture_policy::WgcSmoothnessSurfaceBudget ComputeTexturePoolBudget(uint32_t width, uint32_t height,
                                                                            DXGI_FORMAT format) const {
        const DXGI_FORMAT retainedFormat = GetRetainedPoolFormat(format);
        // The duplication backend has no consumer-owned source frame pool (the
        // OS holds the desktop image), so its entire VRAM budget funds retained
        // copy slots for the smoothness reservoir.
        const bool requiresSourceFramePool = !useDuplicationBackend_;
        return ce::capture_policy::ComputeWgcSmoothnessSurfaceBudget(
            smoothnessBufferEnabled_ ? smoothnessOutputFps_ : 0u, smoothnessBufferEnabled_ ? smoothnessMaxMs_ : 0u,
            width, height, BytesPerPixelForFormat(format), BytesPerPixelForFormat(retainedFormat),
