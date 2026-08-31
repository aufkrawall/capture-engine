#include "dx12_hook_internal.h"

namespace {

ce::capture_policy::FinalOutputTimelineState g_finalOutputTimeline;
CaptureCadenceGateState g_finalOutputCadenceGate;
std::atomic<uint32_t> g_skippedFinalOutputs{0};
std::atomic<int> g_finalOutputMultiplier{0};

int64_t GetQpcFrequency() {
    static const int64_t frequency = []() {
        LARGE_INTEGER value{};
        return QueryPerformanceFrequency(&value) ? value.QuadPart : 0;
    }();
    return frequency;
}

int64_t GetCurrentQpc() {
    LARGE_INTEGER value{};
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

float GetObservedFinalOutputFps() {
    if (auto* metrics = DXGIShared::GetPerformanceMetrics()) {
        const float presentationFps = metrics->GetCurrentFPS();
        if (presentationFps >= 10.0f && presentationFps <= 1000.0f)
            return presentationFps;
    }

    const float detectedOutputFps = g_FGCompat.GetOutputFPS();
    if (detectedOutputFps >= 10.0f && detectedOutputFps <= 1000.0f)
        return detectedOutputFps;

    return 60.0f * static_cast<float>(std::clamp(g_FGCompat.GetFGMultiplier(), 2, 4));
}

}  // namespace

void DX12_NoteSkippedStreamlineFinalOutput() {
    if (!DXGIShared::IsPostSLFinalOutputPresentCallback() ||
        !DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire))
        return;
    g_finalOutputTimeline.callbacksSinceSourcePresent.fetch_add(1, std::memory_order_relaxed);
    g_skippedFinalOutputs.fetch_add(1, std::memory_order_relaxed);
}

DX12FinalOutputCapturePlan DX12_PlanStreamlineFinalOutputCapture(SharedMemoryLayout* shm,
                                                                 const OverlayConfig& overlayConfig) {
    DX12FinalOutputCapturePlan plan;
    if (!DXGIShared::IsPostSLFinalOutputPresentCallback() ||
        !DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire))
        return plan;

    const int64_t qpcFrequency = GetQpcFrequency();
    const int64_t callbackQpc = GetCurrentQpc();
    const float outputFps = GetObservedFinalOutputFps();
    const int currentMultiplier = std::clamp(g_FGCompat.GetFGMultiplier(), 2, 4);
    const int previousMultiplier =
        g_finalOutputMultiplier.exchange(currentMultiplier, std::memory_order_acq_rel);
    if (previousMultiplier >= 2 && previousMultiplier != currentMultiplier) {
        ce::capture_policy::AdjustFinalOutputTimelineForMultiplierChange(
            g_finalOutputTimeline, static_cast<uint32_t>(previousMultiplier),
            static_cast<uint32_t>(currentMultiplier));
        HookLogImportant("DX12: Final-output clock adjusted for DLSS MFG multiplier %dx -> %dx",
                         previousMultiplier, currentMultiplier);
    }
    const uint32_t skippedOutputs = g_skippedFinalOutputs.exchange(0, std::memory_order_acq_rel);
    for (uint32_t i = 0; i < skippedOutputs; ++i) {
        ce::capture_policy::NextFinalOutputTimestampQpc(g_finalOutputTimeline, callbackQpc, qpcFrequency, outputFps,
                                                       false);
    }
    plan.metadata.timestampQpc = ce::capture_policy::NextFinalOutputTimestampQpc(
        g_finalOutputTimeline, callbackQpc, qpcFrequency, outputFps);
    plan.metadata.captureFlags = SHARED_FRAME_CAPTURE_FINAL_PRESENTED_OUTPUT;
    if (shm) {
        const auto watermark =
            ce::capture_policy::CaptureDisplayTimingPublicationWatermark(shm->displayTiming);
        if (watermark) {
            plan.metadata.displayTimingSequence = watermark.sequence;
            plan.metadata.displayTimingGeneration = watermark.generation;
            plan.metadata.captureFlags |= SHARED_FRAME_CAPTURE_DISPLAY_TIMING_WATERMARK;
        }
    }
    plan.captureCandidate = shm && g_IPC && g_IPC->IsRecording() &&
                            shm->runtimeState.IsInjectVideoCaptureRequested();
    plan.includeOverlay = overlayConfig.showOverlay && overlayConfig.captureIncludeOverlay;

    static std::atomic<uint64_t> s_planCount{0};
    const uint64_t planCount = s_planCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (planCount <= 10 || (planCount % 600) == 0) {
        HookLogImportant(
            "DX12: Final Streamline output #%llu planned (capture=%d includeOverlay=%d timestamp=%lld "
            "displayWatermark=%llu generation=%u skippedCallbacks=%u outputFps=%.1f multiplier=%d)",
            static_cast<unsigned long long>(planCount), plan.captureCandidate ? 1 : 0,
            plan.includeOverlay ? 1 : 0, static_cast<long long>(plan.metadata.timestampQpc),
            static_cast<unsigned long long>(plan.metadata.displayTimingSequence),
            plan.metadata.displayTimingGeneration, skippedOutputs, outputFps, currentMultiplier);
    }
    return plan;
}

bool DX12_TryClaimStreamlineFinalOutputCapture(DX12FinalOutputCapturePlan& plan) {
    if (plan.claimEvaluated)
        return false;
    plan.claimEvaluated = true;
    if (!plan.captureCandidate)
        return false;

    SharedMemoryLayout* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    const int64_t timestampUs = DisplayTimingQpcToUs(plan.metadata.timestampQpc, GetQpcFrequency());
    return !ShouldSkipCaptureForTargetCadenceAtUs(
        shm, "DX12 PostSL final output", timestampUs, g_finalOutputCadenceGate,
        ce::capture_policy::kFinalOutputCfrPublicationHeadroomPermille);
}

void DX12_ObserveStreamlineSourcePresentTiming() {
    if (!DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire))
        return;
    ce::capture_policy::ObserveFinalOutputSourcePresent(g_finalOutputTimeline, GetCurrentQpc(), GetQpcFrequency());
}

void DX12_ResetStreamlineFinalOutputCaptureTiming(const char* reason) {
    ce::capture_policy::ResetFinalOutputTimeline(g_finalOutputTimeline);
    g_finalOutputCadenceGate.Reset();
    g_skippedFinalOutputs.store(0, std::memory_order_release);
    g_finalOutputMultiplier.store(0, std::memory_order_release);
    HookLogImportant("DX12: Reset final Streamline output capture timing (%s)", reason ? reason : "state change");
}

bool DX12_ShouldUseStreamlineFinalOutputCapture() {
    return DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire) &&
           dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire) &&
           dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire) &&
           dx12_hook_g_PostSLCallbackExecutionEnabled.load(std::memory_order_acquire) &&
           DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr &&
           !HookOverlayObserverOnlyEnabled() && !dx12_hook_g_DeviceRemoved.load(std::memory_order_acquire);
}
