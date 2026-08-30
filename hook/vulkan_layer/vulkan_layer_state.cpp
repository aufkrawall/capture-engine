#include "vulkan_layer_internal.h"
#include "vulkan_prerender_policy.h"

#include <iterator>

#include "../common/vulkan_wsi_surface_table.h"
#include "layer_wsi_surface_bridge.h"

VulkanLayerState& VulkanLayerState::Get() {
    static VulkanLayerState instance;
    return instance;
}

VulkanLayerState::VulkanLayerState()
    : m_OverlayEnabled(true),
      m_CaptureEnabled(true),
      m_MaxAnisotropy(16),
      m_AnisotropyOverrideActive(false),
      m_MipLodBias(0.0f),
      m_MipBiasOverrideActive(false),
      m_ForceMipBiasClamp(false),
      m_MipBiasMode("strict"),
      m_MipMapping("default"),
      m_SamplerOverrideMode("safe"),
      m_VsyncMode("default"),
      m_BackbufferCount(0),
      m_PrerenderLimit(-1.0f) {}

void VulkanLayerState::RegisterInstance(VkInstance instance, InstanceDispatch* dispatch) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_Instances[instance] = dispatch;
    m_InstanceRegistry.AddInstance(instance, ce::vulkan_instance_registry::DispatchKey(instance));
}

void VulkanLayerState::UnregisterInstance(VkInstance instance) {
    std::vector<HWND> windowsToRetire;
    {
        std::lock_guard<std::recursive_mutex> lock(m_Lock);
        auto it = m_Instances.find(instance);
        if (it != m_Instances.end()) {
            delete it->second;
            m_Instances.erase(it);
        }
        m_InstanceRegistry.RemoveInstance(instance);
        // vkDestroyInstance also destroys every surface the instance still
        // owned, so an application that skips explicit vkDestroySurfaceKHR
        // calls must not leave its HWNDs published as live Vulkan targets. The
        // sweep erases the owned surfaces and selects one window per surface;
        // retirement runs after m_Lock is released because the bridge's spin
        // section never nests under a layer lock.
        ce::vulkan_wsi_surfaces::SelectWindowsToRetireOnInstanceDestroy(
            m_Surfaces, instance, std::back_inserter(windowsToRetire));
    }
    // The bridge refcounts per HWND: a window shared by several retired
    // surfaces stays live until its last surface is retired.
    for (const HWND window : windowsToRetire)
        ce::vulkan_wsi::RetireLiveSurfaceHwnd(window);
}

InstanceDispatch* VulkanLayerState::GetInstanceDispatch(VkInstance instance) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Instances.find(instance);
    if (it != m_Instances.end())
        return it->second;
    // The handle is not one CE recorded. Resolving it through the loader
    // dispatch key still reaches the right chain, and returning nullptr here
    // would make vkGetInstanceProcAddr report functions as unsupported.
    const auto lookup = m_InstanceRegistry.ResolveInstance(instance);
    if (lookup.instance == nullptr)
        return nullptr;
    auto resolved = m_Instances.find(static_cast<VkInstance>(lookup.instance));
    return (resolved != m_Instances.end()) ? resolved->second : nullptr;
}

void VulkanLayerState::RegisterDevice(VkDevice device, DeviceDispatch* dispatch) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_Devices[device] = dispatch;
    const void* key = ce::vulkan_instance_registry::DispatchKey(device);
    if (key != nullptr)
        m_DevicesByDispatchKey[key] = device;
}

void VulkanLayerState::UnregisterDevice(VkDevice device) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Devices.find(device);
    if (it != m_Devices.end()) {
        delete it->second;
        m_Devices.erase(it);
    }
    // A VkQueue dies with its device and its handle value can come back on the
    // next one. Leaving the queue maps populated would let a stale entry answer
    // a lookup for a queue CE never saw created.
    // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order) - erase-by-value, order-independent
    for (auto queueIt = m_Queues.begin(); queueIt != m_Queues.end();) {
        if (queueIt->second != device) {
            ++queueIt;
            continue;
        }
        m_QueueFamilies.erase(queueIt->first);
        m_QueueFlags.erase(queueIt->first);
        m_QueueLastSubmitThreadIds.erase(queueIt->first);
        m_QueueGraphicsProducerQueues.erase(queueIt->first);
        m_QueueGraphicsProducerBoundaries.erase(queueIt->first);
        // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order) - erase-by-value, order-independent
        for (auto boundaryIt = m_PrerenderProducerBoundaryQueues.begin();
             boundaryIt != m_PrerenderProducerBoundaryQueues.end();) {
            boundaryIt = (boundaryIt->second == queueIt->first)
                             ? m_PrerenderProducerBoundaryQueues.erase(boundaryIt)
                             : std::next(boundaryIt);
        }
        queueIt = m_Queues.erase(queueIt);
    }
    m_DeviceLastSubmitThreadIds.erase(device);
    m_DeviceLastGraphicsSubmitQueues.erase(device);
    // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order) - erase-by-value, order-independent
    for (auto keyIt = m_DevicesByDispatchKey.begin(); keyIt != m_DevicesByDispatchKey.end();) {
        keyIt = (keyIt->second == device) ? m_DevicesByDispatchKey.erase(keyIt) : std::next(keyIt);
    }
}

DeviceDispatch* VulkanLayerState::GetDeviceDispatch(VkDevice device) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Devices.find(device);
    if (it != m_Devices.end())
        return it->second;
    return ResolveDispatchByKey(device);
}

// Last resort for a dispatchable device-level handle CE never recorded. The
// loader stamps a VkQueue with its VkDevice's dispatch table pointer, so the
// device's own key answers for its queues too - and forwarding down the right
// chain always beats failing a call the application made correctly.
DeviceDispatch* VulkanLayerState::ResolveDispatchByKey(const void* dispatchableHandle) {
    const void* key = ce::vulkan_instance_registry::DispatchKey(dispatchableHandle);
    if (key == nullptr)
        return nullptr;
    auto keyIt = m_DevicesByDispatchKey.find(key);
    if (keyIt == m_DevicesByDispatchKey.end())
        return nullptr;
    auto devIt = m_Devices.find(keyIt->second);
    return (devIt != m_Devices.end()) ? devIt->second : nullptr;
}

void VulkanLayerState::RegisterQueue(VkQueue queue, VkDevice device, uint32_t familyIndex) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_Queues[queue] = device;
    m_QueueFamilies[queue] = familyIndex;

    uint32_t queueFlags = 0;
    auto deviceIt = m_Devices.find(device);
    if (deviceIt != m_Devices.end() && deviceIt->second) {
        VkPhysicalDevice physicalDevice = deviceIt->second->physicalDevice;
        const auto owner = m_InstanceRegistry.ResolveByPhysicalDevice(physicalDevice);
        if (owner.instance != nullptr) {
            auto instanceIt = m_Instances.find(static_cast<VkInstance>(owner.instance));
            if (instanceIt != m_Instances.end() && instanceIt->second &&
                instanceIt->second->fp_vkGetPhysicalDeviceQueueFamilyProperties) {
                uint32_t queueFamilyCount = 0;
                instanceIt->second->fp_vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount,
                                                                                nullptr);
                if (familyIndex < queueFamilyCount) {
                    std::vector<VkQueueFamilyProperties> queueFamilies(queueFamilyCount);
                    instanceIt->second->fp_vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &queueFamilyCount,
                                                                                    queueFamilies.data());
                    queueFlags = queueFamilies[familyIndex].queueFlags;
                }
            }
        }
    }

    m_QueueFlags[queue] = queueFlags;
}

DeviceDispatch* VulkanLayerState::GetDeviceFromQueue(VkQueue queue) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Queues.find(queue);
    if (it != m_Queues.end()) {
        auto itDev = m_Devices.find(it->second);
        if (itDev != m_Devices.end())
            return itDev->second;
    }
    return ResolveDispatchByKey(queue);
}

VkDevice VulkanLayerState::GetVkDeviceFromQueue(VkQueue queue) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Queues.find(queue);
    if (it != m_Queues.end()) {
        return it->second;
    }
    return VK_NULL_HANDLE;
}

uint32_t VulkanLayerState::GetQueueFamilyIndex(VkQueue queue) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_QueueFamilies.find(queue);
    return (it != m_QueueFamilies.end()) ? it->second : VK_QUEUE_FAMILY_IGNORED;
}

uint32_t VulkanLayerState::GetQueueFlags(VkQueue queue) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_QueueFlags.find(queue);
    return (it != m_QueueFlags.end()) ? it->second : 0;
}

bool VulkanLayerState::QueueSupportsGraphics(VkQueue queue) {
    return (GetQueueFlags(queue) & VK_QUEUE_GRAPHICS_BIT) != 0;
}

bool VulkanLayerState::QueueSupportsCompute(VkQueue queue) {
    return (GetQueueFlags(queue) & VK_QUEUE_COMPUTE_BIT) != 0;
}

bool VulkanLayerState::QueueSupportsTransfer(VkQueue queue) {
    const uint32_t flags = GetQueueFlags(queue);
    return (flags & (VK_QUEUE_TRANSFER_BIT | VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT)) != 0;
}

VkQueue VulkanLayerState::FindGameGraphicsQueue(VkDevice device) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    DeviceDispatch* dispatch = nullptr;
    auto deviceIt = m_Devices.find(device);
    if (deviceIt != m_Devices.end()) {
        dispatch = deviceIt->second;
    }
    // Deterministic pick: lowest queue family index, then lowest handle, so the
    // borrow decision is stable across runs and across the maps' iteration
    // order. CE's own reserved queue is never a borrow candidate.
    VkQueue best = VK_NULL_HANDLE;
    uint32_t bestFamily = VK_QUEUE_FAMILY_IGNORED;
    // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order) - selection is order-independent by construction
    for (const auto& entry : m_Queues) {
        if (entry.second != device || entry.first == VK_NULL_HANDLE) {
            continue;
        }
        if (dispatch && entry.first == dispatch->overlayQueue) {
            continue;
        }
        auto flagsIt = m_QueueFlags.find(entry.first);
        if (flagsIt == m_QueueFlags.end() || (flagsIt->second & VK_QUEUE_GRAPHICS_BIT) == 0) {
            continue;
        }
        auto familyIt = m_QueueFamilies.find(entry.first);
        const uint32_t family = (familyIt != m_QueueFamilies.end()) ? familyIt->second : VK_QUEUE_FAMILY_IGNORED;
        if (best == VK_NULL_HANDLE || family < bestFamily ||
            (family == bestFamily && reinterpret_cast<uintptr_t>(entry.first) <
                                         reinterpret_cast<uintptr_t>(best))) {
            best = entry.first;
            bestFamily = family;
        }
    }
    return best;
}

VkQueue VulkanLayerState::FindLastGameGraphicsSubmitQueue(VkDevice device) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_DeviceLastGraphicsSubmitQueues.find(device);
    return (it != m_DeviceLastGraphicsSubmitQueues.end()) ? it->second : VK_NULL_HANDLE;
}

void VulkanLayerState::NoteQueueSubmit(VkQueue queue) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto queueIt = m_Queues.find(queue);
    if (queueIt == m_Queues.end()) {
        return;
    }
    m_DeviceLastSubmitThreadIds[queueIt->second] = GetCurrentThreadId();
    m_QueueLastSubmitThreadIds[queue] = GetCurrentThreadId();
    // Only the game reaches this: CE's own overlay, capture, screenshot and
    // prerender submissions call the dispatch pointer directly and never come
    // through the layer's vkQueueSubmit wrappers.
    auto flagsIt = m_QueueFlags.find(queue);
    if (flagsIt != m_QueueFlags.end() && (flagsIt->second & VK_QUEUE_GRAPHICS_BIT) != 0) {
        m_DeviceLastGraphicsSubmitQueues[queueIt->second] = queue;
    }
}

void VulkanLayerState::ArmPresentTopologyLearning() {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_SignalSemaphoreQueues.clear();
    m_SemaphoreGraphicsProducerQueues.clear();
    m_SemaphoreGraphicsProducerBoundaries.clear();
    m_PrerenderProducerBoundaryQueues.clear();
    m_QueueGraphicsProducerQueues.clear();
    m_QueueGraphicsProducerBoundaries.clear();
    m_LearnPresentTopology.store(true, std::memory_order_relaxed);
    m_LearnPrerenderBoundaries.store(true, std::memory_order_relaxed);
}

void VulkanLayerState::NoteSemaphoreDependencies(VkQueue queue, const VkSemaphore* waitSemaphores,
                                                 uint32_t waitCount, const VkSemaphore* signalSemaphores,
                                                 uint32_t signalCount) {
    if (!signalSemaphores || signalCount == 0)
        return;
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    if (!m_LearnPresentTopology.load(std::memory_order_relaxed) &&
        !m_LearnPrerenderBoundaries.load(std::memory_order_relaxed))
        return;

    VkQueue graphicsProducer = VK_NULL_HANDLE;
    const auto queueFlags = m_QueueFlags.find(queue);
    const bool queueSupportsGraphics =
        queueFlags != m_QueueFlags.end() && (queueFlags->second & VK_QUEUE_GRAPHICS_BIT) != 0;
    VkSemaphore graphicsProducerBoundary = VK_NULL_HANDLE;
    if (queueSupportsGraphics) {
        graphicsProducer = queue;
    } else if (waitSemaphores) {
        for (uint32_t i = 0; i < waitCount; ++i) {
            const auto upstream = m_SemaphoreGraphicsProducerQueues.find(waitSemaphores[i]);
            if (upstream != m_SemaphoreGraphicsProducerQueues.end()) {
                graphicsProducer = upstream->second;
                const auto boundary = m_SemaphoreGraphicsProducerBoundaries.find(waitSemaphores[i]);
                if (boundary != m_SemaphoreGraphicsProducerBoundaries.end())
                    graphicsProducerBoundary = boundary->second;
            }
        }
    }
    if (graphicsProducer == VK_NULL_HANDLE) {
        const auto prior = m_QueueGraphicsProducerQueues.find(queue);
        if (prior != m_QueueGraphicsProducerQueues.end())
            graphicsProducer = prior->second;
    } else {
        m_QueueGraphicsProducerQueues[queue] = graphicsProducer;
    }
    if (!queueSupportsGraphics && graphicsProducerBoundary == VK_NULL_HANDLE) {
        const auto priorBoundary = m_QueueGraphicsProducerBoundaries.find(queue);
        if (priorBoundary != m_QueueGraphicsProducerBoundaries.end())
            graphicsProducerBoundary = priorBoundary->second;
    } else if (graphicsProducerBoundary != VK_NULL_HANDLE) {
        m_QueueGraphicsProducerBoundaries[queue] = graphicsProducerBoundary;
    }

    for (uint32_t i = 0; i < signalCount; ++i) {
        const VkSemaphore signalSemaphore = signalSemaphores[i];
        if (signalSemaphore == VK_NULL_HANDLE)
            continue;
        m_SignalSemaphoreQueues[signalSemaphore] = queue;
        if (graphicsProducer != VK_NULL_HANDLE)
            m_SemaphoreGraphicsProducerQueues[signalSemaphore] = graphicsProducer;
        const VkSemaphore signalBoundary = queueSupportsGraphics ? signalSemaphore : graphicsProducerBoundary;
        if (signalBoundary != VK_NULL_HANDLE)
            m_SemaphoreGraphicsProducerBoundaries[signalSemaphore] = signalBoundary;
    }
}

void VulkanLayerState::NoteSemaphoreDependencies2(VkQueue queue, const VkSemaphoreSubmitInfo* waitSemaphores,
                                                  uint32_t waitCount,
                                                  const VkSemaphoreSubmitInfo* signalSemaphores,
                                                  uint32_t signalCount) {
    if (!signalSemaphores || signalCount == 0)
        return;
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    if (!m_LearnPresentTopology.load(std::memory_order_relaxed) &&
        !m_LearnPrerenderBoundaries.load(std::memory_order_relaxed))
        return;

    VkQueue graphicsProducer = VK_NULL_HANDLE;
    const auto queueFlags = m_QueueFlags.find(queue);
    const bool queueSupportsGraphics =
        queueFlags != m_QueueFlags.end() && (queueFlags->second & VK_QUEUE_GRAPHICS_BIT) != 0;
    VkSemaphore graphicsProducerBoundary = VK_NULL_HANDLE;
    if (queueSupportsGraphics) {
        graphicsProducer = queue;
    } else if (waitSemaphores) {
        for (uint32_t i = 0; i < waitCount; ++i) {
            const auto upstream = m_SemaphoreGraphicsProducerQueues.find(waitSemaphores[i].semaphore);
            if (upstream != m_SemaphoreGraphicsProducerQueues.end()) {
                graphicsProducer = upstream->second;
                const auto boundary = m_SemaphoreGraphicsProducerBoundaries.find(waitSemaphores[i].semaphore);
                if (boundary != m_SemaphoreGraphicsProducerBoundaries.end())
                    graphicsProducerBoundary = boundary->second;
            }
        }
    }
    if (graphicsProducer == VK_NULL_HANDLE) {
        const auto prior = m_QueueGraphicsProducerQueues.find(queue);
        if (prior != m_QueueGraphicsProducerQueues.end())
            graphicsProducer = prior->second;
    } else {
        m_QueueGraphicsProducerQueues[queue] = graphicsProducer;
    }
    if (!queueSupportsGraphics && graphicsProducerBoundary == VK_NULL_HANDLE) {
        const auto priorBoundary = m_QueueGraphicsProducerBoundaries.find(queue);
        if (priorBoundary != m_QueueGraphicsProducerBoundaries.end())
            graphicsProducerBoundary = priorBoundary->second;
    } else if (graphicsProducerBoundary != VK_NULL_HANDLE) {
        m_QueueGraphicsProducerBoundaries[queue] = graphicsProducerBoundary;
    }

    for (uint32_t i = 0; i < signalCount; ++i) {
        const VkSemaphore semaphore = signalSemaphores[i].semaphore;
        if (semaphore == VK_NULL_HANDLE)
            continue;
        m_SignalSemaphoreQueues[semaphore] = queue;
        if (graphicsProducer != VK_NULL_HANDLE)
            m_SemaphoreGraphicsProducerQueues[semaphore] = graphicsProducer;
        const VkSemaphore signalBoundary = queueSupportsGraphics ? semaphore : graphicsProducerBoundary;
        if (signalBoundary != VK_NULL_HANDLE)
            m_SemaphoreGraphicsProducerBoundaries[semaphore] = signalBoundary;
    }
}

VkQueue VulkanLayerState::GetSemaphoreSignalQueue(VkSemaphore semaphore) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_SignalSemaphoreQueues.find(semaphore);
    return (it != m_SignalSemaphoreQueues.end()) ? it->second : VK_NULL_HANDLE;
}

VkQueue VulkanLayerState::GetSemaphoreGraphicsProducerQueue(VkSemaphore semaphore) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_SemaphoreGraphicsProducerQueues.find(semaphore);
    return (it != m_SemaphoreGraphicsProducerQueues.end()) ? it->second : VK_NULL_HANDLE;
}

VkSemaphore VulkanLayerState::GetSemaphoreGraphicsProducerBoundary(VkSemaphore semaphore) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_SemaphoreGraphicsProducerBoundaries.find(semaphore);
    return (it != m_SemaphoreGraphicsProducerBoundaries.end()) ? it->second : VK_NULL_HANDLE;
}

void VulkanLayerState::RegisterPrerenderProducerBoundary(VkQueue queue, VkSemaphore semaphore) {
    if (queue == VK_NULL_HANDLE || semaphore == VK_NULL_HANDLE)
        return;
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_PrerenderProducerBoundaryQueues[semaphore] = queue;
}

bool VulkanLayerState::IsPrerenderProducerSubmit(VkQueue queue, uint32_t submitCount,
                                                 const VkSubmitInfo* pSubmits) {
    if (!pSubmits)
        return false;
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    for (uint32_t submit = 0; submit < submitCount; ++submit) {
        if (!pSubmits[submit].pSignalSemaphores)
            continue;
        for (uint32_t signal = 0; signal < pSubmits[submit].signalSemaphoreCount; ++signal) {
            const auto boundary = m_PrerenderProducerBoundaryQueues.find(pSubmits[submit].pSignalSemaphores[signal]);
            if (boundary != m_PrerenderProducerBoundaryQueues.end() && boundary->second == queue)
                return true;
        }
    }
    return false;
}

bool VulkanLayerState::IsPrerenderProducerSubmit2(VkQueue queue, uint32_t submitCount,
                                                  const VkSubmitInfo2* pSubmits) {
    if (!pSubmits)
        return false;
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    for (uint32_t submit = 0; submit < submitCount; ++submit) {
        if (!pSubmits[submit].pSignalSemaphoreInfos)
            continue;
        for (uint32_t signal = 0; signal < pSubmits[submit].signalSemaphoreInfoCount; ++signal) {
            const VkSemaphore semaphore = pSubmits[submit].pSignalSemaphoreInfos[signal].semaphore;
            const auto boundary = m_PrerenderProducerBoundaryQueues.find(semaphore);
            if (boundary != m_PrerenderProducerBoundaryQueues.end() && boundary->second == queue)
                return true;
        }
    }
    return false;
}

void VulkanLayerState::FinishPresentTopologyLearning() {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_LearnPresentTopology.store(false, std::memory_order_relaxed);
    if (!m_LearnPrerenderBoundaries.load(std::memory_order_relaxed)) {
        m_SignalSemaphoreQueues.clear();
        m_SemaphoreGraphicsProducerQueues.clear();
        m_SemaphoreGraphicsProducerBoundaries.clear();
        m_QueueGraphicsProducerQueues.clear();
        m_QueueGraphicsProducerBoundaries.clear();
    }
}

void VulkanLayerState::FinishPrerenderBoundaryLearning() {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_LearnPrerenderBoundaries.store(false, std::memory_order_relaxed);
    if (!m_LearnPresentTopology.load(std::memory_order_relaxed)) {
        m_SignalSemaphoreQueues.clear();
        m_SemaphoreGraphicsProducerQueues.clear();
        m_SemaphoreGraphicsProducerBoundaries.clear();
        m_QueueGraphicsProducerQueues.clear();
        m_QueueGraphicsProducerBoundaries.clear();
    }
}

void LearnPrerenderProducerTopology(SwapchainData* swapchainData, VkQueue presentQueue,
                                    const VkPresentInfoKHR* presentInfo) {
    auto& state = VulkanLayerState::Get();
    if (!swapchainData || !presentInfo || !presentInfo->pWaitSemaphores || presentInfo->waitSemaphoreCount == 0 ||
        !state.IsTrackingPresentDependencies()) {
        return;
    }

    VkSemaphore presentBoundary = VK_NULL_HANDLE;
    VkSemaphore graphicsBoundary = VK_NULL_HANDLE;
    VkQueue signalQueue = VK_NULL_HANDLE;
    VkQueue graphicsQueue = VK_NULL_HANDLE;
    for (uint32_t index = 0; index < presentInfo->waitSemaphoreCount; ++index) {
        const VkSemaphore candidate = presentInfo->pWaitSemaphores[index];
        const VkQueue candidateSignalQueue = state.GetSemaphoreSignalQueue(candidate);
        if (candidateSignalQueue == VK_NULL_HANDLE)
            continue;
        presentBoundary = candidate;
        signalQueue = candidateSignalQueue;
        graphicsQueue = state.GetSemaphoreGraphicsProducerQueue(candidate);
        graphicsBoundary = state.GetSemaphoreGraphicsProducerBoundary(candidate);
        if (graphicsQueue != VK_NULL_HANDLE && graphicsBoundary != VK_NULL_HANDLE)
            break;
    }
    if (signalQueue == VK_NULL_HANDLE)
        return;

    const bool learningPresentTopology = state.IsLearningPresentTopology();
    if (graphicsQueue != VK_NULL_HANDLE && state.GetVkDeviceFromQueue(graphicsQueue) == swapchainData->device &&
        state.QueueSupportsGraphics(graphicsQueue)) {
        swapchainData->prerenderProducerQueue.store(graphicsQueue, std::memory_order_release);
        const uint32_t producerThreadId = state.GetQueueLastSubmitThreadId(graphicsQueue);
        if (ce::vulkan_prerender_policy::ShouldPaceOnProducerSubmit(producerThreadId, GetCurrentThreadId()) &&
            graphicsBoundary != VK_NULL_HANDLE) {
            state.RegisterPrerenderProducerBoundary(graphicsQueue, graphicsBoundary);
            if (!swapchainData->prerenderOnProducerSubmit.exchange(true, std::memory_order_acq_rel)) {
                LayerLog(
                    "Vulkan Prerender: producer-submit pacing armed queue=%p boundary=%p producerThread=%u "
                    "presentThread=%u",
                    (void*)graphicsQueue, (void*)graphicsBoundary, producerThreadId, GetCurrentThreadId());
            }
        }
    }

    if (learningPresentTopology) {
        LayerLog(
            "Vulkan Layer: Present topology - present queue family=%u, boundary=%p signalled by queue %p "
            "(family=%u, graphics=%d), upstream graphics producer=%p (family=%u), producer boundary=%p, "
            "waitSemaphoreCount=%u",
            state.GetQueueFamilyIndex(presentQueue), (void*)presentBoundary, (void*)signalQueue,
            state.GetQueueFamilyIndex(signalQueue), state.QueueSupportsGraphics(signalQueue) ? 1 : 0,
            (void*)graphicsQueue, state.GetQueueFamilyIndex(graphicsQueue), (void*)graphicsBoundary,
            presentInfo->waitSemaphoreCount);
        state.FinishPresentTopologyLearning();
    }
    const uint32_t topologySamples =
        swapchainData->prerenderTopologySamples.fetch_add(1, std::memory_order_acq_rel) + 1;
    if (topologySamples >= std::clamp(swapchainData->imageCount, 1u, 8u))
        state.FinishPrerenderBoundaryLearning();
}

uint32_t VulkanLayerState::GetQueueLastSubmitThreadId(VkQueue queue) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_QueueLastSubmitThreadIds.find(queue);
    return (it != m_QueueLastSubmitThreadIds.end()) ? it->second : 0;
}

uint32_t VulkanLayerState::GetLastSubmitThreadId(VkDevice device) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_DeviceLastSubmitThreadIds.find(device);
    return (it != m_DeviceLastSubmitThreadIds.end()) ? it->second : 0;
}

void VulkanLayerState::RegisterSwapchain(VkSwapchainKHR swapchain, SwapchainData* data) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_Swapchains[swapchain] = data;
}

void VulkanLayerState::UnregisterSwapchain(VkSwapchainKHR swapchain) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Swapchains.find(swapchain);
    if (it != m_Swapchains.end()) {
        delete it->second;
        m_Swapchains.erase(it);
        // Increment generation to invalidate all TLS caches
        m_SwapchainGeneration.fetch_add(1, std::memory_order_release);
    }
}

SwapchainData* VulkanLayerState::GetSwapchainData(VkSwapchainKHR swapchain) {
    static thread_local VkSwapchainKHR tls_LastSwapchain = VK_NULL_HANDLE;
    static thread_local SwapchainData* tls_LastData = nullptr;
    static thread_local uint64_t tls_LastGeneration = 0;

    // Check if TLS cache is still valid (no swapchains unregistered since last lookup)
    uint64_t currentGen = m_SwapchainGeneration.load(std::memory_order_acquire);
    if (currentGen != tls_LastGeneration) {
        tls_LastSwapchain = VK_NULL_HANDLE;
        tls_LastData = nullptr;
        tls_LastGeneration = currentGen;
    }

    if (swapchain == tls_LastSwapchain && tls_LastData)
        return tls_LastData;

    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Swapchains.find(swapchain);
    if (it != m_Swapchains.end()) {
        tls_LastSwapchain = swapchain;
        tls_LastData = it->second;
        tls_LastGeneration = m_SwapchainGeneration.load(std::memory_order_acquire);
        return tls_LastData;
    }
    return nullptr;
}

void VulkanLayerState::RegisterSurface(VkSurfaceKHR surface, HWND window, VkInstance instance) {
    {
        std::lock_guard<std::recursive_mutex> lock(m_Lock);
        m_Surfaces[surface] = SurfaceRecord{window, instance};
    }
    // Publish the live Win32 surface HWND to the lock-free WSI bridge so the
    // hook DLL's scoped FIFO backstop can authorize swapchains on it. Multiple
    // surfaces on one window are refcounted by the bridge. The publish runs
    // outside m_Lock - the bridge owns its own spin section and never calls
    // back into this class, so bridge calls never nest under layer locks.
    ce::vulkan_wsi::PublishLiveSurfaceHwnd(window);
}

void VulkanLayerState::UnregisterSurface(VkSurfaceKHR surface) {
    HWND window = NULL;
    {
        std::lock_guard<std::recursive_mutex> lock(m_Lock);
        const auto it = m_Surfaces.find(surface);
        if (it == m_Surfaces.end())
            return;
        window = it->second.window;
        m_Surfaces.erase(surface);
    }
    // Retirement is refcounted: the window stays live while any other surface
    // still targets it.
    ce::vulkan_wsi::RetireLiveSurfaceHwnd(window);
}

HWND VulkanLayerState::GetSurfaceWindow(VkSurfaceKHR surface) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    auto it = m_Surfaces.find(surface);
    return (it != m_Surfaces.end()) ? it->second.window : NULL;
}

void VulkanLayerState::TrackPhysicalDevice(VkPhysicalDevice pd, VkInstance inst) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    m_InstanceRegistry.AddPhysicalDevice(pd, inst);
}

VkInstance VulkanLayerState::GetInstanceFromPhysicalDevice(VkPhysicalDevice pd) {
    return ResolveInstanceForPhysicalDevice(pd).instance;
}

// Ownership plus how it was established, so a caller can log that it fell back
// on the loader dispatch key instead of a handle CE itself enumerated.
VulkanLayerState::PhysicalDeviceOwner VulkanLayerState::ResolveInstanceForPhysicalDevice(VkPhysicalDevice pd) {
    std::lock_guard<std::recursive_mutex> lock(m_Lock);
    const auto lookup = m_InstanceRegistry.ResolveByPhysicalDevice(pd);
    PhysicalDeviceOwner owner;
    owner.instance = static_cast<VkInstance>(lookup.instance);
    owner.resolution = lookup.resolution;
    return owner;
}

void VulkanLayerState::UpdateFromSharedMemory(IPCClient* ipc) {
    if (!ipc || !ipc->GetSharedMem())
        return;

    auto& cfg = ipc->GetSharedMem()->graphicsConfig;

    // Parse anisotropic filtering
    if (strncmp(cfg.anisotropicFiltering, "default", 7) == 0 || cfg.anisotropicFiltering[0] == '\0') {
        m_AnisotropyOverrideActive = false;
        m_MaxAnisotropy = 16;
    } else {
        m_AnisotropyOverrideActive = true;
        if (strncmp(cfg.anisotropicFiltering, "16x", 3) == 0)
            m_MaxAnisotropy = 16;
        else if (strncmp(cfg.anisotropicFiltering, "8x", 2) == 0)
            m_MaxAnisotropy = 8;
        else if (strncmp(cfg.anisotropicFiltering, "4x", 2) == 0)
            m_MaxAnisotropy = 4;
        else if (strncmp(cfg.anisotropicFiltering, "2x", 2) == 0)
            m_MaxAnisotropy = 2;
        else if (strncmp(cfg.anisotropicFiltering, "off", 3) == 0)
            m_MaxAnisotropy = 1;
        else {
            m_AnisotropyOverrideActive = false;
            m_MaxAnisotropy = 16;
        }
    }

    // Parse mip bias
    if (strncmp(cfg.mipBias, "default", 7) != 0 && cfg.mipBias[0] != '\0') {
        float parsedBias = 0.0f;
        if (ce::TryParseFiniteFloat(cfg.mipBias, parsedBias)) {
            m_MipLodBias = parsedBias;
            m_MipBiasOverrideActive = true;
        } else {
            m_MipLodBias = 0.0f;
            m_MipBiasOverrideActive = false;
            static std::atomic<bool> loggedInvalidMipBias{false};
            if (!loggedInvalidMipBias.exchange(true, std::memory_order_relaxed))
                LayerLog("VulkanLayerState: rejected malformed mip bias '%s'", cfg.mipBias);
        }
    } else {
        m_MipLodBias = 0.0f;
        m_MipBiasOverrideActive = false;
    }

    // Parse mip bias mode
    if (cfg.mipBiasMode[0] != '\0')
        m_MipBiasMode = cfg.mipBiasMode;
    else
        m_MipBiasMode = "strict";

    m_ForceMipBiasClamp = cfg.forceMipBiasClamp;
    m_MipMapping = cfg.mipMapping[0] ? cfg.mipMapping : "default";
    m_SamplerOverrideMode = cfg.samplerOverrideMode[0] ? cfg.samplerOverrideMode : "safe";

    // VSync mode
    m_VsyncMode = cfg.vsyncMode;
    m_VblankPacedPresentation.store(
        ce::vulkan_present_metering_policy::RequestsVblankPacedPresentation(m_VsyncMode), std::memory_order_release);

    // Backbuffer count
    m_BackbufferCount = cfg.backbufferCount;

    // Prerender limit
    m_PrerenderLimit = cfg.prerenderLimit;

    LayerLog(
        "VulkanLayerState: Updated from config - policy=%s AF=%d, MipBias=%.1f, "
        "MipMap=%s, Clamp=%d, VSync=%s, BBCount=%d, Prerender=%.0f",
        m_SamplerOverrideMode.c_str(), m_MaxAnisotropy, m_MipLodBias, m_MipMapping.c_str(), m_ForceMipBiasClamp ? 1 : 0,
        m_VsyncMode.c_str(), m_BackbufferCount, m_PrerenderLimit);
}
