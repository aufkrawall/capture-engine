#include "vulkan_final_output_capture.h"

#include "../common/capture_pacing.h"
#include "layer_main.h"

namespace {

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

int64_t QpcDeltaToUs(int64_t deltaQpc, int64_t qpcFrequency) {
    if (qpcFrequency <= 0)
        return 0;
    return (deltaQpc / qpcFrequency) * 1'000'000 +
           ((deltaQpc % qpcFrequency) * 1'000'000) / qpcFrequency;
}

}  // namespace

VulkanFinalOutputCapturePlan PlanVulkanFinalOutputCapture(VulkanFinalOutputCaptureState& state,
                                                          SharedMemoryLayout* sharedMemory,
                                                          float observedOutputFps,
                                                          bool startsMeteredBatch,
                                                          bool meteredBatchOutput,
                                                          int currentMultiplier) {
    VulkanFinalOutputCapturePlan plan;
    std::unique_lock<std::mutex> planningLock(state.planningMutex, std::try_to_lock);
    if (!planningLock.owns_lock()) {
        state.skippedOutputs.fetch_add(1, std::memory_order_relaxed);
        if (meteredBatchOutput)
            state.timeline.callbacksSinceSourcePresent.fetch_add(1, std::memory_order_relaxed);
        const uint64_t total =
            state.planningContentionTotal.fetch_add(1, std::memory_order_relaxed) + 1;
        if (sharedMemory)
            sharedMemory->runtimeState.injectProducerCaptureLockDrops.fetch_add(1, std::memory_order_relaxed);
        if (total <= 10 || (total % 1000) == 0) {
            LayerLog("Vulkan Layer: final-output capture planning contention skipped output #%llu",
                     static_cast<unsigned long long>(total));
        }
        return plan;
    }

    const int64_t qpcFrequency = GetQpcFrequency();
    const int64_t callbackQpc = GetCurrentQpc();
    if (ce::capture_policy::UpdateFinalOutputCaptureEpoch(
            state.timeline, sharedMemory &&
                                sharedMemory->runtimeState.IsInjectVideoCaptureRequested())) {
        state.cadenceGate.Reset();
        state.observedMultiplier.store(0, std::memory_order_release);
        state.skippedOutputs.store(0, std::memory_order_release);
        LayerLog("Vulkan Layer: final-output clock reset for new recording capture epoch");
    }
    if (startsMeteredBatch) {
        ce::capture_policy::ObserveFinalOutputSourcePresent(state.timeline, callbackQpc,
                                                            qpcFrequency);
    }
    const int previousMultiplier =
        state.observedMultiplier.exchange(currentMultiplier, std::memory_order_acq_rel);
    ce::capture_policy::AdjustFinalOutputTimelineForMultiplierChange(
        state.timeline, static_cast<uint32_t>(previousMultiplier),
        static_cast<uint32_t>(currentMultiplier), callbackQpc);
    plan.skippedOutputs = state.skippedOutputs.exchange(0, std::memory_order_acq_rel);
    for (uint32_t index = 0; index < plan.skippedOutputs; ++index) {
        ce::capture_policy::NextFinalOutputTimestampQpc(state.timeline, callbackQpc, qpcFrequency,
                                                       observedOutputFps, false);
    }
    plan.metadata.timestampQpc = ce::capture_policy::NextFinalOutputTimestampQpc(
        state.timeline, callbackQpc, qpcFrequency, observedOutputFps,
        meteredBatchOutput);
    plan.virtualLeadUs = QpcDeltaToUs(plan.metadata.timestampQpc - callbackQpc, qpcFrequency);
    plan.metadata.captureFlags = SHARED_FRAME_CAPTURE_FINAL_PRESENTED_OUTPUT;
    if (sharedMemory) {
        const auto watermark =
            ce::capture_policy::CaptureDisplayTimingPublicationWatermark(sharedMemory->displayTiming);
        if (watermark) {
            plan.metadata.displayTimingSequence = watermark.sequence;
            plan.metadata.displayTimingGeneration = watermark.generation;
            plan.metadata.captureFlags |= SHARED_FRAME_CAPTURE_DISPLAY_TIMING_WATERMARK;
        }
    }

    const int64_t timestampUs = DisplayTimingQpcToUs(plan.metadata.timestampQpc, qpcFrequency);
    plan.shouldCapture = !ShouldSkipCaptureForTargetCadenceAtUs(
        sharedMemory, "Vulkan final output", timestampUs, state.cadenceGate,
        ce::capture_policy::kFinalOutputCfrPublicationHeadroomPermille);
    return plan;
}
