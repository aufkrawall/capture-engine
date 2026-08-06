#include "wgc_capture_internal.h"


#if HAS_WGC

void WGCCapture::Impl::FlagResetNeeded(const char* reason) {

        resetNeeded_.store(true, std::memory_order_release);
        if (reason && *reason) {
            std::lock_guard<std::mutex> lock(resetReasonMutex_);
            if (resetReason_.empty()) {
                resetReason_ = reason;
            }
        }

}

#endif


#if HAS_WGC

bool WGCCapture::Impl::NeedsReset() const {

        return resetNeeded_.load(std::memory_order_acquire);

}

#endif


#if HAS_WGC

std::string WGCCapture::Impl::ConsumeResetReason() {

        resetNeeded_.store(false, std::memory_order_release);
        std::lock_guard<std::mutex> lock(resetReasonMutex_);
        std::string reason = resetReason_;
        resetReason_.clear();
        return reason;

}

#endif


#if HAS_WGC

void WGCCapture::Impl::PerformHDRRecheck() {

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

#endif


#if HAS_WGC

void WGCCapture::Impl::RequestHDRRecheckIfDue() {

        const ULONGLONG now = GetTickCount64();
        const ULONGLONG lastCheckTick = lastHDRCheckTick_.load(std::memory_order_relaxed);
        if (now - lastCheckTick < 2000) {
            return;
        }

        hdrRecheckPending_.store(true, std::memory_order_relaxed);

}

#endif


#if HAS_WGC

void WGCCapture::Impl::MaybePerformDeferredHDRRecheck() {

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

#endif


#if HAS_WGC

void WGCCapture::Impl::SetVideoMemoryReservationBytes(uint64_t requestedBytes,  const char* stage) {

        if (!d3dDevice_)
            return;
        IDXGIDevice* dxgiDevice = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIAdapter3* adapter3 = nullptr;
        HRESULT hr = d3dDevice_->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (SUCCEEDED(hr) && dxgiDevice)
            hr = dxgiDevice->GetAdapter(&adapter);
        if (SUCCEEDED(hr) && adapter)
            hr = adapter->QueryInterface(IID_PPV_ARGS(&adapter3));
        DXGI_QUERY_VIDEO_MEMORY_INFO before = {};
        if (SUCCEEDED(hr) && adapter3)
            hr = adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &before);
        uint64_t clampedBytes = requestedBytes;
        if (SUCCEEDED(hr)) {
            const uint64_t reservable = before.CurrentReservation + before.AvailableForReservation;
            clampedBytes = std::min(requestedBytes, reservable);
            hr = adapter3->SetVideoMemoryReservation(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, clampedBytes);
        }
        DXGI_QUERY_VIDEO_MEMORY_INFO after = {};
        const HRESULT readbackHr =
            SUCCEEDED(hr) ? adapter3->QueryVideoMemoryInfo(0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &after) : hr;
        if (SUCCEEDED(hr) && SUCCEEDED(readbackHr)) {
            activeVideoMemoryReservationBytes_ = after.CurrentReservation;
            LogInfo(
                "[WGC] Video-memory reservation: stage=%s requested=%.1fMB clamped=%.1fMB actual=%.1fMB "
                "available=%.1fMB verified=%d",
                stage ? stage : "unknown", static_cast<double>(requestedBytes) / (1024.0 * 1024.0),
                static_cast<double>(clampedBytes) / (1024.0 * 1024.0),
                static_cast<double>(after.CurrentReservation) / (1024.0 * 1024.0),
                static_cast<double>(after.AvailableForReservation) / (1024.0 * 1024.0),
                after.CurrentReservation >= clampedBytes ? 1 : 0);
        } else {
            LogWarn(
                "[WGC] Video-memory reservation failed: stage=%s requested=%.1fMB hr=0x%08lX "
                "readbackHr=0x%08lX",
                stage ? stage : "unknown", static_cast<double>(requestedBytes) / (1024.0 * 1024.0),
                static_cast<unsigned long>(hr), static_cast<unsigned long>(readbackHr));
        }
        SafeRelease(adapter3);
        SafeRelease(adapter);
        SafeRelease(dxgiDevice);

}

#endif


#if HAS_WGC

void WGCCapture::Impl::ApplyConfiguredVideoMemoryReservation() {

        if (videoMemoryReservationMode_ == VideoMemoryReservationMode::kOff)
            return;
        const uint32_t mandatorySlots =
            std::max<uint32_t>(ce::capture_policy::kWgcSmoothnessBufferMinPoolFrames,
                               smoothnessSyncDelayFrames_ + smoothnessReservedFreeSlots_ + smoothnessSafetySlots_ + 1u);
        const uint64_t mandatoryBytes = smoothnessSourceEstimatedVramBytes_ +
                                        static_cast<uint64_t>(mandatorySlots) * smoothnessCopyBytesPerSurface_;
        const uint64_t fullBytes = smoothnessSourceEstimatedVramBytes_ +
                                   static_cast<uint64_t>(texturePool_.size()) * smoothnessCopyBytesPerSurface_;
        const bool full = videoMemoryReservationMode_ == VideoMemoryReservationMode::kFull;
        SetVideoMemoryReservationBytes(full ? fullBytes : mandatoryBytes, full ? "full" : "mandatory");

}

#endif


#if HAS_WGC

void WGCCapture::Impl::ResetVideoMemoryReservation() {

        if (activeVideoMemoryReservationBytes_ != 0)
            SetVideoMemoryReservationBytes(0, "teardown");
        activeVideoMemoryReservationBytes_ = 0;

}

#endif


#if HAS_WGC

void WGCCapture::Impl::EnableMultithreadProtection(ID3D11Device* device,  const char* label) {

        if (!device) {
            return;
        }

        ID3D11Multithread* multithread = nullptr;
        if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&multithread))) && multithread) {
            multithread->SetMultithreadProtected(TRUE);
            multithread->Release();
            LogInfo("[WGC] D3D11 multithread protection enabled on %s device", label);
        }

}

#endif


#if HAS_WGC

void WGCCapture::Impl::ApplyConfiguredGpuPriority(const char* role) {

        if (!d3dDevice_) {
            return;
        }
        const int requested = std::clamp(desiredGpuPriority_.load(std::memory_order_relaxed), -7, 7);
        IDXGIDevice* dxgiDevice = nullptr;
        HRESULT setHr = d3dDevice_->QueryInterface(IID_PPV_ARGS(&dxgiDevice));
        if (FAILED(setHr) || !dxgiDevice) {
            LogWarn("[WGC] GPU priority query failed: role=%s requested=%d hr=0x%08lX", role, requested,
                    static_cast<unsigned long>(setHr));
            return;
        }
        setHr = dxgiDevice->SetGPUThreadPriority(requested);
        INT actual = 0;
        const HRESULT readbackHr = SUCCEEDED(setHr) ? dxgiDevice->GetGPUThreadPriority(&actual) : setHr;
        if (SUCCEEDED(setHr) && SUCCEEDED(readbackHr) && actual == requested) {
            LogInfo("[WGC] GPU thread priority applied: role=%s requested=%d actual=%d verified=1 dedicated=%d", role,
                    requested, actual, usingDedicatedCaptureDevice_ ? 1 : 0);
        } else {
            LogWarn(
                "[WGC] GPU thread priority apply/readback failed: role=%s requested=%d actual=%d setHr=0x%08lX "
                "readbackHr=0x%08lX verified=0 dedicated=%d",
                role, requested, actual, static_cast<unsigned long>(setHr), static_cast<unsigned long>(readbackHr),
                usingDedicatedCaptureDevice_ ? 1 : 0);
        }
        dxgiDevice->Release();

}

#endif




#if HAS_WGC

bool WGCCapture::Impl::StartCapture(uint32_t& width,  uint32_t& height,  bool captureCursor) {


        if (useDuplicationBackend_) {
            if (StartDuplicationCapture(width, height)) {
                return true;
            }
            if (!allowDuplicationFallback_) {
                LogError("[WGC] Strict DXGI duplication contract failed (%s); WGC fallback is disabled",
                         dupInitFailureReason_.empty() ? "unknown reason" : dupInitFailureReason_.c_str());
                return false;
            }
            LogWarn("[WGC] DXGI duplication backend unavailable (%s); falling back to WGC monitor capture",
                    dupInitFailureReason_.empty() ? "unknown reason" : dupInitFailureReason_.c_str());
            useDuplicationBackend_ = false;
            if (!winrtDevice_ && !CreateWinRTDevice()) {
                LogError("[WGC] Failed to create WinRT device for duplication fallback");
                return false;
            }
            if (!item_ && !CreateForMonitor(ResolveTargetMonitor())) {
                LogError("[WGC] Failed to create WGC monitor item for duplication fallback");
                return false;
            }
        }

        if (!item_ || !winrtDevice_)
            return false;

        allocationLimitedPoolSlots_ = 0;
        allocationLimitedSourceBuffers_ = 0;
        allocationLimitSourceFormat_ = DXGI_FORMAT_UNKNOWN;
        resetNeeded_.store(false, std::memory_order_relaxed);
        {
            std::lock_guard<std::mutex> lock(resetReasonMutex_);
            resetReason_.clear();
        }

        // Cache QPC frequency for timestamp and throttle calculations
        LARGE_INTEGER freq;
        QueryPerformanceFrequency(&freq);
        qpcFreq_ = freq.QuadPart;
        ApplyFrameThrottleInterval();
        ApplyProducerInterval();
        if (targetFps_ > 0 && targetIntervalQPC_ > 0) {
            LogInfo("[WGC] Frame throttle active at %u fps (interval=%lld QPC ticks)", targetFps_,
                    (long long)targetIntervalQPC_);
        }
        if (producerTargetFps_ > 0 && producerIntervalQPC_ > 0) {
            LogInfo("[WGC] Producer cadence target active at %u fps (interval=%lld QPC ticks)", producerTargetFps_,
                    (long long)producerIntervalQPC_);
        }

        auto size = item_.Size();
        width = size.Width;
        height = size.Height;
        frameWidth_ = width;
        frameHeight_ = height;
        UpdateCaptureFormatSelection();
        UpdateSmoothnessBudget(width, height, captureDxgiFormat_, true);
        const DXGI_FORMAT requestedDxgiFormat = captureDxgiFormat_;
        const bool requestedHighPrecision = useHighPrecisionCapture_;
        const bool highPrecisionRequired = requireHighPrecisionCapture_ || captureIsHDR_;
        bool attemptedFp16Fallback = false;
        bool attemptedBgraFallback = false;

        // CRITICAL: Must use CreateFreeThreaded (not Create) because we have no
        // message pump! Create() requires a DispatcherQueue pumping messages for
        // callbacks to fire. CreateFreeThreaded() uses an internal worker thread
        // for callbacks. WGC source buffers are budgeted separately from CE copy
        // slots so delivery bursts and encoder texture lifetime do not fight over
        // one unbounded pool.
        auto tryCreateFramePool = [&](winrt::DirectXPixelFormat format) -> bool {
            uint32_t attemptBuffers = sourceFramePoolBufferCount_;
            for (;;) {
                try {
                    LogInfo("[WGC] Frame pool allocation attempt: format=%d sourceBuffers=%u", (int)format,
                            attemptBuffers);
                    framePool_ = winrt::Direct3D11CaptureFramePool::CreateFreeThreaded(
                        winrtDevice_, format, static_cast<int32_t>(attemptBuffers), size);
                    if (framePool_) {
                        if (attemptBuffers != sourceFramePoolBufferCount_) {
                            LogWarn(
                                "[WGC] Source frame pool reduced after memory exhaustion: configured=%u "
                                "allocated=%u",
                                sourceFramePoolBufferCount_, attemptBuffers);
                        }
                        sourceFramePoolBufferCount_ = attemptBuffers;
                        allocationLimitedSourceBuffers_ = attemptBuffers;
                        return true;
                    }
                    return false;
                } catch (const winrt::hresult_error& e) {
                    const HRESULT failureHr = e.code().value;
                    LogWarn("[WGC] Frame pool creation failed for format=%d buffers=%u: 0x%08X", (int)format,
                            attemptBuffers, (unsigned)failureHr);
                    framePool_ = nullptr;
                    const uint32_t minimumBuffers = ce::capture_policy::kWgcSmoothnessSourceFramePoolMinBuffers;
                    if (!IsAllocationExhaustion(failureHr) || attemptBuffers <= minimumBuffers)
                        return false;
                    attemptBuffers = std::max<uint32_t>(minimumBuffers, attemptBuffers / 2u);
                }
            }
        };

        if (!tryCreateFramePool(capturePixelFormat_)) {
            if (!captureIsHDR_ && capturePixelFormat_ == winrt::DirectXPixelFormat::R10G10B10A2UIntNormalized) {
                // R10 frame pools are rejected by the WinRT capture layer itself
                // (E_INVALIDARG regardless of buffer count or driver; WGC only
                // supports the BGRA8/FP16 family). One attempt is kept above for
                // future Windows versions; no buffer-count retry ladder.
                bool resolved = false;
                if (allowLossyBgra8Pool_ &&
                    ce::capture_policy::ShouldAllowBgra8WgcFallback(requireHighPrecisionCapture_, captureIsHDR_)) {
                    // BGRA8 as a compact pool format (4bpp instead of FP16's
                    // 8bpp), halving VRAM per source surface. Encoding stays
                    // 10-bit P010 (8-bit source content, transparent upconvert).
                    LogInfo(
                        "[WGC] R10 frame pool unsupported by WGC (API-level), "
                        "trying lossy BGRA8 pool (wgc_allow_lossy_bgra8_pool=true)");
                    const winrt::DirectXPixelFormat bgraFormat = winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized;
                    const DXGI_FORMAT bgraDxgi = DXGI_FORMAT_B8G8R8A8_UNORM;
                    if (tryCreateFramePool(bgraFormat)) {
                        LogInfo(
                            "[WGC] Compact BGRA8 frame pool created successfully. "
                            "Encoding stays 10-bit P010. VRAM saved: ~%.0fMB per source surface vs FP16.",
                            static_cast<double>(BytesPerPixelForFormat(DXGI_FORMAT_R16G16B16A16_FLOAT) -
                                                BytesPerPixelForFormat(bgraDxgi)) *
                                static_cast<double>(width) * static_cast<double>(height) / (1024.0 * 1024.0));
                        capturePixelFormat_ = bgraFormat;
                        captureDxgiFormat_ = bgraDxgi;
                        // Keep useHighPrecisionCapture_=true so encoding stays
                        // 10-bit P010.
                        resolved = true;
                    } else {
                        LogInfo("[WGC] Compact BGRA8 pool also failed, falling back to FP16.");
                    }
                }
                if (!resolved) {
                    LogWarn(
                        "[WGC] R10 frame pool unsupported by WGC (API-level), falling back to FP16. "
                        "FP16 at 2x VRAM cost per source surface preserves >8-bit source content losslessly "
                        "via shader conversion to R10 for retained copies.");
                    // SDR 10-bpc: FP16 preserves full >8-bit source content
                    // (e.g. a 10-bit game swapchain re-composed into the pool).
                    // Explicit 10-bit recording never reaches the compact
                    // BGRA8 branch above, so this preserves source precision.
                    attemptedFp16Fallback = true;
                    capturePixelFormat_ = winrt::DirectXPixelFormat::R16G16B16A16Float;
                    captureDxgiFormat_ = DXGI_FORMAT_R16G16B16A16_FLOAT;
                    UpdateSmoothnessBudget(width, height, captureDxgiFormat_, true);
                    if (smoothnessRetainedFrames_ > 0 &&
                        smoothnessRetainedFrames_ <
                            ce::capture_policy::GetWgcSmoothnessDesiredFrames(smoothnessOutputFps_, smoothnessMaxMs_)) {
                        const uint32_t desiredFrames =
                            ce::capture_policy::GetWgcSmoothnessDesiredFrames(smoothnessOutputFps_, smoothnessMaxMs_);
                        const uint32_t shortfall = desiredFrames - smoothnessRetainedFrames_;
                        const uint64_t neededBytes = static_cast<uint64_t>(shortfall) * smoothnessCopyBytesPerSurface_;
                        LogWarn(
                            "[WGC] FP16 pool cap-limited: shortfall=%u/%u frames (need ~%.0fMB more "
                            "copy VRAM or reduce source buffers). "
                            "Expected smoothness degraded: reservoir may starve under source dips.",
                            shortfall, desiredFrames, static_cast<double>(neededBytes) / (1024.0 * 1024.0));
                    }
                    // useHighPrecisionCapture_ stays true
                }
            } else if (highPrecisionRequired) {
                LogError("[WGC] Failed to create required high-precision frame pool for format=%s",
                         DescribeCaptureFormat());
                return false;
            } else {
                attemptedBgraFallback = true;
                capturePixelFormat_ = winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized;
                captureDxgiFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
                useHighPrecisionCapture_ = false;
                UpdateSmoothnessBudget(width, height, captureDxgiFormat_, true);
            }

            if (!tryCreateFramePool(capturePixelFormat_)) {
                if (capturePixelFormat_ != winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized) {
                    if (!ce::capture_policy::ShouldAllowBgra8WgcFallback(requireHighPrecisionCapture_, captureIsHDR_)) {
                        LogError(
                            "[WGC] Required high-precision frame pool failed after fallback attempts "
                            "(requested=%s finalAttempt=%s)",
                            requestedDxgiFormat == DXGI_FORMAT_R16G16B16A16_FLOAT
                                ? "R16G16B16A16_FLOAT"
                                : (requestedDxgiFormat == DXGI_FORMAT_R10G10B10A2_UNORM ? "R10G10B10A2_UNORM"
                                                                                        : "B8G8R8A8_UNORM"),
                            DescribeCaptureFormat());
                        return false;
                    }
                    attemptedBgraFallback = true;
                    capturePixelFormat_ = winrt::DirectXPixelFormat::B8G8R8A8UIntNormalized;
                    captureDxgiFormat_ = DXGI_FORMAT_B8G8R8A8_UNORM;
                    useHighPrecisionCapture_ = false;
                    UpdateSmoothnessBudget(width, height, captureDxgiFormat_, true);
                    if (!tryCreateFramePool(capturePixelFormat_)) {
                        LogError("[WGC] Failed to create frame pool in all capture formats");
                        return false;
                    }
                } else {
                    LogError("[WGC] Failed to create BGRA8 frame pool");
                    return false;
                }
            }
        }
        if (captureDxgiFormat_ != requestedDxgiFormat) {
            const char* requestedFormat =
                requestedDxgiFormat == DXGI_FORMAT_R16G16B16A16_FLOAT
                    ? "R16G16B16A16_FLOAT"
                    : (requestedDxgiFormat == DXGI_FORMAT_R10G10B10A2_UNORM ? "R10G10B10A2_UNORM" : "B8G8R8A8_UNORM");
            LogWarn("[WGC] Frame pool fallback: requested=%s final=%s hdr=%s highPrecision=%s tried(fp16=%d bgra8=%d)",
                    requestedFormat, DescribeCaptureFormat(), captureIsHDR_ ? "YES" : "NO",
                    requestedHighPrecision ? "YES" : "NO", attemptedFp16Fallback ? 1 : 0,
                    attemptedBgraFallback ? 1 : 0);
        }
        LogInfo("[WGC] Frame pool format: %s", DescribeCaptureFormat());

        // Allocate and compile everything whose dimensions and format are now
        // authoritative before registering the callback or starting WGC. This
        // keeps first-frame allocation/driver compilation out of the producer
        // callback during the most contention-sensitive startup interval.
        const ULONGLONG prewarmStart = GetTickCount64();
        const DXGI_FORMAT retainedFormat = GetRetainedPoolFormat(captureDxgiFormat_);
        const bool needsRetainedShader = IsCompactRetainedCopy(captureDxgiFormat_, retainedFormat);
        EnsureGpuTimingQueries();
        if ((needsRetainedShader && !EnsurePoolCopyShader()) || !EnsureTexturePool(width, height, captureDxgiFormat_)) {
            LogError("[WGC] Capture prewarm failed: sourceFormat=%s retainedFormat=%s shaderRequired=%d",
                     DxgiFormatName(captureDxgiFormat_), DxgiFormatName(retainedFormat), needsRetainedShader ? 1 : 0);
            framePool_.Close();
            framePool_ = nullptr;
            return false;
        }
        LogInfo(
            "[WGC] Capture prewarm complete: duration=%llums sourceFormat=%s retainedFormat=%s "
            "sourceBuffers=%u copySlots=%zu estimatedPool=%lluMB",
            static_cast<unsigned long long>(GetTickCount64() - prewarmStart), DxgiFormatName(captureDxgiFormat_),
            DxgiFormatName(retainedFormat), sourceFramePoolBufferCount_, texturePool_.size(),
            static_cast<unsigned long long>((smoothnessEstimatedVramBytes_ + 1024ull * 1024ull - 1ull) /
                                            (1024ull * 1024ull)));

        // Create event for frame arrival signaling (OBS-style immediate wake)
        // Auto-reset event ensures we wake once per signal
        if (!frameArrivedEvent_) {
            frameArrivedEvent_ = CreateEvent(NULL, FALSE, FALSE, NULL);  // Auto-reset
        }

        // A queued WinRT callback retains only this shared epoch gate. Stop
        // invalidates the epoch before releasing capture resources, so a
        // handler that starts late cannot dereference a destroyed Impl.
        const uint64_t callbackEpoch = frameCallbackState_->Begin(this);
        auto callbackState = frameCallbackState_;
        frameArrivedToken_ = framePool_.FrameArrived(
            [callbackState = std::move(callbackState), callbackEpoch](auto&& sender, auto&& args) {
                auto owner = callbackState->Enter(callbackEpoch);
                if (!owner) {
                    return;
                }
                owner->OnFrameArrived(sender, args);
            });

        // Create and start capture session
        session_ = framePool_.CreateCaptureSession(item_);

        // Try to request borderless access and enable border removal (like OBS)
        borderlessCapture_ = false;
        if (session_) {
            if (ShouldKeepWgcBorderByDiagnosticEnv()) {
                LogInfo("[WGC] Border removal skipped by CE_WGC_BORDER_MODE diagnostic override");
            } else if (EnsureBorderlessAccessRequested()) {
                try {
                    session_.IsBorderRequired(false);
                    borderlessCapture_ = true;
                    LogInfo("[WGC] Borderless access granted, border removal enabled");
                } catch (winrt::hresult_error const& e) {
                    borderlessCapture_ = false;
                    LogInfo("[WGC] Borderless access granted but border removal failed: 0x%08lX",
                            static_cast<unsigned long>(e.code().value));
                } catch (...) {
                    borderlessCapture_ = false;
                    LogInfo("[WGC] Borderless access granted but border removal failed");
                }
            } else {
                borderlessCapture_ = false;
                const int accessState = GetBorderlessAccessRequestState();
                if (accessState == kBorderlessAccessDenied) {
                    LogInfo("[WGC] Borderless access denied, keeping default border");
                } else {
                    LogInfo("[WGC] Borderless access unavailable, keeping default border");
                }
            }
        }

        // Configure native WGC cursor capture. WGC recording normally keeps
        // this disabled and composites the cursor in the encoder so the live
        // cursor can remain in the hardware plane.
        try {
            if (session_) {
                session_.IsCursorCaptureEnabled(captureCursor);
                LogInfo("[WGC] Native cursor capture: %s", captureCursor ? "YES" : "NO");
            }
        } catch (...) {
            // Not available on older Windows versions
            LogInfo("[WGC] IsCursorCaptureEnabled not available");
        }

        LogInfo(
            "[WGC] Capture session diagnostics: target=%s method=%s hwnd=0x%p hmon=0x%p itemId=0x%llx "
            "framePool=CreateFreeThreaded sourceBuffers=%u borderless=%d nativeCursorRequested=%d "
            "producerTargetFps=%u localThrottleFps=%u",
            targetWindow_ ? "window" : "monitor", WgcItemCreationMethodName(itemCreationMethod_), targetWindow_,
            targetMonitor_, static_cast<unsigned long long>(itemCreationIdValue_), sourceFramePoolBufferCount_,
            borderlessCapture_ ? 1 : 0, captureCursor ? 1 : 0, producerTargetFps_, targetFps_);

        int32_t originLeft = 0;
        int32_t originTop = 0;
        const char* originMode = nullptr;
        if (targetWindow_) {
            originMode = ResolveWindowCaptureOrigin(originLeft, originTop);
        } else if (GetCaptureOrigin(originLeft, originTop)) {
            originMode = "monitor";
        }
        LogInfo("[WGC] Capture origin diagnostics: target=%s originMode=%s origin=(%d,%d) itemSize=%ux%u",
                targetWindow_ ? "window" : "monitor", originMode ? originMode : "unresolved", originLeft, originTop,
                frameWidth_, frameHeight_);

        // Apply and verify the producer contract before capture starts. CFR uses
        // zero (max rate); any surplus compositor updates are cheaper and safer
        // to discard in the timestamp scheduler than through interval aliasing.
        ApplyMinUpdateInterval();

        session_.StartCapture();
        LogInfo("[WGC] Capture session started: %dx%d", width, height);
        return true;

}

#endif

