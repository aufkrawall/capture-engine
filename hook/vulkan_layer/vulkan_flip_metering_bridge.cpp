// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall

#include "vulkan_flip_metering_bridge.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <mutex>

#include "common/logging/log_meter.h"
#include "layer_wsi_surface_bridge.h"
#include "vulkan_layer_internal.h"

namespace policy = ce::vulkan_flip_metering_bridge;
namespace metering = ce::vulkan_present_metering_policy;

namespace {

// vkd3d announces each present's frame id with this marker on its present thread.
constexpr VkLatencyMarkerNV kOutOfBandPresentStart = static_cast<VkLatencyMarkerNV>(10);

std::mutex g_scheduleMutex;
policy::BatchSchedule g_schedule;
std::atomic<bool> g_bridgeAvailable{false};
std::atomic<bool> g_meteringEngaged{false};

bool ListHasExtension(const char* const* names, uint32_t count, const char* name) {
    for (uint32_t i = 0; i < count; ++i) {
        if (names && names[i] && std::strcmp(names[i], name) == 0)
            return true;
    }
    return false;
}

// Non-dispatchable handles are pointers on x64 and 64-bit integers on x86.
uint64_t SwapchainKey(VkSwapchainKHR swapchain) {
    return (uint64_t)(swapchain);
}

bool ChainHasStructure(const void* pNext, VkStructureType sType) {
    for (const auto* node = static_cast<const VkBaseInStructure*>(pNext); node; node = node->pNext) {
        if (node->sType == sType)
            return true;
    }
    return false;
}

void LogScheduleState(const SwapchainData* sd, const policy::PresentDecision& decision,
                      const policy::ScheduleStats& stats) {
    // One line when a swapchain's metering engages, disengages or changes its batch size; the
    // counters say how the schedule fared in between.
    static ce::log_meter::ChangeGate s_stateGate(10000);
    const uint64_t key = ce::log_meter::FieldKey(static_cast<const void*>(sd), decision.metered,
                                                 decision.metered ? decision.framesPerBatch : 0u);
    if (decision.outcome != policy::PresentOutcome::kBatchStartMetered &&
        decision.outcome != policy::PresentOutcome::kBatchStartUnmetered) {
        return;
    }
    if (const auto verdict = s_stateGate.Observe(key, GetTickCount64())) {
        LayerLog(
            "Vulkan Layer: D3D12 flip metering %s sc=%p numFramesPerBatch=%u (batches metered=%llu unmetered=%llu "
            "singleFrame=%llu published=%llu skipped=%llu evicted=%llu; driver metered=%llu refused=%llu)%s",
            decision.metered ? "drives" : "idle on", (void*)(sd ? sd->swapchain : VK_NULL_HANDLE),
            decision.framesPerBatch, static_cast<unsigned long long>(stats.meteredBatches),
            static_cast<unsigned long long>(stats.unmeteredBatches),
            static_cast<unsigned long long>(stats.singleFrameBatches), static_cast<unsigned long long>(stats.published),
            static_cast<unsigned long long>(stats.skipped), static_cast<unsigned long long>(stats.evicted),
            static_cast<unsigned long long>(stats.driverMetered), static_cast<unsigned long long>(stats.driverRefused),
            ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
}

}  // namespace

void PrepareFlipMeteringBridgeForDevice(bool d3d12TranslationEngine, PFN_vkGetPhysicalDeviceFeatures2 getFeatures2,
                                        VkPhysicalDevice physicalDevice, const VkDeviceCreateInfo& applicationInfo,
                                        const std::vector<VkExtensionProperties>& availableExtensions,
                                        std::vector<const char*>& extensions, VkDeviceCreateInfo& modifiedCreateInfo,
                                        FlipMeteringBridgeDeviceSetup& setup) {
    policy::DeviceInput input;
    input.d3d12TranslationEngine = d3d12TranslationEngine;
    input.extensionAvailable =
        std::any_of(availableExtensions.begin(), availableExtensions.end(), [](const VkExtensionProperties& ext) {
            return std::strcmp(ext.extensionName, metering::kExtensionName) == 0;
        });
    input.applicationEnabledExtension = ListHasExtension(applicationInfo.ppEnabledExtensionNames,
                                                         applicationInfo.enabledExtensionCount, metering::kExtensionName);
    input.applicationChainHasFeatures =
        ChainHasStructure(applicationInfo.pNext, policy::kStructureTypePhysicalDevicePresentMeteringFeaturesNV);
    if (getFeatures2 && input.extensionAvailable && d3d12TranslationEngine) {
        policy::PhysicalDevicePresentMeteringFeaturesNV supported = {
            policy::kStructureTypePhysicalDevicePresentMeteringFeaturesNV, nullptr, VK_FALSE};
        VkPhysicalDeviceFeatures2 features = {};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &supported;
        getFeatures2(physicalDevice, &features);
        input.featureSupported = supported.presentMetering == VK_TRUE;
    }
    setup.decision = policy::DecideDeviceEnablement(input);
    if (d3d12TranslationEngine) {
        LayerLog(
            "Vulkan Layer: D3D12 translation device - %s for the D3D12 flip-metering bridge (extension=%d "
            "feature=%d applicationEnabled=%d applicationFeatures=%d)",
            policy::DescribeDeviceDecision(setup.decision), input.extensionAvailable ? 1 : 0,
            input.featureSupported ? 1 : 0, input.applicationEnabledExtension ? 1 : 0,
            input.applicationChainHasFeatures ? 1 : 0);
    }
    if (setup.decision != policy::DeviceDecision::kEnable)
        return;

    setup.pNextBefore = modifiedCreateInfo.pNext;
    setup.extensionCountBefore = extensions.size();
    extensions.push_back(metering::kExtensionName);
    modifiedCreateInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    modifiedCreateInfo.ppEnabledExtensionNames = extensions.data();
    setup.features.sType = policy::kStructureTypePhysicalDevicePresentMeteringFeaturesNV;
    setup.features.pNext = const_cast<void*>(modifiedCreateInfo.pNext);
    setup.features.presentMetering = VK_TRUE;
    modifiedCreateInfo.pNext = &setup.features;
    setup.applied = true;
}

bool WithdrawFlipMeteringBridgeFromCreateInfo(std::vector<const char*>& extensions,
                                              VkDeviceCreateInfo& modifiedCreateInfo,
                                              FlipMeteringBridgeDeviceSetup& setup) {
    if (!setup.applied)
        return false;
    // Called after any later CE addition was withdrawn, so this node heads the chain again.
    extensions.resize(setup.extensionCountBefore);
    modifiedCreateInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    modifiedCreateInfo.ppEnabledExtensionNames = extensions.data();
    modifiedCreateInfo.pNext = setup.pNextBefore;
    setup.applied = false;
    LayerLog("Vulkan Layer: vkCreateDevice refused VK_NV_present_metering; retrying without the D3D12 flip-metering "
             "bridge");
    return true;
}

void FinishFlipMeteringBridgeForDevice(DeviceDispatch* dispatch, const VkDeviceCreateInfo& applicationInfo,
                                       const FlipMeteringBridgeDeviceSetup& setup) {
    if (!dispatch)
        return;
    // The application's own list, not CE's copy: it is the statement that a metered frame generator of
    // the application's runs here.
    if (ListHasExtension(applicationInfo.ppEnabledExtensionNames, applicationInfo.enabledExtensionCount,
                         metering::kExtensionName)) {
        dispatch->applicationEnabledPresentMetering = true;
        ce::vulkan_wsi::PublishDevicePresentMetering();
        LayerLog("Vulkan Layer: application enabled %s - a metered frame generator owns the display placement of "
                 "its generated images on this device, so CE schedules none of its presents",
                 metering::kExtensionName);
    }
    if (!setup.applied)
        return;
    dispatch->flipMeteringBridgeEnabled = true;
    g_bridgeAvailable.store(true, std::memory_order_release);
    LayerLog("Vulkan Layer: D3D12 flip-metering bridge ready on device %p (vkSetLatencyMarkerNV=%p)",
             (void*)dispatch->device, reinterpret_cast<void*>(dispatch->fp_vkSetLatencyMarkerNV));
}

extern "C" VKAPI_ATTR void VKAPI_CALL Capture_vkSetLatencyMarkerNV(VkDevice device, VkSwapchainKHR swapchain,
                                                                   const VkSetLatencyMarkerInfoNV* pLatencyMarkerInfo) {
    DeviceDispatch* disp = VulkanLayerState::Get().GetDeviceDispatch(device);
    if (disp && disp->fp_vkSetLatencyMarkerNV)
        disp->fp_vkSetLatencyMarkerNV(device, swapchain, pLatencyMarkerInfo);
    if (!disp || !disp->flipMeteringBridgeEnabled || !pLatencyMarkerInfo ||
        pLatencyMarkerInfo->marker != kOutOfBandPresentStart) {
        return;
    }
    if (SwapchainData* sd = VulkanLayerState::Get().GetSwapchainData(swapchain))
        sd->outOfBandPresentFrameId.store(pLatencyMarkerInfo->presentID, std::memory_order_release);
}

void PlanFlipMeteringPresent(const DeviceDispatch* disp, SwapchainData* sd, const VkPresentInfoKHR* pPresentInfo,
                             FlipMeteringPresentTag& tag) {
    tag = FlipMeteringPresentTag{};
    if (!disp || !disp->flipMeteringBridgeEnabled || !sd || !pPresentInfo || pPresentInfo->swapchainCount != 1)
        return;
    // Consumed here, so a marker that no present followed cannot attach to a later one.
    const uint64_t frameId = sd->outOfBandPresentFrameId.exchange(0, std::memory_order_acq_rel);
    policy::ScheduleStats stats;
    {
        std::lock_guard<std::mutex> lock(g_scheduleMutex);
        tag.decision = g_schedule.OnPresent(frameId, sd->flipMeteringBatchFrameId, sd->flipMeteringMetered);
        stats = g_schedule.Stats();
    }
    // Follows the generator's request, not the driver's answer: CE's vertical-blank handling switching on a
    // refusal would change the presents the driver answers for, and the two could chase each other.
    g_meteringEngaged.store(tag.decision.metered, std::memory_order_release);
    if (tag.decision.framesPerBatch != 0)
        tag.node = policy::MakePresentConfig(tag.decision.framesPerBatch, nullptr);
    LogScheduleState(sd, tag.decision, stats);
}

bool AttachFlipMeteringNode(FlipMeteringPresentTag& tag, const void*& finalPresentChain) {
    if (tag.decision.framesPerBatch == 0)
        return false;
    tag.node.pNext = finalPresentChain;
    finalPresentChain = &tag.node;
    tag.attached = true;
    return true;
}

void NoteFlipMeteringPresentResult(SwapchainData* sd, const FlipMeteringPresentTag& tag, VkResult result) {
    if (!tag.attached)
        return;
    // Only a present the driver took has an answer in the node.
    const bool answered = result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR;
    policy::ScheduleStats stats;
    if (answered) {
        std::lock_guard<std::mutex> lock(g_scheduleMutex);
        g_schedule.OnMeteredPresentAnswered(SwapchainKey(sd ? sd->swapchain : VK_NULL_HANDLE),
                                            tag.node.presentConfigFeedback);
        stats = g_schedule.Stats();
    }
    // A nonzero feedback is the driver saying the batch could not be metered as asked; the swapchain shape
    // is on the line because it is the first suspect when a whole session is refused.
    static ce::log_meter::ChangeGate s_feedbackGate;
    const uint64_t key = ce::log_meter::FieldKey(tag.node.presentConfigFeedback, answered, tag.node.numFramesPerBatch);
    if (const auto verdict = s_feedbackGate.Observe(key)) {
        LayerLog("Vulkan Layer: D3D12 flip metering present sc=%p numFramesPerBatch=%u result=%d "
                 "presentConfigFeedback=0x%X - %s (driver metered=%llu refused=%llu; swapchain presentMode=%d "
                 "images=%u)%s",
                 (void*)(sd ? sd->swapchain : VK_NULL_HANDLE), tag.node.numFramesPerBatch, static_cast<int>(result),
                 tag.node.presentConfigFeedback,
                 !answered                               ? "no answer, the present failed"
                 : tag.node.presentConfigFeedback == 0 ? "the driver meters the batch"
                                                         : "the driver did NOT meter the batch; the D3D12 generator "
                                                           "is told so and paces it itself",
                 static_cast<unsigned long long>(stats.driverMetered),
                 static_cast<unsigned long long>(stats.driverRefused), sd ? static_cast<int>(sd->presentMode) : -1,
                 sd ? sd->imageCount : 0u, ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
}

void RetireFlipMeteringSwapchain(VkSwapchainKHR swapchain) {
    std::lock_guard<std::mutex> lock(g_scheduleMutex);
    g_schedule.OnSwapchainRetired(SwapchainKey(swapchain));
}

void ReportApplicationPresentMetering(SwapchainData* sd, const VkPresentInfoKHR* pPresentInfo,
                                      const metering::ChainScan& scan) {
    if (!sd || !pPresentInfo || !scan.found)
        return;
    metering::Input meteringInput = {};
    meteringInput.vblankPacedPresentationRequested = true;
    meteringInput.swapchainPresentModeKnown = true;
    meteringInput.swapchainPresentMode = sd->presentMode;
    meteringInput.swapchainCount = pPresentInfo->swapchainCount;
    meteringInput.meteredFramesPerBatch = scan.framesPerBatch;
    const metering::Decision decision = metering::Decide(meteringInput);
    // One line per state, not per present: activation, a frame generation multiplier change, or a chain
    // shape change.
    const uint32_t logState = (scan.framesPerBatch & 0xFFFFu) | (decision.preserveMetering ? 0x10000u : 0u) |
                              (scan.isChainHead ? 0x20000u : 0u) | 0x80000000u;
    if (sd->presentMeteringLogState.load(std::memory_order_relaxed) != logState &&
        sd->presentMeteringLogState.exchange(logState, std::memory_order_acq_rel) != logState) {
        LayerLog(
            "Vulkan Layer: frame-generation present metering numFramesPerBatch=%u "
            "(swapchain presentMode=%d images=%u chainNodes=%u chainHead=%d) - %s",
            scan.framesPerBatch, static_cast<int>(sd->presentMode), sd->imageCount, scan.nodeCount,
            scan.isChainHead ? 1 : 0,
            decision.preserveMetering ? "preserved for generated-frame spacing; CE schedules none of these presents"
                                      : "left in place unchanged");
    }
}

namespace ce::vulkan_flip_metering_bridge {
bool IsMeteringEngaged() {
    return g_meteringEngaged.load(std::memory_order_acquire);
}
}  // namespace ce::vulkan_flip_metering_bridge

extern "C" __declspec(dllexport) BOOL CEVulkanLayerFlipMeteringBridgeAvailable(void) {
    return g_bridgeAvailable.load(std::memory_order_acquire) ? TRUE : FALSE;
}

extern "C" __declspec(dllexport) void CEVulkanLayerPublishFlipMeteringBatch(UINT64 frameId, UINT32 framesPerBatch) {
    std::lock_guard<std::mutex> lock(g_scheduleMutex);
    g_schedule.Publish(frameId, framesPerBatch);
}

extern "C" __declspec(dllexport) UINT32 CEVulkanLayerFlipMeteringFeedback(void) {
    std::lock_guard<std::mutex> lock(g_scheduleMutex);
    return g_schedule.DriverFeedback();
}
