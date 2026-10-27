#pragma once

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall

#include <vulkan/vulkan.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "vulkan_present_metering_policy.h"

// D3D12 flip metering carried onto Vulkan present metering for D3D12-on-Vulkan devices.
//
// Native D3D12 DLSS frame generation meters its generated frames with
// `NvAPI_D3D12_SetFlipConfig(queue, vidpnSrcID, NV_FLIP_CONFIG*)`, called "right before the first
// Present() of each batch": the display engine then spaces the batch's flips evenly. Under
// vkd3d-proton that call has no implementation (dxvk-nvapi 0.9.2 names it only in its function-ID
// table) and vkd3d-proton never enables VK_NV_present_metering, so the batch reaches the screen at
// GPU completion. GTA V Enhanced, session 20261027_140018 (FG 2x, ~122 fps, 144 Hz VRR):
// `vkQueuePresentKHR` intervals alternated ~7.5/8.1 ms while the display-change series read 1.8 ms
// stddev and 2.7-3.1 ms jaggedness, and `nvFlipSchedule(... applied=0)` held for every vkd3d session
// against `applied=2728 avgDelayUs=3778` for native D3D12 on the same PC.
//
// The hook DLL answers the D3D12 call and binds each batch size to the D3D12 frame id of the
// batch's OUT_OF_BAND_PRESENT markers (hook/ngx/flip_metering_bridge_policy.h); vkd3d delivers that
// same frame id through `vkSetLatencyMarkerNV(OUT_OF_BAND_PRESENT_START)` immediately before the
// batch's presents on its present thread. A new frame id there is the first present of a batch,
// which is where `VkSetPresentConfigNV` belongs: it applies to the next numFramesPerBatch presents.
// Keyed by frame id, not by counting presents, so a present vkd3d drops cannot shift the batches.

namespace ce::vulkan_flip_metering_bridge {

// VkPhysicalDevicePresentMeteringFeaturesNV, mirrored for the same reason as
// ce::vulkan_present_metering_policy::SetPresentConfigNV: the MSYS2 headers predate the extension.
inline constexpr VkStructureType kStructureTypePhysicalDevicePresentMeteringFeaturesNV =
    static_cast<VkStructureType>(1000613001);

struct PhysicalDevicePresentMeteringFeaturesNV {
    VkStructureType sType;
    void* pNext;
    VkBool32 presentMetering;
};

#ifdef VK_NV_PRESENT_METERING_SPEC_VERSION
static_assert(static_cast<int>(kStructureTypePhysicalDevicePresentMeteringFeaturesNV) ==
                  static_cast<int>(VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_METERING_FEATURES_NV),
              "local VK_NV_present_metering feature sType must match the Vulkan headers");
static_assert(sizeof(PhysicalDevicePresentMeteringFeaturesNV) == sizeof(VkPhysicalDevicePresentMeteringFeaturesNV),
              "local VkPhysicalDevicePresentMeteringFeaturesNV mirror must match the Vulkan headers");
#endif

// vkd3d and vkd3d-proton name their instance engine "vkd3d" (VkApplicationInfo::pEngineName); the
// translation runtime, not the title, is what decides that D3D12 consumers can only meter through CE.
inline bool IsD3D12TranslationEngine(const char* engineName) {
    return engineName != nullptr && std::strcmp(engineName, "vkd3d") == 0;
}

struct DeviceInput {
    bool d3d12TranslationEngine = false;
    bool extensionAvailable = false;
    bool featureSupported = false;
    // The application enabled VK_NV_present_metering itself: its own generator owns metering.
    bool applicationEnabledExtension = false;
    // VkPhysicalDevicePresentMeteringFeaturesNV is already in the application's chain, so CE
    // cannot add its own (a duplicated sType is invalid).
    bool applicationChainHasFeatures = false;
};

enum class DeviceDecision {
    kEnable,
    kNotD3D12Translation,
    kApplicationOwnsMetering,
    kExtensionUnavailable,
    kFeatureUnsupported,
};

inline DeviceDecision DecideDeviceEnablement(const DeviceInput& input) {
    if (!input.d3d12TranslationEngine)
        return DeviceDecision::kNotD3D12Translation;
    if (input.applicationEnabledExtension || input.applicationChainHasFeatures)
        return DeviceDecision::kApplicationOwnsMetering;
    if (!input.extensionAvailable)
        return DeviceDecision::kExtensionUnavailable;
    if (!input.featureSupported)
        return DeviceDecision::kFeatureUnsupported;
    return DeviceDecision::kEnable;
}

inline const char* DescribeDeviceDecision(DeviceDecision decision) {
    switch (decision) {
        case DeviceDecision::kEnable:
            return "enabled for D3D12 flip metering";
        case DeviceDecision::kNotD3D12Translation:
            return "not a D3D12 translation device";
        case DeviceDecision::kApplicationOwnsMetering:
            return "the application owns VK_NV_present_metering";
        case DeviceDecision::kExtensionUnavailable:
            return "VK_NV_present_metering not advertised";
        case DeviceDecision::kFeatureUnsupported:
            return "presentMetering feature unsupported";
    }
    return "unknown";
}

// Batch sizes the hook bound to D3D12 frame ids, waiting for the batch's first present. Entries are
// published before the D3D12 Present() that vkd3d later turns into the Vulkan present, so a lookup
// never races its own publication. Not thread-safe; the runtime guards it.
inline constexpr size_t kPendingBatchCapacity = 16;

enum class PresentOutcome {
    // vkd3d announced no OUT_OF_BAND_PRESENT frame id for this present.
    kUnmarked,
    // A further present of the batch already started: the first present's node covers it.
    kBatchContinuation,
    // First present of a batch with a bound size: carries VkSetPresentConfigNV.
    kBatchStartMetered,
    // First present of a batch nothing was bound to (frame generation off, or metering disabled).
    kBatchStartUnmetered,
};

struct PresentDecision {
    PresentOutcome outcome = PresentOutcome::kUnmarked;
    // numFramesPerBatch for the node; 0 means no node.
    uint32_t framesPerBatch = 0;
    // The present belongs to a metered batch, so the generator owns its display placement.
    bool metered = false;
};

struct ScheduleStats {
    uint64_t published = 0;
    uint64_t meteredBatches = 0;
    uint64_t unmeteredBatches = 0;
    // Entries pushed out by a full table, or passed over by a newer frame id before their batch
    // presented. Either means a bound batch never reached Vulkan as a batch start.
    uint64_t evicted = 0;
    uint64_t skipped = 0;
};

class BatchSchedule {
public:
    // A size of 0 is the generator disabling metering for that batch, published like any other.
    void Publish(uint64_t frameId, uint32_t framesPerBatch) {
        if (frameId == 0)
            return;
        ++stats_.published;
        for (Entry& entry : entries_) {
            if (entry.frameId == frameId) {
                entry.framesPerBatch = framesPerBatch;
                return;
            }
        }
        Entry* slot = nullptr;
        for (Entry& entry : entries_) {
            if (entry.frameId == 0) {
                slot = &entry;
                break;
            }
        }
        if (!slot) {
            slot = &entries_[0];
            for (Entry& entry : entries_) {
                if (entry.frameId < slot->frameId)
                    slot = &entry;
            }
            ++stats_.evicted;
        }
        slot->frameId = frameId;
        slot->framesPerBatch = framesPerBatch;
    }

    // `swapchainLastBatchFrameId` is the swapchain's own record of the batch in flight.
    PresentDecision OnPresent(uint64_t presentFrameId, uint64_t& swapchainLastBatchFrameId, bool& swapchainMetered) {
        PresentDecision decision;
        if (presentFrameId == 0) {
            swapchainMetered = false;
            return decision;
        }
        if (presentFrameId == swapchainLastBatchFrameId) {
            decision.outcome = PresentOutcome::kBatchContinuation;
            decision.metered = swapchainMetered;
            return decision;
        }
        swapchainLastBatchFrameId = presentFrameId;
        uint32_t framesPerBatch = 0;
        bool found = false;
        for (Entry& entry : entries_) {
            if (entry.frameId == 0)
                continue;
            if (entry.frameId == presentFrameId) {
                framesPerBatch = entry.framesPerBatch;
                found = true;
                entry = {};
            } else if (entry.frameId < presentFrameId) {
                // dxvk-nvapi hands vkd3d monotonically increasing frame ids, so an older entry's
                // batch can no longer start.
                entry = {};
                ++stats_.skipped;
            }
        }
        swapchainMetered = found && framesPerBatch != 0;
        if (swapchainMetered) {
            ++stats_.meteredBatches;
            decision.outcome = PresentOutcome::kBatchStartMetered;
            decision.framesPerBatch = framesPerBatch;
            decision.metered = true;
        } else {
            ++stats_.unmeteredBatches;
            decision.outcome = PresentOutcome::kBatchStartUnmetered;
        }
        return decision;
    }

    const ScheduleStats& Stats() const { return stats_; }

private:
    struct Entry {
        uint64_t frameId = 0;
        uint32_t framesPerBatch = 0;
    };
    std::array<Entry, kPendingBatchCapacity> entries_{};
    ScheduleStats stats_;
};

// The node CE prepends to the batch's first present. Lives in the present call's frame.
inline ce::vulkan_present_metering_policy::SetPresentConfigNV MakePresentConfig(uint32_t framesPerBatch,
                                                                               const void* next) {
    ce::vulkan_present_metering_policy::SetPresentConfigNV config = {};
    config.sType = ce::vulkan_present_metering_policy::kStructureTypeSetPresentConfigNV;
    config.pNext = next;
    config.numFramesPerBatch = framesPerBatch;
    config.presentConfigFeedback = 0;
    return config;
}

}  // namespace ce::vulkan_flip_metering_bridge
