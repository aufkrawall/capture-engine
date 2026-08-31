#include "vulkan_layer_internal.h"

#include "vulkan_reflex_limiter.h"

#include <algorithm>
#include <array>

bool VulkanReflexLimiter::QueryLatencyReport(VkDevice device, ce::system_latency::NativeReport& report) {
    report = {};
    std::lock_guard<std::mutex> lock(mutex_);
    if (device == VK_NULL_HANDLE || device != device_ || modernSwapchain_ == VK_NULL_HANDLE || !dispatch_ ||
        !dispatch_->fp_vkGetDeviceProcAddr) {
        return false;
    }

    const auto getLatencyTimings = reinterpret_cast<PFN_vkGetLatencyTimingsNV>(
        dispatch_->fp_vkGetDeviceProcAddr(device_, "vkGetLatencyTimingsNV"));
    if (!getLatencyTimings)
        return false;

    std::array<VkLatencyTimingsFrameReportNV, ce::system_latency::NativeReport::kCapacity> timings{};
    for (auto& timing : timings)
        timing.sType = VK_STRUCTURE_TYPE_LATENCY_TIMINGS_FRAME_REPORT_NV;

    VkGetLatencyMarkerInfoNV markerInfo{VK_STRUCTURE_TYPE_GET_LATENCY_MARKER_INFO_NV};
    markerInfo.timingCount = static_cast<uint32_t>(timings.size());
    markerInfo.pTimings = timings.data();
    getLatencyTimings(device_, modernSwapchain_, &markerInfo);

    report.count = (std::min)(static_cast<size_t>(markerInfo.timingCount), report.frames.size());
    bool hasMarkers = false;
    for (size_t i = 0; i < report.count; ++i) {
        auto& destination = report.frames[i];
        const auto& source = timings[i];
        destination.frameId = source.presentID;
        destination.inputSampleTimeUs = source.inputSampleTimeUs;
        destination.simulationStartTimeUs = source.simStartTimeUs;
        destination.presentStartTimeUs = source.presentStartTimeUs;
        destination.gpuRenderEndTimeUs = source.gpuRenderEndTimeUs;
        hasMarkers = hasMarkers || (source.simStartTimeUs != 0 && source.presentStartTimeUs != 0);
    }
    if (!hasMarkers)
        report = {};
    return hasMarkers;
}

namespace ce::system_latency {

bool QueryNativeReport(void* device, NativeReport& report) {
    return g_VulkanReflexLimiter.QueryLatencyReport(reinterpret_cast<VkDevice>(device), report);
}

}  // namespace ce::system_latency

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkSetLatencySleepModeNV(VkDevice device, VkSwapchainKHR swapchain,
                                                               const VkLatencySleepModeInfoNV* pSleepModeInfo) {
    DeviceDispatch* dispatch = VulkanLayerState::Get().GetDeviceDispatch(device);
    return g_VulkanReflexLimiter.InterceptSetSleepMode(
        device, swapchain, pSleepModeInfo, dispatch ? dispatch->fp_vkSetLatencySleepModeNV : nullptr);
}

VKAPI_ATTR VkResult VKAPI_CALL Capture_vkLatencySleepNV(VkDevice device, VkSwapchainKHR swapchain,
                                                        const VkLatencySleepInfoNV* pSleepInfo) {
    DeviceDispatch* dispatch = VulkanLayerState::Get().GetDeviceDispatch(device);
    return g_VulkanReflexLimiter.InterceptSleep(device, swapchain, pSleepInfo,
                                                dispatch ? dispatch->fp_vkLatencySleepNV : nullptr);
}
