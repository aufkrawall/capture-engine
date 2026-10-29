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

// NVIDIA's Vulkan driver picks its presenter by that exact engine name: "vkd3d" (and "DXVK") present
// through a D3D11-backed DXGI swapchain that answers every VkSetPresentConfigNV with
// presentConfigFeedback 0x1 (NO_IFLIP); any other name gets the D3D12-backed one, which meters 2-frame
// batches after a 1-4 batch warm-up. build/vk-metering-probe, RTX 5070, driver 0x9b430000
// (2026-10-29), same swapchain and presents: engine none, "VKD3D", "vkd3d-proton" metered 116-178 of
// 120-181 batches, "vkd3d" 0 of 171-174, also under GTA V Enhanced's driver profile. None of the 32 bits
// of that profile's present-method flags (0x20324987) moved "vkd3d" off D3D11, and nvoglv64.dll reads
// its profile without NVAPI, so the name the driver sees is the only lever. GTA V Enhanced
// 20261029_135421 / 141337: 882 and 1924 metering requests, every one refused.
// The driver therefore sees the project's full name; CE itself keeps keying on the instance's own name.
inline constexpr char kDriverFacingD3D12TranslationEngineName[] = "vkd3d-proton";

struct DriverEngineNameInput {
    bool d3d12TranslationEngine = false;
    // NVIDIA's Vulkan ICD is mapped. The loader opens the ICDs before it builds the layer chain.
    bool nvidiaVulkanDriverMapped = false;
    // Streamline is mapped: DLSS frame generation, the only consumer of the bridge, runs inside it.
    bool streamlineMapped = false;
};

enum class DriverEngineNameDecision {
    kDriverFacingName,
    kNotD3D12Translation,
    kNotNvidiaDriver,
    kNoFrameGenerationRuntime,
};

// Only where the bridge can have a consumer does the driver see another name, so no other driver,
// title or vkd3d process loses whatever the exact name selects.
inline DriverEngineNameDecision DecideDriverEngineName(const DriverEngineNameInput& input) {
    if (!input.d3d12TranslationEngine)
        return DriverEngineNameDecision::kNotD3D12Translation;
    if (!input.nvidiaVulkanDriverMapped)
        return DriverEngineNameDecision::kNotNvidiaDriver;
    if (!input.streamlineMapped)
        return DriverEngineNameDecision::kNoFrameGenerationRuntime;
    return DriverEngineNameDecision::kDriverFacingName;
}

inline const char* DescribeDriverEngineNameDecision(DriverEngineNameDecision decision) {
    switch (decision) {
        case DriverEngineNameDecision::kDriverFacingName:
            return "the driver presents it on its metering (D3D12) presenter";
        case DriverEngineNameDecision::kNotD3D12Translation:
            return "not a D3D12 translation instance";
        case DriverEngineNameDecision::kNotNvidiaDriver:
            return "NVIDIA's Vulkan driver is not mapped";
        case DriverEngineNameDecision::kNoFrameGenerationRuntime:
            return "no Streamline in the process, so no flip-metering consumer";
    }
    return "unknown";
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

// A batch with something to space: one rendered frame plus at least one generated frame. RTX Remix's
// presenter, NVIDIA's own user of VK_NV_present_metering, asks for `1 + interpolatedFrameCount` and never
// for fewer. DLSS-G's D3D12 path configures single-frame batches while it warms up (not yet interpolating);
// NVIDIA's ICD answered that first single-frame batch with presentConfigFeedback 0x1 in both sessions where
// it was the only metered batch (20261027_155313, 20261029_123525), the refusal reached DLSS-G as NO_IFLIP,
// and DLSS-G never asked again for that swapchain. A single frame has no flips to space, so such a batch
// reaches Vulkan unmetered: no node, no driver answer to carry back.
inline constexpr uint32_t kMinMeteredFramesPerBatch = 2;

enum class PresentOutcome {
    // vkd3d announced no OUT_OF_BAND_PRESENT frame id for this present.
    kUnmarked,
    // A further present of the batch already started: the first present's node covers it.
    kBatchContinuation,
    // First present of a batch with a bound size: carries VkSetPresentConfigNV.
    kBatchStartMetered,
    // First present of a batch nothing was bound to (frame generation off, metering disabled, or a
    // single-frame batch).
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
    // Bound batches below kMinMeteredFramesPerBatch, presented unmetered (included in unmeteredBatches).
    uint64_t singleFrameBatches = 0;
    // The driver's answers to metered presents (presentConfigFeedback 0 / nonzero).
    uint64_t driverMetered = 0;
    uint64_t driverRefused = 0;
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
        swapchainMetered = found && framesPerBatch >= kMinMeteredFramesPerBatch;
        if (found && framesPerBatch != 0 && !swapchainMetered)
            ++stats_.singleFrameBatches;
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

    // The driver wrote `presentConfigFeedback` into the node of a metered present on `swapchain`; nonzero
    // means it did not meter that batch. The D3D12 generator reads the latest answer back through
    // SetFlipConfig's feedback mask, so it paces a refused batch itself instead of trusting metering that
    // never happened (session 20261027_151844: every batch refused with 0x1, presents bunched 0.3 ms apart).
    void OnMeteredPresentAnswered(uint64_t swapchain, uint32_t presentConfigFeedback) {
        answeredSwapchain_ = swapchain;
        driverFeedback_ = presentConfigFeedback;
        ++(presentConfigFeedback == 0 ? stats_.driverMetered : stats_.driverRefused);
    }

    // A destroyed swapchain's answer says nothing about its successor, which the next metered present asks
    // afresh. Without this a refusal would outlive the swapchain it was about and keep a generator off
    // metering that the new swapchain may well accept.
    void OnSwapchainRetired(uint64_t swapchain) {
        if (swapchain != 0 && swapchain == answeredSwapchain_) {
            answeredSwapchain_ = 0;
            driverFeedback_ = 0;
        }
    }

    // 0 until the driver refused a batch on a live swapchain.
    uint32_t DriverFeedback() const { return driverFeedback_; }

    const ScheduleStats& Stats() const { return stats_; }

private:
    struct Entry {
        uint64_t frameId = 0;
        uint32_t framesPerBatch = 0;
    };
    std::array<Entry, kPendingBatchCapacity> entries_{};
    ScheduleStats stats_;
    uint64_t answeredSwapchain_ = 0;
    uint32_t driverFeedback_ = 0;
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
