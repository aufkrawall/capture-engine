#include "media_main_encoder_session.h"

void MediaEncoderSession::RefreshCaptureContentDelay(bool screenGrab) {
    // A/V content delay: align video content with inherently-late loopback audio by biasing
    // WGC source-frame selection back by the loopback capture latency (= the slowest audio
    // source's latency, so faster sources can be equalized up to it). Audio samples and the
    // CFR PTS grid are untouched, so track length/start/end and zero-drift are preserved.
    // Video buffer self-builds/holds via the bounded "too new for slot" path. QPC ticks.
    maxAudioCaptureLatencyMs = 0.0f;
    for (const auto& audioSrc : config.audioSources) {
        if (audioSrc.captureLatencyMs > maxAudioCaptureLatencyMs) {
            maxAudioCaptureLatencyMs = audioSrc.captureLatencyMs;
        }
    }
    screenGrabLatencyReductionQpc = 0;
    if (screenGrab && maxAudioCaptureLatencyMs > 0.0f && config.screenGrabQueueCompensation) {
        // A screen-grab video timestamp is a composition/screen time: it already trails the application's
        // Present by the flip-queue latency, which the render->loopback probe knows nothing about. Moving
        // audio and video apart by the full probe latency then over-corrects (audio early by the queue
        // depth, measured 0-30 ms). Take the measured present-to-screen latency off the delay on BOTH sides:
        // here for the video delay, and through the media engine for the audio anchor it computes from the
        // per-source latency. Held for the session; without a hooked game's display timing the probe
        // latency stands.
        const double probeLatencyMs = static_cast<double>(maxAudioCaptureLatencyMs);
        ce::capture_policy::PresentToScreenLatency presentToScreen;
        DWORD captureTargetPid = 0;
        if (auto capture = media_main_g_WgcCap.Read()) {
            HWND captureWindow = nullptr;
            capture->GetTargetIdentity(&captureWindow, nullptr);
            if (captureWindow)
                GetWindowThreadProcessId(captureWindow, &captureTargetPid);
        }
        if (media_main_g_pSharedMem) {
            presentToScreen = ce::capture_policy::EstimatePresentToScreenLatency(
                media_main_g_pSharedMem->displayTiming, captureTargetPid);
        }
        const double reductionMs =
            ce::capture_policy::ComputeScreenGrabLatencyReductionMs(probeLatencyMs, presentToScreen);
        if (reductionMs > 0.0 && qpcFreq.QuadPart > 0) {
            screenGrabLatencyReductionQpc = static_cast<int64_t>(
                std::llround(reductionMs / 1000.0 * static_cast<double>(qpcFreq.QuadPart)));
        }
        maxAudioCaptureLatencyMs = static_cast<float>(probeLatencyMs - reductionMs);
        LogInfo(
            "[AVSyncApply] screen_grab_queue_compensation: source=%s targetPid=%lu probeMs=%.3f presentToScreenMedianMs=%.3f "
            "min=%.3f max=%.3f samples=%u reductionMs=%.3f effectiveMs=%.3f",
            presentToScreen.valid ? "display_timing"
                                  : (media_main_g_pSharedMem ? "display_timing_unavailable" : "no_hook"),
            captureTargetPid, probeLatencyMs, static_cast<double>(presentToScreen.medianUs) / 1000.0,
            static_cast<double>(presentToScreen.minUs) / 1000.0,
            static_cast<double>(presentToScreen.maxUs) / 1000.0, presentToScreen.samples, reductionMs,
            probeLatencyMs - reductionMs);
    }
    avContentDelayQpc =
        (maxAudioCaptureLatencyMs > 0.0f && qpcFreq.QuadPart > 0)
            ? static_cast<int64_t>(std::llround(static_cast<double>(maxAudioCaptureLatencyMs) / 1000.0 *
                                                static_cast<double>(qpcFreq.QuadPart)))
            : 0;
    avContentDelayActive = avContentDelayQpc > 0;
    // Smoothness FLOOR (WGC only): engage the active-delay jitter-absorbing playout even when there
    // is no audio-latency content delay. Configured = auto or explicit > 0; only meaningful for the
    // Screen-grab smoothness can arm without an A/V delay; the floor magnitude is resolved once
    // at the startup barrier from measured delivery jitter.
    wgcSmoothnessFloorConfigured = screenGrab && config.wgcSmoothnessBufferEnabled &&
                                              !config.video.useVFR &&
                                              (config.wgcSmoothnessFloorAuto || config.wgcSmoothnessFloorMs > 0);
    wgcSmoothnessDelayDesired =
        ce::capture_policy::WgcSmoothnessDelayDesired(avContentDelayActive, wgcSmoothnessFloorConfigured);
    // Inject delays content by retaining whole extra frames above its oldest-first reserve.
    injectContentDelayFrames =
        (avContentDelayActive && frameIntervalMs > 0.0)
            ? static_cast<size_t>(std::ceil(static_cast<double>(maxAudioCaptureLatencyMs) / frameIntervalMs))
            : 0;
    injectResidualEstimateMs = (injectContentDelayFrames > 0 && frameIntervalMs > 0.0)
                                                ? static_cast<double>(injectContentDelayFrames) * frameIntervalMs -
                                                      static_cast<double>(maxAudioCaptureLatencyMs)
                                                : 0.0;
    avContentDelayFrames =
        (avContentDelayActive && frameIntervalMs > 0.0) ? maxAudioCaptureLatencyMs / frameIntervalMs : 0.0;
    if (avContentDelayActive) {
        LogInfo(
            "[AVSyncApply] armed: maxAudioCaptureLatencyMs=%.3f delayUs=%lld method=%s injectDelayFrames=%zu "
            "videoDelayFrames=%.2f residualEstimateMs=%.3f confidence=%s reason=%s "
            "(delays video content to match late loopback audio; audio/PTS unchanged)",
            static_cast<double>(maxAudioCaptureLatencyMs),
            (long long)((avContentDelayQpc * 1000000) / qpcFreq.QuadPart),
            screenGrab ? "wgc-selection-bias" : "inject-buffer-reserve", injectContentDelayFrames,
            avContentDelayFrames, screenGrab ? 0.0 : injectResidualEstimateMs,
            config.avSyncConfidence.c_str(), config.avSyncReason.c_str());
        if (screenGrab) {
            LogInfo(
                "[AVSyncApply] wgc_cadence_policy: uniformCadence=%d (when active, WGC paces the content "
                "delay like the inject path: a delay-deep frame floor with unique frames advanced at the "
                "source input rate, so a VRR/under-delivering source keeps the realized delay stable and "
                "the source-limited repeats uniform instead of clustering into delay-slot holds)",
                config.wgcActiveDelayUniformCadence ? 1 : 0);
        }
    } else {
        LogWarn(
            "[AVSyncApply] inactive: maxAudioCaptureLatencyMs=%.3f delayUs=0 method=%s injectDelayFrames=0 "
            "residualEstimateMs=0.000 confidence=%s reason=%s",
            static_cast<double>(maxAudioCaptureLatencyMs),

            screenGrab ? "wgc-selection-bias" : "inject-buffer-reserve", config.avSyncConfidence.c_str(),
            config.avSyncReason.c_str());
    }

}

void MediaEncoderSession::ReleaseQueuedFrameTexture(QueuedFrame& queuedFrame) {

if (!queuedFrame.isInjectMode && queuedFrame.texture) {
    queuedFrame.texture->Release();
    queuedFrame.texture = nullptr;
}
if (!queuedFrame.isInjectMode) {
    queuedFrame.wgcPoolLease.Reset();
    queuedFrame.wgcPoolSlot = std::numeric_limits<uint32_t>::max();
    queuedFrame.wgcPoolGeneration = 0;
}

}

void MediaEncoderSession::DiscardQueuedFrame(QueuedFrame& queuedFrame) {

if (!queuedFrame.isInjectMode) {
    ReleaseQueuedFrameTexture(queuedFrame);
}
queuedFrame = QueuedFrame{};

}
