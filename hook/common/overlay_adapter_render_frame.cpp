#include "overlay_adapter_internal.h"
#include "benchmark_manager.h"
#include "benchmark_overlay_render.h"

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

    int64_t currentQpcUs = 0;
    if (metrics) {
        // Deliberately not PerfLogger::GetQpcUs(): this value is compared against
        // a publication stamp produced in the sensor process, so it has to use
        // the same conversion the producer used, including its overflow guard.
        LARGE_INTEGER displayTimingNow = {};
        QueryPerformanceCounter(&displayTimingNow);
        currentQpcUs = DisplayTimingQpcToUs(displayTimingNow.QuadPart, PerfLogger::GetQpcFrequency());
        metrics->SetFrameTimeSource(cfg.frameTimeSource);
        metrics->ConsumeDisplayTiming(sharedMem->displayTiming, currentQpcUs);
        const FrameTimeSource effectiveSource = metrics->GetEffectiveFrameTimeSource();
        const DWORD sourceNow = GetTickCount();
        if (!hasObservedFrameTimeSource || effectiveSource != lastObservedFrameTimeSource) {
            if (!hasObservedFrameTimeSource || sourceNow - lastFrameTimeSourceLogTime >= 10000) {
                HookLogImportant("[Overlay] Frame timing source: %s (requested=%s sensorStatus=%u)",
                                 effectiveSource == FrameTimeSource::DisplayChange ? "display-change" : "presentation",
                                 cfg.frameTimeSource == FrameTimeSource::DisplayChange ? "display-change" : "presentation",
                                 static_cast<uint32_t>(sharedMem->displayTiming.GetStatus()));
                lastFrameTimeSourceLogTime = sourceNow;
                lastObservedFrameTimeSource = effectiveSource;
                hasObservedFrameTimeSource = true;
            }
        }
    }

    const float presFrameTimeMs = metrics ? metrics->GetLastPresentationFrameTimeMs() : 0.0f;
    const float dispFrameTimeMs = metrics ? metrics->GetLastDisplayFrameTimeMs() : 0.0f;
    BenchmarkManager::Get().OnFrame(currentQpcUs, presFrameTimeMs, dispFrameTimeMs,
                                    SystemMetricsCollector::Get().GetMetrics(), sharedMem);
    const bool benchmarkActive = BenchmarkManager::Get().IsActiveOrShowingResults();

    if (!cfg.showOverlay && !benchmarkActive) {
        if (renderLogCount < 5)
            HookLog("[Overlay] RenderOverlay: early return - showOverlay is false");
        return;
    }

    // Update throttling
    DWORD now = GetTickCount();
    const bool latencyJustEnabled =
        cfg.showSystemLatency && (!hasRenderedConfig || !lastRenderedConfig.showSystemLatency);
    bool shouldUpdate = (now - lastUpdateTime) >= cfg.textUpdateInterval || latencyJustEnabled;
    if (shouldUpdate) {
        lastUpdateTime = now;
        if (metrics) {
            cachedFPS = metrics->GetCurrentFPS();
            cachedAvgFPS = metrics->GetAverageFPS();
            cached1PercentLow = metrics->Get1PercentLowFPS();
            cached01PercentLow = metrics->Get01PercentLowFPS();
            if (cfg.showSystemLatency && currentQpcUs > 0) {
                constexpr DWORD kNativeLatencyQueryIntervalMs = 250;
                // A registered cross-IHV marker provider reports without a
                // graphics device, so the poll must not wait for one to be
                // resolved: gating on the device alone silently disables the
                // game's own PCL markers whenever the device is unavailable.
                const bool nativeLatencySourceAvailable =
                    latencyDevice != nullptr ||
                    ce::system_latency::GetSupplementalNativeReportProvider() != nullptr;
                if (nativeLatencySourceAvailable &&
                    (lastNativeLatencyQueryTime == 0 ||
                     now - lastNativeLatencyQueryTime >= kNativeLatencyQueryIntervalMs)) {
                    ce::system_latency::NativeReport nativeReport;
                    if (ce::system_latency::QueryNativeReport(latencyDevice, nativeReport))
                        metrics->SubmitNativeLatencyReport(nativeReport);
                    lastNativeLatencyQueryTime = now;
                }

                cachedSystemLatency = metrics->GetSystemLatency(currentQpcUs);
                const auto latencySource = cachedSystemLatency.source;
                // Comparing a latency reading across a configuration change is
                // only sound if the reading's provenance is on record, so the
                // full decomposition is logged, not just the published number:
                // a wide min/max spread or a low association ratio is how a
                // broken present/display correlation announces itself.
                constexpr DWORD kSystemLatencyLogIntervalMs = 5000;
                const bool latencySourceChanged = latencySource != lastLoggedSystemLatencySource;
                const bool latencyLogDue =
                    !hasObservedSystemLatencySource || latencySourceChanged ||
                    now - lastSystemLatencySourceLogTime >= kSystemLatencyLogIntervalMs;
                if (latencyLogDue) {
                    const auto latencyDiagnostics = metrics->GetSystemLatencyDiagnostics(currentQpcUs);
                    HookLogImportant(
                        "[Overlay] PC latency sample: source=%s value=%.1fms median=%.1f min=%.1f max=%.1f "
                        "samples=%u fg=%d multiplier=%d baseFps=%.1f outputFps=%.1f",
                        ce::system_latency::SourceLogLabel(latencySource), cachedSystemLatency.milliseconds,
                        cachedSystemLatency.medianMilliseconds, cachedSystemLatency.minimumMilliseconds,
                        cachedSystemLatency.maximumMilliseconds, cachedSystemLatency.sampleCount,
                        metrics->IsFGActive() ? 1 : 0, metrics->GetFGMultiplier(), metrics->GetFGBaseFPS(),
                        metrics->GetFGOutputFPS());
                    HookLogImportant(
                        "[Overlay] PC latency chain: frameBegin=%s anchorToPresent=%lldus presentToDisplay=%lldus "
                        "inputWait=%lldus baseInterval=%lldus applicationInterval=%lldus frameBeginInterval=%lldus "
                        "displayInterval=%lldus outputRatio=%dpermille generationObserved=%d generatorHold=%d "
                        "markerInterval=%lldus markerTrusted=%d markerAssociated=%d displays=%llu associated=%llu "
                        "unmatched=%llu droppedPresents=%llu rejected=%llu (p2d=%llu base=%llu total=%llu) "
                        "markerCadenceRejects=%llu epochResets=%llu sourceChanges=%llu",
                        ce::system_latency::FrameBeginKindLabel(latencyDiagnostics.lastFrameBeginKind),
                        static_cast<long long>(latencyDiagnostics.lastAnchorToPresentUs),
                        static_cast<long long>(latencyDiagnostics.lastPresentToDisplayUs),
                        static_cast<long long>(latencyDiagnostics.lastInputWaitUs),
                        static_cast<long long>(latencyDiagnostics.lastBaseIntervalUs),
                        static_cast<long long>(latencyDiagnostics.applicationIntervalUs),
                        static_cast<long long>(latencyDiagnostics.frameBeginIntervalUs),
                        static_cast<long long>(latencyDiagnostics.displayIntervalUs),
                        latencyDiagnostics.observedOutputRatioPermille,
                        latencyDiagnostics.frameGenerationObserved ? 1 : 0,
                        latencyDiagnostics.generatorHoldApplied ? 1 : 0,
                        static_cast<long long>(latencyDiagnostics.markerIntervalUs),
                        latencyDiagnostics.markerCadenceTrusted ? 1 : 0,
                        latencyDiagnostics.lastMarkerUsedAssociation ? 1 : 0,
                        static_cast<unsigned long long>(latencyDiagnostics.displaysObserved),
                        static_cast<unsigned long long>(latencyDiagnostics.displaysWithPresentAssociation),
                        static_cast<unsigned long long>(latencyDiagnostics.displaysWithoutMatchedPresent),
                        static_cast<unsigned long long>(latencyDiagnostics.presentsDroppedUnderContention),
                        static_cast<unsigned long long>(latencyDiagnostics.samplesRejectedOutOfRange),
                        static_cast<unsigned long long>(latencyDiagnostics.samplesRejectedPresentToDisplay),
                        static_cast<unsigned long long>(latencyDiagnostics.samplesRejectedBaseInterval),
                        static_cast<unsigned long long>(latencyDiagnostics.samplesRejectedTotalLatency),
                        static_cast<unsigned long long>(latencyDiagnostics.markerReportsRejectedForOutputCadence),
                        static_cast<unsigned long long>(latencyDiagnostics.measurementEpochResets),
                        static_cast<unsigned long long>(latencyDiagnostics.sourceTransitions));
                    if (latencyDiagnostics.crossCheckSource != ce::system_latency::Source::Unavailable) {
                        HookLogImportant("[Overlay] PC latency cross-check: %s=%.1fms vs published %.1fms",
                                         ce::system_latency::SourceLogLabel(latencyDiagnostics.crossCheckSource),
                                         latencyDiagnostics.crossCheckMilliseconds,
                                         cachedSystemLatency.milliseconds);
                    }
                    lastSystemLatencySourceLogTime = now;
                    lastLoggedSystemLatencySource = latencySource;
                    hasObservedSystemLatencySource = true;
                }
            } else {
                cachedSystemLatency = {};
            }
        }
        if (cfg.showCPU || cfg.showRAM || cfg.showGPU || cfg.showVRAM) {
            SystemMetricsCollector::Get().Update();
            cachedSystemMetrics = SystemMetricsCollector::Get().GetMetrics();
        }
    }

    FrameLayoutSnapshot frameLayout = {};
    frameLayout.systemLatency = cachedSystemLatency;
    frameLayout.fgActive = cfg.showFG && metrics && metrics->IsFGActive();
    frameLayout.reserveFGSpace = false;
    if (frameLayout.fgActive && metrics) {
        frameLayout.fgMultiplier = metrics->GetFGMultiplier();
        std::snprintf(frameLayout.fgLabel, sizeof(frameLayout.fgLabel), "%s", metrics->GetFGTypeLabel());
        frameLayout.fgBaseFPS = metrics->GetFGBaseFPS();
        frameLayout.fgOutputFPS = metrics->GetFGOutputFPS();
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
        frameLayout.notificationType <= static_cast<uint32_t>(OverlayNotificationType::StreamingFailed);
    frameLayout.notificationVisible =
        notificationExpiry > nowTick64 && frameLayout.notificationType != 0 &&
        (!recordingFinalizationNotification || frameLayout.recordingState == ce::recording_indicator::State::Idle);

    ce::overlay_layout::RowInputs rowInputs = {};
    rowInputs.showGPU = cfg.showGPU;
    rowInputs.showCPU = cfg.showCPU;
    rowInputs.showGPUClocks = cachedSystemMetrics.gpuCoreClockValid || cachedSystemMetrics.gpuMemoryClockValid ||
                              cachedSystemMetrics.gpuCoreVoltageValid;
    rowInputs.showCPUClocks = cachedSystemMetrics.cpuCoreClockValid;
    rowInputs.showVRAM = cfg.showVRAM;
    rowInputs.showRAM = cfg.showRAM;
    rowInputs.showFPS = cfg.showFPS;
    rowInputs.showFPSAverages = cachedAvgFPS > 0.0f && cached1PercentLow > 0.0f;
    rowInputs.showSystemLatency = cfg.showSystemLatency;
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
    bool configChanged = !hasRenderedConfig || !OverlayConfigEquals(cfg, lastRenderedConfig);
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
                       dynamicStateChanged || layoutDirty || benchmarkActive;
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
            const HWND referenceHwnd = ResolveOverlayReferenceHwnd(reinterpret_cast<HWND>(hwnd));
            HMONITOR monitor = MonitorFromWindow(referenceHwnd, MONITOR_DEFAULTTONEAREST);
            if (monitor != reinterpret_cast<HMONITOR>(hdrPaperWhiteMonitor)) {
                ULONG rawLevel = 0;
                float queriedNits = 0.0f;
                if (QueryWindowsSdrWhiteNits(monitor, queriedNits, rawLevel)) {
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
    if (benchmarkActive) {
        const float benchDpiScale = renderer->GetDpiScale();
        const float pad = (float)cfg.padding * benchDpiScale;
        RenderBenchmarkOverlay(renderer, pad, pad, benchDpiScale, viewportWidth, viewportHeight);
    }
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
