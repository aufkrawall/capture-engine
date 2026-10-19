#include "vulkan_queue_depth_pacing.h"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <unordered_map>

#include "vulkan_layer_internal.h"
#include "vulkan_present_metering_policy.h"

namespace policy = ce::vulkan_queue_depth_pacing;

namespace {

bool ChainHasStructure(const void* pNext, VkStructureType sType) {
    for (const auto* node = static_cast<const VkBaseInStructure*>(pNext); node; node = node->pNext) {
        if (node->sType == sType)
            return true;
    }
    return false;
}

bool ListHasExtension(const char* const* names, uint32_t count, const char* name) {
    for (uint32_t i = 0; i < count; ++i) {
        if (names && names[i] && std::strcmp(names[i], name) == 0)
            return true;
    }
    return false;
}

// One line per window; `generations` counts the swapchains it spanned.
void LogWindow(const policy::WindowStats& w, VkSwapchainKHR swapchain, int32_t configured, uint32_t imageCount,
               bool degraded, uint64_t nowUs, const char* reason) {
    LayerLog(
        "Vulkan Layer: queue-depth pacing sc=%p backbuffer_count=%d images=%u window=%llums generations=%u "
        "presents=%u waits=%u waitMeanUs=%llu waitMaxUs=%u unqueuedSkips=%u timeouts=%u degraded=%d (totals "
        "timeouts=%llu recoveries=%llu)%s",
        (void*)swapchain, configured, imageCount, static_cast<unsigned long long>((nowUs - w.startUs) / 1000ull),
        w.generations, w.presents, w.waits, static_cast<unsigned long long>(w.waits ? w.waitUs / w.waits : 0),
        w.maxWaitUs, w.skippedUnqueued, w.timeouts, degraded ? 1 : 0, static_cast<unsigned long long>(w.totalTimeouts),
        static_cast<unsigned long long>(w.totalRecoveries), reason);
}

void LogWindowIfDue(SwapchainData& sd, int32_t configured, uint64_t nowUs) {
    policy::WindowStats& w = sd.queueDepth.window;
    if (w.startUs == 0) {
        w.startUs = nowUs;
        return;
    }
    if (!policy::IsWindowDue(w, nowUs))
        return;
    LogWindow(w, sd.swapchain, configured, sd.imageCount, sd.queueDepth.degraded, nowUs, "");
    policy::ResetWindow(w, nowUs);
}

// Open windows of destroyed swapchains, keyed by their window, until the next swapchain there on the
// same device adopts one. Touched only at swapchain create/destroy.
struct CarriedWindow {
    VkDevice device = VK_NULL_HANDLE;
    policy::WindowStats window;
};
std::mutex g_carriedWindowsMutex;
std::unordered_map<HWND, CarriedWindow> g_carriedWindows;

}  // namespace

void PrepareQueueDepthPacingForDevice(PFN_vkGetPhysicalDeviceFeatures2 getFeatures2, VkPhysicalDevice physicalDevice,
                                      const VkDeviceCreateInfo& applicationInfo,
                                      const std::vector<VkExtensionProperties>& availableExtensions,
                                      std::vector<const char*>& extensions, VkDeviceCreateInfo& modifiedCreateInfo,
                                      QueueDepthPacingDeviceSetup& setup) {
    const auto available = [&](const char* name) {
        return std::any_of(availableExtensions.begin(), availableExtensions.end(),
                           [&](const VkExtensionProperties& ext) { return std::strcmp(ext.extensionName, name) == 0; });
    };
    const char* const* names = applicationInfo.ppEnabledExtensionNames;
    const uint32_t count = applicationInfo.enabledExtensionCount;

    policy::DeviceInput input;
    input.configuredBackbufferCount = VulkanLayerState::Get().GetBackbufferCount();
    input.presentIdExtensionAvailable = available(VK_KHR_PRESENT_ID_EXTENSION_NAME);
    input.presentWaitExtensionAvailable = available(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    input.applicationEnabledPresentId = ListHasExtension(names, count, VK_KHR_PRESENT_ID_EXTENSION_NAME);
    input.applicationEnabledPresentWait = ListHasExtension(names, count, VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    input.applicationEnabledPresentMetering =
        ListHasExtension(names, count, ce::vulkan_present_metering_policy::kExtensionName);
    input.applicationEnabledSwapchain = ListHasExtension(names, count, VK_KHR_SWAPCHAIN_EXTENSION_NAME);
    input.applicationChainHasPresentFeatures =
        ChainHasStructure(applicationInfo.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR) ||
        ChainHasStructure(applicationInfo.pNext, VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR);
    if (getFeatures2 && input.presentIdExtensionAvailable && input.presentWaitExtensionAvailable) {
        VkPhysicalDevicePresentWaitFeaturesKHR wait = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_WAIT_FEATURES_KHR,
                                                       nullptr, VK_FALSE};
        VkPhysicalDevicePresentIdFeaturesKHR id = {VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PRESENT_ID_FEATURES_KHR, nullptr,
                                                   VK_FALSE};
        id.pNext = &wait;
        VkPhysicalDeviceFeatures2 features = {};
        features.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2;
        features.pNext = &id;
        getFeatures2(physicalDevice, &features);
        input.presentIdFeatureSupported = id.presentId == VK_TRUE;
        input.presentWaitFeatureSupported = wait.presentWait == VK_TRUE;
    }
    setup.decision = policy::DecideDeviceEnablement(input);
    if (input.configuredBackbufferCount >= 2) {
        LayerLog(
            "Vulkan Layer: backbuffer_count=%d queue-depth pacing on this device: %s (extensions id/wait=%d/%d "
            "features=%d/%d applicationEnabled id/wait=%d/%d metering=%d swapchain=%d)",
            input.configuredBackbufferCount, policy::DescribeDeviceDecision(setup.decision),
            input.presentIdExtensionAvailable ? 1 : 0, input.presentWaitExtensionAvailable ? 1 : 0,
            input.presentIdFeatureSupported ? 1 : 0, input.presentWaitFeatureSupported ? 1 : 0,
            input.applicationEnabledPresentId ? 1 : 0, input.applicationEnabledPresentWait ? 1 : 0,
            input.applicationEnabledPresentMetering ? 1 : 0, input.applicationEnabledSwapchain ? 1 : 0);
    }
    if (setup.decision != policy::DeviceDecision::kEnable)
        return;

    setup.pNextBefore = modifiedCreateInfo.pNext;
    setup.extensionCountBefore = extensions.size();
    extensions.push_back(VK_KHR_PRESENT_ID_EXTENSION_NAME);
    extensions.push_back(VK_KHR_PRESENT_WAIT_EXTENSION_NAME);
    modifiedCreateInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    modifiedCreateInfo.ppEnabledExtensionNames = extensions.data();
    setup.presentIdFeatures.presentId = VK_TRUE;
    setup.presentWaitFeatures.presentWait = VK_TRUE;
    setup.presentWaitFeatures.pNext = const_cast<void*>(modifiedCreateInfo.pNext);
    setup.presentIdFeatures.pNext = &setup.presentWaitFeatures;
    modifiedCreateInfo.pNext = &setup.presentIdFeatures;
}

bool WithdrawQueueDepthPacingFromCreateInfo(std::vector<const char*>& extensions,
                                            VkDeviceCreateInfo& modifiedCreateInfo,
                                            QueueDepthPacingDeviceSetup& setup) {
    if (setup.decision != policy::DeviceDecision::kEnable)
        return false;
    extensions.resize(setup.extensionCountBefore);
    modifiedCreateInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    modifiedCreateInfo.ppEnabledExtensionNames = extensions.data();
    modifiedCreateInfo.pNext = setup.pNextBefore;
    setup.decision = policy::DeviceDecision::kRefusedByDevice;
    return true;
}

void FinishQueueDepthPacingForDevice(DeviceDispatch* dispatch, VkDevice device, PFN_vkGetDeviceProcAddr gdpa,
                                     const QueueDepthPacingDeviceSetup& setup) {
    if (!dispatch)
        return;
    if (setup.decision == policy::DeviceDecision::kRefusedByDevice) {
        LayerLog("Vulkan Layer: queue-depth pacing withdrawn - %s", policy::DescribeDeviceDecision(setup.decision));
        return;
    }
    if (setup.decision != policy::DeviceDecision::kEnable || !gdpa)
        return;
    dispatch->fp_vkWaitForPresentKHR = reinterpret_cast<PFN_vkWaitForPresentKHR>(gdpa(device, "vkWaitForPresentKHR"));
    dispatch->queueDepthPacingEnabled = dispatch->fp_vkWaitForPresentKHR != nullptr;
    LayerLog("Vulkan Layer: queue-depth pacing %s on device %p (vkWaitForPresentKHR=%p)",
             dispatch->queueDepthPacingEnabled ? "ready" : "UNAVAILABLE", (void*)device,
             reinterpret_cast<void*>(dispatch->fp_vkWaitForPresentKHR));
}

bool PaceVulkanPresentQueueDepth(DeviceDispatch* dispatch, VkDevice device, SwapchainData* swapchain,
                                 const VkPresentInfoKHR* presentInfo, bool frameGenerationOwnsPlacement,
                                 QueueDepthPresentTag& tag, const void*& presentChain) {
    if (!swapchain || !presentInfo)
        return false;
    const int32_t configured = VulkanLayerState::Get().GetBackbufferCount();
    if (configured < 2 && swapchain->queueDepth.loggedDecision < 0)
        return false;

    policy::PresentInput input;
    input.deviceEnabled = dispatch && dispatch->queueDepthPacingEnabled && dispatch->fp_vkWaitForPresentKHR;
    input.configuredBackbufferCount = configured;
    input.swapchainImageCount = swapchain->imageCount;
    input.presentSwapchainCount = presentInfo->swapchainCount;
    input.frameGenerationOwnsPlacement = frameGenerationOwnsPlacement;
    input.applicationSuppliedPresentId = ChainHasStructure(presentInfo->pNext, VK_STRUCTURE_TYPE_PRESENT_ID_KHR);
    const policy::PresentDecision decision = policy::DecidePresent(input);

    policy::SwapchainState& st = swapchain->queueDepth;
    if (static_cast<int32_t>(decision) != st.loggedDecision) {
        LayerLog("Vulkan Layer: queue-depth pacing for swapchain %p: %s (backbuffer_count=%d images=%u outstanding=%d)",
                 (void*)swapchain->swapchain, policy::DescribePresentDecision(decision), configured,
                 swapchain->imageCount, configured >= 2 ? configured - 1 : 0);
        st.loggedDecision = static_cast<int32_t>(decision);
    }
    if (decision != policy::PresentDecision::kPace)
        return false;

    const uint64_t presentId = ++st.nextPresentId;
    const uint64_t target = policy::WaitTargetFor(presentId, configured);
    ++st.window.presents;
    if (target != 0 && !policy::WasQueued(st, target)) {
        ++st.window.skippedUnqueued;
    } else if (target != 0) {
        const int64_t startUs = PerfLogger::GetQpcUs();
        const VkResult result =
            dispatch->fp_vkWaitForPresentKHR(device, swapchain->swapchain, target, policy::WaitTimeoutNs(st.degraded));
        const uint64_t waitedUs = static_cast<uint64_t>((std::max<int64_t>)(0, PerfLogger::GetQpcUs() - startUs));
        const policy::WaitOutcome outcome = policy::ClassifyWaitResult(result);
        ++st.window.waits;
        st.window.waitUs += waitedUs;
        st.window.maxWaitUs =
            (std::max)(st.window.maxWaitUs, static_cast<uint32_t>((std::min<uint64_t>)(waitedUs, UINT32_MAX)));
        const bool wasDegraded = st.degraded;
        st.degraded = policy::NextDegradedState(st.degraded, outcome);
        if (st.degraded && !wasDegraded) {
            ++st.window.timeouts;
            ++st.window.totalTimeouts;
            LayerLog(
                "Vulkan Layer: queue-depth pacing wait for present %llu on swapchain %p missed the %llu ms ceiling "
                "(result=%d) - checking without blocking until one of its presents completes again",
                static_cast<unsigned long long>(target), (void*)swapchain->swapchain,
                static_cast<unsigned long long>(policy::kWaitCeilingNs / 1000000ull), static_cast<int>(result));
        } else if (wasDegraded && !st.degraded) {
            ++st.window.totalRecoveries;
            LayerLog("Vulkan Layer: queue-depth pacing resumed on swapchain %p - present %llu completed",
                     (void*)swapchain->swapchain, static_cast<unsigned long long>(target));
        } else if (outcome == policy::WaitOutcome::kFailed) {
            static std::atomic<int> s_failureLogs{0};
            if (s_failureLogs.fetch_add(1, std::memory_order_relaxed) < 5) {
                LayerLog("Vulkan Layer: queue-depth pacing wait for present %llu failed result=%d (swapchain %p)",
                         static_cast<unsigned long long>(target), static_cast<int>(result),
                         (void*)swapchain->swapchain);
            }
        }
    }

    tag.presentId = presentId;
    tag.node.sType = VK_STRUCTURE_TYPE_PRESENT_ID_KHR;
    tag.node.pNext = presentChain;
    tag.node.swapchainCount = 1;
    tag.node.pPresentIds = &tag.presentId;
    presentChain = &tag.node;
    LogWindowIfDue(*swapchain, configured, static_cast<uint64_t>(PerfLogger::GetQpcUs()));
    return true;
}

void NoteVulkanQueueDepthPresentResult(SwapchainData* swapchain, const QueueDepthPresentTag& tag, VkResult result) {
    if (!swapchain || tag.presentId == 0)
        return;
    policy::NoteQueuedPresent(swapchain->queueDepth, tag.presentId,
                              result == VK_SUCCESS || result == VK_SUBOPTIMAL_KHR);
}

void HandOverVulkanQueueDepthWindow(SwapchainData& swapchain) {
    policy::WindowStats& w = swapchain.queueDepth.window;
    if (w.startUs == 0 || !swapchain.window)
        return;
    const uint64_t nowUs = static_cast<uint64_t>(PerfLogger::GetQpcUs());
    if (policy::IsWindowDue(w, nowUs)) {
        LogWindow(w, swapchain.swapchain, VulkanLayerState::Get().GetBackbufferCount(), swapchain.imageCount,
                  swapchain.queueDepth.degraded, nowUs, " at swapchain destroy");
        policy::ResetWindow(w, nowUs);
    }
    std::lock_guard<std::mutex> lock(g_carriedWindowsMutex);
    g_carriedWindows[swapchain.window] = CarriedWindow{swapchain.device, w};
}

void AdoptVulkanQueueDepthWindow(SwapchainData& swapchain) {
    if (!swapchain.window)
        return;
    std::lock_guard<std::mutex> lock(g_carriedWindowsMutex);
    const auto it = g_carriedWindows.find(swapchain.window);
    if (it == g_carriedWindows.end())
        return;
    if (it->second.device == swapchain.device)
        policy::CarryWindow(swapchain.queueDepth.window, it->second.window);
    g_carriedWindows.erase(it);
}