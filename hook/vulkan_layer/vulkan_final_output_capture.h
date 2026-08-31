#pragma once

#include <atomic>
#include <mutex>

#include "../../common/capture_policy/final_output_timing.h"
#include "../../common/shared_defs.h"

struct VulkanFinalOutputCaptureState {
    std::mutex planningMutex;
    ce::capture_policy::FinalOutputTimelineState timeline;
    ce::capture_policy::FinalOutputCadenceState cadenceGate;
    std::atomic<int> observedMultiplier{0};
    std::atomic<uint32_t> skippedOutputs{0};
    std::atomic<uint64_t> planningContentionTotal{0};
};

struct VulkanFinalOutputCapturePlan {
    FrameCaptureMetadata metadata;
    uint32_t skippedOutputs = 0;
    bool shouldCapture = false;
};

VulkanFinalOutputCapturePlan PlanVulkanFinalOutputCapture(VulkanFinalOutputCaptureState& state,
                                                          SharedMemoryLayout* sharedMemory,
                                                          float observedOutputFps,
                                                          bool startsMeteredBatch,
                                                          bool meteredBatchOutput,
                                                          int currentMultiplier);
