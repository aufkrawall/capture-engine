#include "overlay_adapter_internal.h"

void OverlayAdapter::RenderOverlay(int viewportWidth, int viewportHeight) {
    std::lock_guard<std::mutex> lock(stateMutex);
    static int renderLogCount = 0;
    if (renderLogCount < 5) {
        HookLog("[Overlay] RenderOverlay#%d: init=%d renderer=%p ipc=%p shm=%p showOverlay=%d vp=%dx%d", renderLogCount,
                initialized ? 1 : 0, (void*)renderer, (void*)ipc, ipc ? (void*)ipc->GetSharedMem() : nullptr,
                (ipc && ipc->GetSharedMem()) ? ipc->GetSharedMem()->ReadOverlayConfig().showOverlay : -1, viewportWidth,
                viewportHeight);
        renderLogCount++;
    }

    if (!initialized || !renderer) {
        if (renderLogCount < 5)
            HookLog("[Overlay] RenderOverlay: early return - not initialized or no renderer");
        return;
    }

    if (!ipc || !ipc->GetSharedMem()) {
        if (renderLogCount < 5)
            HookLog("[Overlay] RenderOverlay: early return - no IPC or shared memory");
        return;
    }
    auto* sharedMem = ipc->GetSharedMem();
    auto cfg = sharedMem->ReadOverlayConfig();
    if (!cfg.showOverlay) {
        if (renderLogCount < 5)
            HookLog("[Overlay] RenderOverlay: early return - showOverlay is false");
        return;
    }

    // Update throttling
    DWORD now = GetTickCount();
    bool shouldUpdate = (now - lastUpdateTime) >= cfg.textUpdateInterval;
    if (shouldUpdate) {
        lastUpdateTime = now;
        if (metrics) {
            cachedFPS = metrics->GetCurrentFPS();
            cachedAvgFPS = metrics->GetAverageFPS();
            cached1PercentLow = metrics->Get1PercentLowFPS();
            cached01PercentLow = metrics->Get01PercentLowFPS();
        }
        if (cfg.showCPU || cfg.showRAM || cfg.showGPU || cfg.showVRAM) {
            SystemMetricsCollector::Get().Update();
            cachedSystemMetrics = SystemMetricsCollector::Get().GetMetrics();
        }
    }

    FrameLayoutSnapshot frameLayout = {};
    frameLayout.fgActive = cfg.showFG && metrics && metrics->IsFGActive();
    frameLayout.reserveFGSpace = cfg.showFG && reserveInactiveFGSpace;
    if (frameLayout.fgActive && metrics) {
        frameLayout.fgMultiplier = metrics->GetFGMultiplier();
        std::snprintf(frameLayout.fgLabel, sizeof(frameLayout.fgLabel), "%s", metrics->GetFGTypeLabel());
        frameLayout.fgBaseFPS = metrics->GetFGBaseFPS();
        frameLayout.fgOutputFPS = metrics->GetFGOutputFPS();
    } else if (frameLayout.reserveFGSpace) {
        frameLayout.fgMultiplier = 4;
        std::snprintf(frameLayout.fgLabel, sizeof(frameLayout.fgLabel), "DLSS FG");
    }
    if (frameLayout.fgOutputFPS < 1.0f)
        frameLayout.fgOutputFPS = cachedFPS;
    if (frameLayout.fgBaseFPS < 1.0f) {
        frameLayout.fgBaseFPS =
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            frameLayout.fgMultiplier >= 2 ? frameLayout.fgOutputFPS / frameLayout.fgMultiplier : cachedFPS;
    }

    frameLayout.recordingActive = sharedMem->runtimeState.isRecording.load(std::memory_order_acquire);
    frameLayout.recordingAudioOnly = sharedMem->runtimeState.audioOnly.load(std::memory_order_acquire);
    const RecordingStartIntent recordingStartIntent = sharedMem->runtimeState.GetRecordingStartIntent();
    frameLayout.recordingState = ce::recording_indicator::SelectState(
        frameLayout.recordingActive, frameLayout.recordingAudioOnly, recordingStartIntent);
    frameLayout.recordingStatusDark =
        sharedMem->runtimeState.HasRuntimeFlag(kCaptureRuntimeFlagStatusOverlayDarkForCapture) &&
        ce::recording_indicator::IsStarting(frameLayout.recordingState);
    uint64_t nowTick64 = GetTickCount64();
    if (cfg.showRecording && frameLayout.recordingActive) {
        int64_t startTime = sharedMem->runtimeState.recordingStartTime.load(std::memory_order_acquire);
        if (startTime > 0) {
            frameLayout.recordingSeconds = (nowTick64 - startTime) / 1000;
        }
        uint32_t overloadFlags = sharedMem->runtimeState.encoderOverloadFlags.load(std::memory_order_relaxed);
        const uint32_t captureHealthFlags =
            sharedMem->runtimeState.wgcCaptureHealthFlags.load(std::memory_order_relaxed);
        const uint32_t recordingHealthFlags =
            sharedMem->runtimeState.recordingHealthFlags.load(std::memory_order_relaxed);
        const uint32_t warningKind = ce::capture_policy::SelectWgcOverlayWarningKind(
            overloadFlags, captureHealthFlags, recordingHealthFlags);
        if (warningKind == ce::capture_policy::kOverlayWarningNone &&
            ce::capture_policy::IsWgcCaptureLimitedForOverlay(captureHealthFlags)) {
            lastEncoderOverloadTick = 0;
            lastRecordingWarningKind = ce::capture_policy::kOverlayWarningNone;
        } else if (warningKind != ce::capture_policy::kOverlayWarningNone) {
            lastEncoderOverloadTick = nowTick64;
            lastRecordingWarningKind = warningKind;
        }
    } else {
        lastEncoderOverloadTick = 0;
        lastRecordingWarningKind = ce::capture_policy::kOverlayWarningNone;

    }
    frameLayout.showOverloadWarning = (lastEncoderOverloadTick != 0) && ((nowTick64 - lastEncoderOverloadTick) <= 5000);
    frameLayout.recordingWarningKind =
        frameLayout.showOverloadWarning ? lastRecordingWarningKind : ce::capture_policy::kOverlayWarningNone;
    frameLayout.recordingTargetFps = sharedMem->runtimeState.wgcTargetFps.load(std::memory_order_relaxed);
    frameLayout.recordingSustainFpsX100 = sharedMem->runtimeState.encoderSustainFpsX100.load(std::memory_order_relaxed);

    const uint64_t notificationExpiry = sharedMem->runtimeState.notificationExpiry.load(std::memory_order_acquire);
    frameLayout.notificationType = sharedMem->runtimeState.notificationType.load(std::memory_order_relaxed);
    const bool recordingFinalizationNotification =
        frameLayout.notificationType >= static_cast<uint32_t>(OverlayNotificationType::RecordingFinalizing) &&
        frameLayout.notificationType <= static_cast<uint32_t>(OverlayNotificationType::RecordingFailed);
    frameLayout.notificationVisible =
        notificationExpiry > nowTick64 && frameLayout.notificationType != 0 &&
        (!recordingFinalizationNotification || frameLayout.recordingState == ce::recording_indicator::State::Idle);

    ce::overlay_layout::RowInputs rowInputs = {};
    rowInputs.showGPU = cfg.showGPU;
    rowInputs.showCPU = cfg.showCPU;
    rowInputs.showVRAM = cfg.showVRAM;
    rowInputs.showRAM = cfg.showRAM;
    rowInputs.showFPS = cfg.showFPS;
    rowInputs.showFPSAverages = cachedAvgFPS > 0.0f && cached1PercentLow > 0.0f;
    rowInputs.showFG = cfg.showFG;
    rowInputs.fgActive = frameLayout.fgActive;
    rowInputs.reserveFGSpace = frameLayout.reserveFGSpace;
    rowInputs.showRecording = cfg.showRecording;
    rowInputs.recordingActive = frameLayout.recordingActive;
    rowInputs.recordingStarting =
        ce::recording_indicator::IsStarting(frameLayout.recordingState) && !frameLayout.recordingStatusDark;
    rowInputs.notificationVisible = frameLayout.notificationVisible;
    frameLayout.rowMask = ce::overlay_layout::BuildOverlayRowMask(rowInputs);
    frameLayout.rowCount = CountOverlayRows(frameLayout.rowMask);

    PresentDebugSample* activeDebugSample = PerfLogger::Get().GetActiveDebugSample();
    bool showGraph = cfg.showFrameTime && metrics;
    bool shouldRefreshGraph = showGraph;
    bool viewportChanged = (viewportWidth != lastViewportWidth) || (viewportHeight != lastViewportHeight);
    bool configChanged = !hasRenderedConfig || !overlay_adapter_OverlayConfigEquals(cfg, lastRenderedConfig);
    const bool rowSetChanged = !hasLastFrameLayout || frameLayout.rowMask != lastFrameLayout.rowMask;
    const bool fgIdentityChanged = !hasLastFrameLayout || frameLayout.fgActive != lastFrameLayout.fgActive ||
                                   frameLayout.reserveFGSpace != lastFrameLayout.reserveFGSpace ||
                                   frameLayout.fgMultiplier != lastFrameLayout.fgMultiplier ||
                                   std::strcmp(frameLayout.fgLabel, lastFrameLayout.fgLabel) != 0;
    const bool recordingChanged = !hasLastFrameLayout ||
                                  frameLayout.recordingState != lastFrameLayout.recordingState ||
                                  frameLayout.recordingStatusDark != lastFrameLayout.recordingStatusDark ||
                                  frameLayout.recordingActive != lastFrameLayout.recordingActive ||
                                  frameLayout.recordingAudioOnly != lastFrameLayout.recordingAudioOnly ||
                                  frameLayout.recordingSeconds != lastFrameLayout.recordingSeconds ||
                                  frameLayout.showOverloadWarning != lastFrameLayout.showOverloadWarning ||
                                  frameLayout.recordingWarningKind != lastFrameLayout.recordingWarningKind;
    const bool notificationChanged = !hasLastFrameLayout ||
                                     frameLayout.notificationVisible != lastFrameLayout.notificationVisible ||
                                     frameLayout.notificationType != lastFrameLayout.notificationType;
    bool dynamicStateChanged = rowSetChanged || fgIdentityChanged || recordingChanged || notificationChanged;
    bool needRebuild = !hasCachedFrame || shouldUpdate || shouldRefreshGraph || viewportChanged || configChanged ||
                       dynamicStateChanged || layoutDirty;
    const bool refreshLayout = shouldUpdate || layoutDirty || configChanged || rowSetChanged || fgIdentityChanged;
    static int renderPathLogCount = 0;
    if (renderPathLogCount < 10) {
        HookLogImportant(
            "[Overlay] RenderOverlay path: rebuild=%d cache=%d shouldUpdate=%d viewportChanged=%d cfgChanged=%d",
            needRebuild ? 1 : 0, hasCachedFrame ? 1 : 0, shouldUpdate ? 1 : 0, viewportChanged ? 1 : 0,
            configChanged ? 1 : 0);
        renderPathLogCount++;
    }

    // Pass HDR params to backend for shader constants
    if (backend) {
        float paperWhite = cfg.hdrPaperWhite;
        if (paperWhite <= 0.0f) {
            const HWND referenceHwnd = overlay_adapter_ResolveOverlayReferenceHwnd(reinterpret_cast<HWND>(hwnd));
            HMONITOR monitor = MonitorFromWindow(referenceHwnd, MONITOR_DEFAULTTONEAREST);
            if (monitor != reinterpret_cast<HMONITOR>(hdrPaperWhiteMonitor)) {
                ULONG rawLevel = 0;
                float queriedNits = 0.0f;
                if (overlay_adapter_QueryWindowsSdrWhiteNits(monitor, queriedNits, rawLevel)) {
                    resolvedHdrPaperWhiteNits = queriedNits;
                    HookLogImportant("[Overlay] Windows SDR white: monitor=%p raw=%lu nits=%.1f", monitor,
                                     static_cast<unsigned long>(rawLevel), resolvedHdrPaperWhiteNits);
                } else {
                    resolvedHdrPaperWhiteNits = 203.0f;
                    HookLogImportant("[Overlay] Windows SDR white unavailable for monitor=%p; using %.1f nits",
                                     monitor, resolvedHdrPaperWhiteNits);
                }
                hdrPaperWhiteMonitor = monitor;
            }
            paperWhite = resolvedHdrPaperWhiteNits;
        }
        int mode = 0;             // SDR
        if (isHDR) {
            // Detect HDR10/PQ (R10G10B10A2) vs scRGB (FP16) from render target format
            // DXGI_FORMAT_R10G10B10A2_UNORM = 24
            mode = (renderTargetFormat == 24) ? 2 : 1;
        }
        backend->SetHDRParams(mode, paperWhite);
    }

    if (!needRebuild) {
        const int64_t cachedRenderStartUs = activeDebugSample ? PerfLogger::GetQpcUs() : 0;
        if (renderer->RenderCachedFrame(viewportWidth, viewportHeight)) {
            if (activeDebugSample) {
                activeDebugSample->flags |= kPresentSampleFlagOverlayCacheHit;
                activeDebugSample->overlayRenderUs +=
                    static_cast<int32_t>(PerfLogger::GetQpcUs() - cachedRenderStartUs);
            }
            return;
        }
        hasCachedFrame = false;
    }

    const int64_t overlayBuildStartUs = activeDebugSample ? PerfLogger::GetQpcUs() : 0;
    if (renderPathLogCount < 10) {
        HookLogImportant("[Overlay] RenderOverlay: BeginFrame %dx%d", viewportWidth, viewportHeight);
    }
    renderer->BeginFrame(viewportWidth, viewportHeight);
    RenderContent(viewportWidth, viewportHeight, cfg, frameLayout, refreshLayout);
    if (activeDebugSample) {
        activeDebugSample->flags |= kPresentSampleFlagOverlayRebuilt;
        activeDebugSample->overlayBuildUs += static_cast<int32_t>(PerfLogger::GetQpcUs() - overlayBuildStartUs);
    }
    const int64_t overlayRenderStartUs = activeDebugSample ? PerfLogger::GetQpcUs() : 0;
    if (renderPathLogCount < 10) {
        HookLogImportant("[Overlay] RenderOverlay: EndFrame");
    }
    renderer->EndFrame();
    if (activeDebugSample) {
        activeDebugSample->overlayRenderUs += static_cast<int32_t>(PerfLogger::GetQpcUs() - overlayRenderStartUs);
    }

    hasCachedFrame = true;
    hasRenderedConfig = true;
    lastRenderedConfig = cfg;
    lastViewportWidth = viewportWidth;
    lastViewportHeight = viewportHeight;
    lastFrameLayout = frameLayout;
    hasLastFrameLayout = true;
}

void OverlayAdapter::RenderContent(int viewportWidth, int viewportHeight, const OverlayConfig& cfg,
                                   const FrameLayoutSnapshot& frameLayout, bool refreshLayout) {
    using namespace CustomOverlay;

    if (!ipc || !ipc->GetSharedMem())
        return;
    // Get DPI scale for consistent sizing
    float dpiScale = renderer->GetDpiScale();

    // Calculate position (DPI-aware padding)
    float padding = (float)cfg.padding * dpiScale;
    float x = padding, y = padding;

    // Log position info on first few renders
    static int posLogCount = 0;
    if (posLogCount < 3) {
        HookLog("[Overlay] RenderContent#%d: dpiScale=%.2f, vp=%dx%d, x=%.1f, y=%.1f, padding=%.1f", posLogCount,
                dpiScale, viewportWidth, viewportHeight, x, y, padding);
        posLogCount++;
    }

    float lineHeight = (float)renderer->GetLineHeight();

    const bool rowGPU = (frameLayout.rowMask & kRowGPU) != 0;
    const bool rowCPU = (frameLayout.rowMask & kRowCPU) != 0;
    const bool rowVRAM = (frameLayout.rowMask & kRowVRAM) != 0;
    const bool rowRAM = (frameLayout.rowMask & kRowRAM) != 0;
    const bool rowFPS = (frameLayout.rowMask & kRowFPS) != 0;
    const bool rowFGRates = (frameLayout.rowMask & kRowFGRates) != 0;
    const bool rowFPSAverages = (frameLayout.rowMask & kRowFPSAverages) != 0;
    const bool rowFGStatus = (frameLayout.rowMask & kRowFGStatus) != 0;
    const bool rowRecording = (frameLayout.rowMask & kRowRecording) != 0;
    const bool rowNotification = (frameLayout.rowMask & kRowNotification) != 0;
    const bool vramTelemetryAvailable = cachedSystemMetrics.vramUsageValid;

    // Adaptive overlay width: measure visible labels/values and size to content.
    const float kShadowPad = 1.0f;
    const float kBgLeftPad = 4.0f * dpiScale;
    const float kBgRightPad = 4.0f * dpiScale + kShadowPad;
    float kBgTopPad = 2.0f * dpiScale;
    if (rowVRAM || rowRAM) {
        kBgTopPad = (std::max)(kBgTopPad, lineHeight * 0.20f + kShadowPad);
    }
    const float kBgBottomPad = 2.0f * dpiScale + kShadowPad;
    const float kColumnGap = 10.0f * dpiScale;
    const float kMinContentWidth = 210.0f * dpiScale;
    const float kMemorySuffixScale = 0.75f;
    const float kMemoryGap = 2.0f * dpiScale;

    auto MeasureTextWidth = [&](const char* text) -> float {
        float w = 0, h = 0;
        renderer->CalcTextSize(text ? text : "", &w, &h);
        return w;
    };
    auto MeasureTextWidthScaled = [&](const char* text, float scale) -> float {
        float w = 0, h = 0;
        renderer->CalcTextSizeScaled(text ? text : "", &w, &h, scale);
        return w;
    };

    // Expensive layout measurement (snprintf + CalcTextSize) – cached between
    // updates to avoid per-frame overhead at high refresh rates.
    if (refreshLayout) {
        float maxLabelWidth = 0.0f;
        float maxValueWidth = 0.0f;
        char measureBuf[96];

        if (rowGPU) {
            if (cachedSystemMetrics.gpuUsageValid)
                snprintf(measureBuf, sizeof(measureBuf), "%.0f%%", cachedSystemMetrics.gpuUsage);
            else
                snprintf(measureBuf, sizeof(measureBuf), "--");
            maxLabelWidth = (std::max)(maxLabelWidth, MeasureTextWidth(SystemMetricsCollector::Get().GetGPUName()));
            maxValueWidth = (std::max)(maxValueWidth, MeasureTextWidth(measureBuf) + kShadowPad);
            maxValueWidth = (std::max)(maxValueWidth, MeasureTextWidth("100%") + kShadowPad);
        }
        if (rowCPU) {
            snprintf(measureBuf, sizeof(measureBuf), "%.0f%% (%.0f%%)", cachedSystemMetrics.cpuUsage,
                     cachedSystemMetrics.cpuMaxCoreUsage);
            maxLabelWidth = (std::max)(maxLabelWidth, MeasureTextWidth(SystemMetricsCollector::Get().GetCPUName()));
            maxValueWidth = (std::max)(maxValueWidth, MeasureTextWidth(measureBuf) + kShadowPad);
            maxValueWidth = (std::max)(maxValueWidth, MeasureTextWidth("100% (100%)") + kShadowPad);
        }
        if (rowVRAM) {
            float gbUsed = (float)cachedSystemMetrics.vramUsed / (1024.0f * 1024.0f * 1024.0f);
            float gbTotal = (float)cachedSystemMetrics.vramTotal / (1024.0f * 1024.0f * 1024.0f);
            const MemoryValueMode memoryMode = SelectMemoryValueMode(
                vramTelemetryAvailable, cachedSystemMetrics.vramUsed, cachedSystemMetrics.vramTotal);
            char usedBuf[32], totalBuf[32];
            float valueWidth = MeasureTextWidth("--") + kShadowPad;
            if (memoryMode != MemoryValueMode::Unavailable) {
                snprintf(usedBuf, sizeof(usedBuf), "%.2f GB",
                         memoryMode == MemoryValueMode::UsedAndTotal ? (std::max)(gbUsed, gbTotal) : gbUsed);
                valueWidth = MeasureTextWidth(usedBuf) + kShadowPad;
                if (memoryMode == MemoryValueMode::UsedAndTotal) {
                    snprintf(totalBuf, sizeof(totalBuf), "of %.2f GB", gbTotal);
                    valueWidth += kMemoryGap + MeasureTextWidthScaled(totalBuf, kMemorySuffixScale);
                }
            }
            maxLabelWidth = (std::max)(maxLabelWidth, MeasureTextWidth("VRAM"));
            maxValueWidth = (std::max)(maxValueWidth, valueWidth);
        }
        if (rowRAM) {
            float gbUsed = (float)cachedSystemMetrics.ramUsed / (1024.0f * 1024.0f * 1024.0f);
            float gbTotal = (float)cachedSystemMetrics.ramTotal / (1024.0f * 1024.0f * 1024.0f);
            const MemoryValueMode memoryMode = SelectMemoryValueMode(
                cachedSystemMetrics.ramUsed != 0, cachedSystemMetrics.ramUsed, cachedSystemMetrics.ramTotal);
            char usedBuf[32], totalBuf[32];
            float valueWidth = MeasureTextWidth("--") + kShadowPad;
            if (memoryMode != MemoryValueMode::Unavailable) {
                snprintf(usedBuf, sizeof(usedBuf), "%.2f GB",
                         memoryMode == MemoryValueMode::UsedAndTotal ? (std::max)(gbUsed, gbTotal) : gbUsed);
                valueWidth = MeasureTextWidth(usedBuf) + kShadowPad;
                if (memoryMode == MemoryValueMode::UsedAndTotal) {
                    snprintf(totalBuf, sizeof(totalBuf), "of %.2f GB", gbTotal);
                    valueWidth += kMemoryGap + MeasureTextWidthScaled(totalBuf, kMemorySuffixScale);
                }
            }
            maxLabelWidth = (std::max)(maxLabelWidth, MeasureTextWidth("RAM"));
            maxValueWidth = (std::max)(maxValueWidth, valueWidth);
        }
        if (rowFPS) {
            const char* apiLabel = graphicsAPI[0] ? graphicsAPI : "FPS";
            snprintf(measureBuf, sizeof(measureBuf), "%.0f FPS", cachedFPS);
            maxLabelWidth = (std::max)(maxLabelWidth, MeasureTextWidth(apiLabel));
            maxValueWidth = (std::max)(maxValueWidth, MeasureTextWidth(measureBuf) + kShadowPad);
            maxValueWidth = (std::max)(maxValueWidth, MeasureTextWidth("9999 FPS") + kShadowPad);

            if (rowFGRates) {
                snprintf(measureBuf, sizeof(measureBuf), "%.0f / %.0f FPS", frameLayout.fgBaseFPS,
                         frameLayout.fgOutputFPS);
                maxLabelWidth = (std::max)(maxLabelWidth, MeasureTextWidth("Base/Display"));
                maxValueWidth = (std::max)(maxValueWidth, MeasureTextWidth(measureBuf) + kShadowPad);
                maxValueWidth = (std::max)(maxValueWidth, MeasureTextWidth("9999 / 9999 FPS") + kShadowPad);
            }

            if (rowFPSAverages) {
                snprintf(measureBuf, sizeof(measureBuf), "%.0f / %.0f / %.0f", cachedAvgFPS, cached1PercentLow,
                         cached01PercentLow);
                maxLabelWidth = (std::max)(maxLabelWidth, MeasureTextWidth("Avg/1%/0.1%"));
                maxValueWidth = (std::max)(maxValueWidth, MeasureTextWidth(measureBuf) + kShadowPad);
                maxValueWidth = (std::max)(maxValueWidth, MeasureTextWidth("9999 / 9999 / 9999") + kShadowPad);
            }
        }
        if (rowFGStatus) {
            const char* fgLabels[] = {"DLSS FG", "FSR FG", "NVIDIA SM", "FG"};
            for (const char* fgLabel : fgLabels) {
                snprintf(measureBuf, sizeof(measureBuf), "%s 4x", fgLabel);
                maxLabelWidth = (std::max)(maxLabelWidth, MeasureTextWidth(fgLabel));
                maxValueWidth = (std::max)(maxValueWidth, MeasureTextWidth(measureBuf) + kShadowPad);
            }
            snprintf(measureBuf, sizeof(measureBuf), "%s %dx", frameLayout.fgLabel, frameLayout.fgMultiplier);
            maxValueWidth = (std::max)(maxValueWidth, MeasureTextWidth(measureBuf) + kShadowPad);
        }

        float measuredWidth = kMinContentWidth;
        if (maxLabelWidth > 0.0f || maxValueWidth > 0.0f) {
            measuredWidth = (std::max)(measuredWidth, maxLabelWidth + kColumnGap + maxValueWidth);
        }

        // Recording row uses a fixed-width digit format; measure a canonical string
        // so the result is stable regardless of elapsed time (digits are tabular).
        if (rowRecording) {
            char recBuf[96];
            snprintf(recBuf, sizeof(recBuf), "STARTING RECORDING...");
            measuredWidth = (std::max)(measuredWidth, MeasureTextWidth(recBuf) + kShadowPad);
            snprintf(recBuf, sizeof(recBuf), "STARTING AUDIO...");
            measuredWidth = (std::max)(measuredWidth, MeasureTextWidth(recBuf) + kShadowPad);
            snprintf(recBuf, sizeof(recBuf), "REC 00:00:00");
            measuredWidth = (std::max)(measuredWidth, MeasureTextWidth(recBuf) + kShadowPad);
            snprintf(recBuf, sizeof(recBuf), "REC 00:00:00 !ENCODER OVERLOAD!");
            measuredWidth = (std::max)(measuredWidth, MeasureTextWidth(recBuf) + kShadowPad);
            snprintf(recBuf, sizeof(recBuf), "REC 00:00:00 !ENC SEVERE 9999.9/9999!");
            measuredWidth = (std::max)(measuredWidth, MeasureTextWidth(recBuf) + kShadowPad);
            snprintf(recBuf, sizeof(recBuf), "REC 00:00:00 !VIDEO DEGRADED!");
            measuredWidth = (std::max)(measuredWidth, MeasureTextWidth(recBuf) + kShadowPad);
            snprintf(recBuf, sizeof(recBuf), "AUDIO 00:00:00");
            measuredWidth = (std::max)(measuredWidth, MeasureTextWidth(recBuf) + kShadowPad);
        }
        if (rowNotification) {
            measuredWidth = (std::max)(measuredWidth, MeasureTextWidth("Screenshot saved!") + kShadowPad);
            measuredWidth = (std::max)(measuredWidth, MeasureTextWidth("Screenshot failed!") + kShadowPad);
            measuredWidth = (std::max)(measuredWidth, MeasureTextWidth("Finalizing recording...") + kShadowPad);
            measuredWidth = (std::max)(measuredWidth, MeasureTextWidth("Recording saved") + kShadowPad);
            measuredWidth =
                (std::max)(measuredWidth, MeasureTextWidth("Recording saved - video degraded") + kShadowPad);
            measuredWidth = (std::max)(measuredWidth, MeasureTextWidth("Recording failed") + kShadowPad);
        }

        cachedContentWidth = measuredWidth;
        layoutDirty = false;
    }

    float contentWidth = cachedContentWidth;
    float bgWidth = contentWidth + kBgLeftPad + kBgRightPad;
    float maxBgWidth = (float)viewportWidth - 2.0f * padding;
    maxBgWidth = (std::max)(maxBgWidth, 100.0f * dpiScale);
    if (bgWidth > maxBgWidth) {
        bgWidth = maxBgWidth;
        contentWidth = (std::max)(80.0f * dpiScale, bgWidth - kBgLeftPad - kBgRightPad);
    }

    // Calculate required height upfront so BottomLeft/BottomRight use the real
    // overlay height instead of a hardcoded estimate.
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    float requiredHeight = frameLayout.rowCount * lineHeight;

    constexpr int GRAPH_SAMPLES = 180;
    bool showGraph = cfg.showFrameTime && metrics;
    if (showGraph)
        requiredHeight += 4 * dpiScale + 50.0f * dpiScale;
    float bgHeight = requiredHeight + kBgTopPad + kBgBottomPad;

    switch (cfg.position) {
        case OverlayPosition::TopLeft:
            x = padding;
            y = padding;
            break;
        case OverlayPosition::TopRight:
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            x = viewportWidth - padding - bgWidth;
            y = padding;
            break;
        case OverlayPosition::BottomLeft:
            x = padding;
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            y = viewportHeight - padding - bgHeight;
            break;
        case OverlayPosition::BottomRight:
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            x = viewportWidth - padding - bgWidth;
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            y = viewportHeight - padding - bgHeight;
            break;
    }

    float valueRightEdge = x + contentWidth;

    float graphData[GRAPH_SAMPLES] = {};
    float graphY = 0, graphHeight = 0, graphWidth = 0, graphX = 0;
    float graphMinVal = 0, graphMaxVal = 0;
    if (showGraph)
        metrics->GetLastHistory(graphData, GRAPH_SAMPLES);

    // --- PASS 1: All solid geometry (background + graph) ---
    uint8_t bgAlpha = (uint8_t)(cfg.bgAlpha * 255);
    uint32_t bgColor = (bgAlpha << 24) | (cfg.bgColor & 0x00FFFFFF);
    renderer->DrawRectFilled(x - kBgLeftPad, y - kBgTopPad, bgWidth, bgHeight, bgColor);

    // The row mask is the single source of truth for text, panel height, and
    // graph placement, so rows appear/disappear atomically.
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    float graphCursorY = y + frameLayout.rowCount * lineHeight;

    // Max frame time display values – recomputed at most once every 2 seconds so
    // the label doesn't flicker on every frame.
    float recentMaxFrameTime = cachedMaxFrameTime;
    float recentAvgFrameTime = cachedAvgFrameTimeForColor;
    if (showGraph) {
        DWORD ftNow = GetTickCount();
        if ((ftNow - lastMaxFrameTimeUpdateTime) >= 2000) {
            lastMaxFrameTimeUpdateTime = ftNow;
            int samplesPerSecond = (cachedFPS > 0) ? (int)cachedFPS : 60;
            int samplesFor2Seconds = (std::min)(GRAPH_SAMPLES, samplesPerSecond * 2);
            int startIdx = GRAPH_SAMPLES - samplesFor2Seconds;

            float newMax = 0.0f, sum = 0.0f;
            int count = 0;
            for (int i = startIdx; i < GRAPH_SAMPLES; i++) {
                float val = graphData[i];
                if (val > newMax)
                    newMax = val;
                sum += val;
                count++;
            }
            cachedMaxFrameTime = newMax;
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            cachedAvgFrameTimeForColor = (count > 0) ? sum / count : 0.0f;
            recentMaxFrameTime = cachedMaxFrameTime;
            recentAvgFrameTime = cachedAvgFrameTimeForColor;
        }

        // Calculate graph scaling - use exact peak with dynamic minimum for
        // better vertical centering. Instead of always starting at 0ms,
        // calculate a minimum that provides ~15% padding below the lowest point.
        float peakVal = 0.0f;
        float minVal = FLT_MAX;
        for (int i = 0; i < GRAPH_SAMPLES; i++) {
            if (graphData[i] > peakVal)
                peakVal = graphData[i];
            if (graphData[i] > 0.001f && graphData[i] < minVal)
                minVal = graphData[i];
        }
        if (minVal == FLT_MAX)
            minVal = 0.0f;

        // Smarter scaling to avoid over-dramatizing frame time variations.
        // Instead of just 10% above peak, use a scale that provides meaningful
        // context (e.g., 30fps vs 60fps threshold should be visible).
        float avgVal = 0;
        int avgCount = 0;
        for (int i = 0; i < GRAPH_SAMPLES; i++) {
            if (graphData[i] > 0.001f) {
                avgVal += graphData[i];
                avgCount++;
            }
        }
        if (avgCount > 0)
            avgVal /= (float)avgCount;

        // Scale based on average with generous headroom:
        // - At least 50% above average (not peak) to avoid zooming in on noise
        // - At least 2x the minimum (so 8ms avg shows 0-16ms, not 0-12ms)
        // - Minimum 33ms range to show 30fps threshold
        float scaleBase = (std::max)(avgVal, minVal * 2.0f);
        float rawMax = (std::max)(scaleBase * 1.5f, 33.0f);

        // Dynamic minimum: leave ~15% padding below lowest point, but never go negative
        // This makes the graph line appear more vertically centered
        float range = rawMax - minVal;
        float dynamicMin = (std::max)(0.0f, minVal - range * 0.15f);

        graphMinVal = dynamicMin;
        graphMaxVal = rawMax;

        graphWidth = bgWidth;
        graphHeight = 50.0f * dpiScale;
        graphX = x - 4 * dpiScale;
        graphY = graphCursorY + 4 * dpiScale;  // Small gap below stats

        // Debug: log graph dimensions on first render
        static bool s_loggedGraphDims = false;
        if (!s_loggedGraphDims) {
            s_loggedGraphDims = true;
            HookLogImportant("[Overlay] Graph dims: graphX=%.1f graphWidth=%.1f bgWidth=%.1f x=%.1f kBgLeftPad=%.1f",
                             graphX, graphWidth, bgWidth, x, kBgLeftPad);
        }

        // Top padding keeps the graph line below the scale marker and frame time
        // labels that are drawn at graphY + ~6px.
        float graphTopPad = 14.0f * dpiScale;

        uint32_t graphColor = cfg.frametimeColor ? cfg.frametimeColor : Colors::Yellow;
        renderer->DrawFrameTimeGraph(graphX, graphY + graphTopPad, graphWidth, graphHeight - graphTopPad, graphData,
                                     GRAPH_SAMPLES, graphMinVal, graphMaxVal, graphColor);
    }

    // --- PASS 2: All text (single textured batch) ---
    float cursorY = y;
    char buf[64];

    uint32_t textColor = cfg.textColor ? cfg.textColor : Colors::White;
    uint32_t shadowColor = cfg.textOutlineColor ? cfg.textOutlineColor : Colors::Black;

    // Column positions for alignment (DPI-aware)
    float labelCol = x;

    // Transient notification inside the overlay panel (background extends to fit)
    if (rowNotification) {
        const char* notifText = nullptr;
        uint32_t notifColor = Colors::Green;
        switch (frameLayout.notificationType) {
            case static_cast<uint32_t>(OverlayNotificationType::ScreenshotSaved):
                notifText = "Screenshot saved!";
                break;
            case static_cast<uint32_t>(OverlayNotificationType::ScreenshotFailed):
                notifText = "Screenshot failed!";
                notifColor = Colors::Red;
                break;
            case static_cast<uint32_t>(OverlayNotificationType::RecordingFinalizing):
                notifText = "Finalizing recording...";
                notifColor = Colors::LabelYellow;
                break;
            case static_cast<uint32_t>(OverlayNotificationType::RecordingSaved):
                notifText = "Recording saved";
                break;
            case static_cast<uint32_t>(OverlayNotificationType::RecordingSavedDegraded):
                notifText = "Recording saved - video degraded";
                notifColor = Colors::Red;
                break;
            case static_cast<uint32_t>(OverlayNotificationType::RecordingCanceled):
                notifText = "Recording canceled";
                break;
            case static_cast<uint32_t>(OverlayNotificationType::RecordingFailed):
                notifText = "Recording failed";
                notifColor = Colors::Red;
                break;
            default:
                break;
        }
        if (notifText) {
            renderer->DrawTextWithShadow(labelCol, cursorY, notifText, notifColor, shadowColor);
            cursorY += lineHeight;
        }
    }

    // GPU - Name in green, % in yellow (based on load)
    if (rowGPU) {
        if (cachedSystemMetrics.gpuUsageValid)
            snprintf(buf, 64, "%.0f%%", cachedSystemMetrics.gpuUsage);
        else
            snprintf(buf, 64, "--");
        renderer->DrawTextWithShadow(labelCol, cursorY, SystemMetricsCollector::Get().GetGPUName(), Colors::LabelGreen,
                                     shadowColor);
        const uint32_t gpuValueColor =
            cachedSystemMetrics.gpuUsageValid ? GetLoadColor(cachedSystemMetrics.gpuUsage) : Colors::Gray;
        renderer->DrawTextRightAligned(valueRightEdge, cursorY, buf, gpuValueColor, shadowColor);
        cursorY += lineHeight;
    }

    // CPU - Name in green, % (maxCore%) in cyan
    if (rowCPU) {
        snprintf(buf, 64, "%.0f%% (%.0f%%)", cachedSystemMetrics.cpuUsage, cachedSystemMetrics.cpuMaxCoreUsage);
        renderer->DrawTextWithShadow(labelCol, cursorY, SystemMetricsCollector::Get().GetCPUName(), Colors::LabelGreen,
                                     shadowColor);
        renderer->DrawTextRightAligned(valueRightEdge, cursorY, buf, Colors::ValueCyan, shadowColor);
        cursorY += lineHeight;
    }

    // VRAM - Label in orange, "X.XX GB" in white, "of Y.YY GB" in smaller raised
    // text
    if (rowVRAM) {
        float gbUsed = (float)cachedSystemMetrics.vramUsed / (1024.0f * 1024.0f * 1024.0f);
        float gbTotal = (float)cachedSystemMetrics.vramTotal / (1024.0f * 1024.0f * 1024.0f);
        const MemoryValueMode memoryMode =
            SelectMemoryValueMode(vramTelemetryAvailable, cachedSystemMetrics.vramUsed, cachedSystemMetrics.vramTotal);

        renderer->DrawTextWithShadow(labelCol, cursorY, "VRAM", Colors::LabelOrange, shadowColor);

        if (memoryMode == MemoryValueMode::Unavailable) {
            renderer->DrawTextRightAligned(valueRightEdge, cursorY, "--", Colors::Gray, shadowColor);
            cursorY += lineHeight;
        } else {
            char usedBuf[32];
            snprintf(usedBuf, 32, "%.2f GB", gbUsed);
            if (memoryMode == MemoryValueMode::UsedOnly) {
                renderer->DrawTextRightAligned(valueRightEdge, cursorY, usedBuf, Colors::LabelOrange, shadowColor);
                cursorY += lineHeight;
            } else {
                char totalBuf[32];
                snprintf(totalBuf, 32, "of %.2f GB", gbTotal);

                // Right-align the whole "used + of total" composite so it always fits.
                float usedWidth = 0, usedHeight = 0;
                float totalWidth = 0;
                renderer->CalcTextSize(usedBuf, &usedWidth, &usedHeight);
                float smallScale = 0.75f;     // Smaller scale for superscript
                float gap = 2.0f * dpiScale;  // Minimal gap between segments
                renderer->CalcTextSizeScaled(totalBuf, &totalWidth, nullptr, smallScale);
                float usedX = valueRightEdge - (usedWidth + gap + totalWidth);
                renderer->DrawTextWithShadow(usedX, cursorY, usedBuf, Colors::LabelOrange, shadowColor);

                float raisedY = cursorY - usedHeight * 0.20f;  // Raised 20% of line height
                renderer->DrawTextScaledWithShadow(usedX + usedWidth + gap, raisedY, totalBuf, textColor, shadowColor,
                                                   smallScale);

                cursorY += lineHeight;
            }
        }
    }

    // RAM - Label in pink, "X.XX GB" in white, "of Y.YY GB" in smaller raised
    // text
    if (rowRAM) {
        float gbUsed = (float)cachedSystemMetrics.ramUsed / (1024.0f * 1024.0f * 1024.0f);
        float gbTotal = (float)cachedSystemMetrics.ramTotal / (1024.0f * 1024.0f * 1024.0f);
        const MemoryValueMode memoryMode = SelectMemoryValueMode(
            cachedSystemMetrics.ramUsed != 0, cachedSystemMetrics.ramUsed, cachedSystemMetrics.ramTotal);

        renderer->DrawTextWithShadow(labelCol, cursorY, "RAM", Colors::LabelPink, shadowColor);

        if (memoryMode == MemoryValueMode::Unavailable) {
            renderer->DrawTextRightAligned(valueRightEdge, cursorY, "--", Colors::Gray, shadowColor);
            cursorY += lineHeight;
        } else {
            char usedBuf[32];
            snprintf(usedBuf, 32, "%.2f GB", gbUsed);
            if (memoryMode == MemoryValueMode::UsedOnly) {
                renderer->DrawTextRightAligned(valueRightEdge, cursorY, usedBuf, Colors::LabelPink, shadowColor);
                cursorY += lineHeight;
            } else {
                char totalBuf[32];
                snprintf(totalBuf, 32, "of %.2f GB", gbTotal);

                // Right-align the whole "used + of total" composite so it always fits.
                float usedWidth = 0, usedHeight = 0;
                float totalWidth = 0;
                renderer->CalcTextSize(usedBuf, &usedWidth, &usedHeight);
                float smallScale = 0.75f;     // Smaller scale for superscript
                float gap = 2.0f * dpiScale;  // Minimal gap between segments
                renderer->CalcTextSizeScaled(totalBuf, &totalWidth, nullptr, smallScale);
                float usedX = valueRightEdge - (usedWidth + gap + totalWidth);
                renderer->DrawTextWithShadow(usedX, cursorY, usedBuf, Colors::LabelPink, shadowColor);

                float raisedY = cursorY - usedHeight * 0.20f;  // Raised 20% of line height
                renderer->DrawTextScaledWithShadow(usedX + usedWidth + gap, raisedY, totalBuf, textColor, shadowColor,
                                                   smallScale);

                cursorY += lineHeight;
            }
        }
    }

    // FPS Section
    if (rowFPS) {
        // Graphics API label (if set) with current FPS
        const char* apiLabel = graphicsAPI[0] ? graphicsAPI : "FPS";
        snprintf(buf, 64, "%.0f FPS", cachedFPS);
        renderer->DrawTextWithShadow(labelCol, cursorY, apiLabel, textColor, shadowColor);
        renderer->DrawTextRightAligned(valueRightEdge, cursorY, buf, cfg.fpsColor, shadowColor);
        cursorY += lineHeight;

        // Base/Display FPS when FG is active (shown first as in reference)
        if (rowFGRates) {
            if (frameLayout.fgActive) {
                snprintf(buf, 64, "%.0f / %.0f FPS", frameLayout.fgBaseFPS, frameLayout.fgOutputFPS);
                renderer->DrawTextWithShadow(labelCol, cursorY, "Base/Display", Colors::LabelYellow, shadowColor);
                renderer->DrawTextRightAligned(valueRightEdge, cursorY, buf, Colors::ValueYellow, shadowColor);
            }
            cursorY += lineHeight;
        }

        // Avg/1%/0.1%
        if (rowFPSAverages) {
            snprintf(buf, 64, "%.0f / %.0f / %.0f", cachedAvgFPS, cached1PercentLow, cached01PercentLow);
            renderer->DrawTextWithShadow(labelCol, cursorY, "Avg/1%/0.1%", textColor, shadowColor);
            renderer->DrawTextRightAligned(valueRightEdge, cursorY, buf, Colors::ValueYellow, shadowColor);
            cursorY += lineHeight;
        }
    }

    // FG Status line
    if (rowFGStatus) {
        if (frameLayout.fgActive) {
            snprintf(buf, 64, "%s %dx", frameLayout.fgLabel, frameLayout.fgMultiplier);
            renderer->DrawTextWithShadow(labelCol, cursorY, frameLayout.fgLabel, Colors::LabelCyan, shadowColor);
            renderer->DrawTextRightAligned(valueRightEdge, cursorY, buf, Colors::LabelCyan, shadowColor);
        }
        cursorY += lineHeight;
    }

    // Recording status line
    if (rowRecording) {
        if (ce::recording_indicator::IsStarting(frameLayout.recordingState) && !frameLayout.recordingStatusDark) {
            renderer->DrawTextWithShadow(labelCol, cursorY,
                                         ce::recording_indicator::GetStartingText(frameLayout.recordingState),

                                         Colors::LabelYellow, shadowColor);
        } else {
            const char* recLabel = frameLayout.recordingAudioOnly ? "AUDIO" : "REC";
            int hours = (int)(frameLayout.recordingSeconds / 3600);
            int minutes = (int)((frameLayout.recordingSeconds % 3600) / 60);
            int seconds = (int)(frameLayout.recordingSeconds % 60);

            if (frameLayout.showOverloadWarning) {
                const std::string overloadLabel =
                    overlay_adapter_FormatRecordingHealthLabel(frameLayout.recordingWarningKind,
                                               frameLayout.recordingSustainFpsX100,
                                               frameLayout.recordingTargetFps);
                std::snprintf(buf, sizeof(buf), "%s %02d:%02d:%02d %s", recLabel, hours, minutes, seconds,
                              overloadLabel.c_str());
                renderer->DrawTextWithShadow(labelCol, cursorY, buf, Colors::Red, shadowColor);
            } else {
                // Normal recording display
                snprintf(buf, 64, "%s %02d:%02d:%02d", recLabel, hours, minutes, seconds);
                renderer->DrawTextWithShadow(labelCol, cursorY, buf, Colors::Red, shadowColor);
            }
        }
        cursorY += lineHeight;
    }

    // Frame time graph labels and markers
    if (showGraph) {
        uint32_t graphLabelColor = Colors::Green;
        uint32_t grayColor = 0xFFB0B0B0;  // Light gray for scale marker
        float smallFontScale = 0.75f;     // Smaller font for graph labels

        // Scale marker: small gray line at top left with ceiling value (with ms
        // unit)
        float scaleLineLength = 15.0f * dpiScale;
        float scaleLineY = graphY + 1.0f * dpiScale;
        renderer->DrawLine(graphX + 4 * dpiScale, scaleLineY, graphX + 4 * dpiScale + scaleLineLength, scaleLineY,
                           grayColor, 1.0f * dpiScale);

        // Scale marker text (ceiling value with ms unit in small gray font)
        snprintf(buf, 64, "%.0f ms", graphMaxVal);
        float scaleTextWidth = 0, scaleTextHeight = 0;
        renderer->CalcTextSizeScaled(buf, &scaleTextWidth, &scaleTextHeight, smallFontScale);
        renderer->DrawTextScaledWithShadow(graphX + 6 * dpiScale + scaleLineLength, scaleLineY - scaleTextHeight * 0.5f,
                                           buf, grayColor, shadowColor, smallFontScale);

        // Current frame time display at top right of graph
        // Color based on comparison with average: green (close), yellow (spike),
        // red (stutter)
        uint32_t frameTimeColor = Colors::Green;
        if (recentAvgFrameTime > 0.001f) {
            float ratio = recentMaxFrameTime / recentAvgFrameTime;
            if (ratio > 2.0f) {
                frameTimeColor = Colors::Red;  // Bad stutter (2x average)
            } else if (ratio > 1.5f) {
                frameTimeColor = Colors::Yellow;  // Moderate spike (1.5x average)
            }
        }

        snprintf(buf, 64, "%.1f ms", recentMaxFrameTime);
        float ftTextWidth = 0, ftTextHeight = 0;
        renderer->CalcTextSizeScaled(buf, &ftTextWidth, &ftTextHeight, smallFontScale);
        renderer->DrawTextScaledWithShadow(graphX + graphWidth - ftTextWidth - 4 * dpiScale,
                                           scaleLineY - ftTextHeight * 0.5f, buf, frameTimeColor, shadowColor,
                                           smallFontScale);
    }

    (void)viewportWidth;
    (void)viewportHeight;
}
