#include "vulkan_layer_internal.h"
#include "vulkan_prerender_policy.h"

#include <algorithm>

// Present-topology and prerender-route learning: which queue produced the image a
// present waits on, and from which thread the configured queue-depth marker may
// be appended without breaking Vulkan's external queue synchronization.

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

void VulkanLayerState::ObservePrerenderPresentQueue(SwapchainData* swapchainData, VkQueue presentQueue) {
    if (!swapchainData || presentQueue == VK_NULL_HANDLE)
        return;
    if (swapchainData->prerenderPresentQueue.load(std::memory_order_acquire) == presentQueue)
        return;

    uint32_t learnedFamily = VK_QUEUE_FAMILY_IGNORED;
    uint32_t currentFamily = VK_QUEUE_FAMILY_IGNORED;
    {
        // A stable queue pays only the atomic check above. Keep the state lock
        // across a queue change and any route reset so another present cannot
        // observe the new queue before old producer evidence is retired.
        std::lock_guard<std::recursive_mutex> lock(m_Lock);
        if (swapchainData->prerenderPresentQueue.load(std::memory_order_relaxed) == presentQueue)
            return;
        const auto familyIt = m_QueueFamilies.find(presentQueue);
        currentFamily = familyIt != m_QueueFamilies.end() ? familyIt->second : VK_QUEUE_FAMILY_IGNORED;
        if (currentFamily == VK_QUEUE_FAMILY_IGNORED)
            return;

        learnedFamily = swapchainData->prerenderPresentQueueFamily.load(std::memory_order_acquire);
        if (learnedFamily == VK_QUEUE_FAMILY_IGNORED) {
            swapchainData->prerenderPresentQueueFamily.store(currentFamily, std::memory_order_release);
            swapchainData->prerenderPresentQueue.store(presentQueue, std::memory_order_release);
            return;
        }
        if (!ce::vulkan_prerender_policy::ShouldRelearnPresentTopology(learnedFamily, currentFamily)) {
            swapchainData->prerenderPresentQueue.store(presentQueue, std::memory_order_release);
            return;
        }

        swapchainData->prerenderProducerQueue.store(VK_NULL_HANDLE, std::memory_order_release);
        swapchainData->prerenderOnProducerSubmit.store(false, std::memory_order_release);
        swapchainData->prerenderTopologySamples.store(0, std::memory_order_release);
        ArmPresentTopologyLearning();
        swapchainData->prerenderPresentQueueFamily.store(currentFamily, std::memory_order_release);
        swapchainData->prerenderPresentQueue.store(presentQueue, std::memory_order_release);
    }

    LayerLog("Vulkan Layer: Present queue family changed %u -> %u without swapchain recreation; re-learning "
             "producer topology",
             learnedFamily, currentFamily);
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
