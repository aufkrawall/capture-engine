/**
 * VK_LAYER_CE_overlay - overlay submission queue ownership
 *
 * Picks and owns the graphics queue the overlay's render pass is submitted on
 * when the game does not present from one. See overlay_submit_queue_policy.h
 * for the rules and for the DOOM Eternal session that motivated them.
 */

#include <mutex>
#include <vector>
#include "layer_main.h"
#include "overlay_submit_queue_policy.h"
#include "vulkan_layer.h"
#include "vulkan_loader_data.h"

namespace {

// The one game queue CE is allowed to submit the overlay on, when it could not
// reserve a queue of its own. Published before the first overlay submit and
// never changed afterwards, so an app submit can never race the decision.
std::atomic<VkQueue> g_BorrowedOverlayQueue{VK_NULL_HANDLE};

std::mutex& BorrowedQueueSubmissionLock() {
    static std::mutex s_lock;
    return s_lock;
}

}  // namespace

VkQueue GetBorrowedOverlaySubmitQueue() {
    return g_BorrowedOverlayQueue.load(std::memory_order_acquire);
}

void SetBorrowedOverlaySubmitQueue(VkQueue queue) {
    g_BorrowedOverlayQueue.store(queue, std::memory_order_release);
}

bool ShouldSerializeQueueSubmission(VkQueue queue) {
    const VkQueue borrowed = g_BorrowedOverlayQueue.load(std::memory_order_relaxed);
    return ce::overlay_submit_queue_policy::ShouldSerializeSubmissionsOnQueue(borrowed != VK_NULL_HANDLE,
                                                                             borrowed == queue);
}

void LockBorrowedQueueSubmission() {
    BorrowedQueueSubmissionLock().lock();
}

void UnlockBorrowedQueueSubmission() {
    BorrowedQueueSubmissionLock().unlock();
}

PFN_vkSetDeviceLoaderData FindDeviceLoaderDataCallback(const VkDeviceCreateInfo& createInfo) {
    for (const VkBaseInStructure* node = reinterpret_cast<const VkBaseInStructure*>(createInfo.pNext); node;
         node = node->pNext) {
        if (node->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO)
            continue;
        const auto* layerInfo = reinterpret_cast<const VkLayerDeviceCreateInfo*>(node);
        if (layerInfo->function == VK_LOADER_DATA_CALLBACK)
            return layerInfo->u.pfnSetDeviceLoaderData;
    }
    return nullptr;
}

// Build the queue-create list CE wants the device created with. Returns true
// when a queue was reserved; `reservation` then names where to fetch it from
// after vkCreateDevice succeeds and `queueCreateInfos` holds the rewritten list
// that must outlive the vkCreateDevice call.
bool BuildOverlayQueueReservation(InstanceDispatch* instanceDispatch, VkPhysicalDevice physicalDevice,
                                  const VkDeviceCreateInfo& createInfo,
                                  std::vector<VkDeviceQueueCreateInfo>& queueCreateInfos,
                                  std::vector<float>& widenedPriorities, OverlayQueueReservation& reservation) {
    reservation = OverlayQueueReservation{};
    if (!instanceDispatch || !instanceDispatch->fp_vkGetPhysicalDeviceQueueFamilyProperties ||
        physicalDevice == VK_NULL_HANDLE) {
        return false;
    }

    uint32_t familyCount = 0;
    instanceDispatch->fp_vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, nullptr);
    if (familyCount == 0) {
        return false;
    }
    std::vector<VkQueueFamilyProperties> families(familyCount);
    instanceDispatch->fp_vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &familyCount, families.data());

    queueCreateInfos.assign(createInfo.pQueueCreateInfos,
                            createInfo.pQueueCreateInfos + createInfo.queueCreateInfoCount);

    // Prefer widening a graphics family the game already uses: that keeps CE's
    // queue on the same hardware engine the game renders with, so the overlay
    // never crosses a queue-family ownership boundary on the swapchain image.
    size_t widenEntry = queueCreateInfos.size();
    for (size_t i = 0; i < queueCreateInfos.size(); ++i) {
        const VkDeviceQueueCreateInfo& entry = queueCreateInfos[i];
        if (entry.queueFamilyIndex >= familyCount ||
            (families[entry.queueFamilyIndex].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0) {
            continue;
        }
        if (!ce::overlay_submit_queue_policy::CanWidenQueueCreateEntry(
                (entry.flags & VK_DEVICE_QUEUE_CREATE_PROTECTED_BIT) != 0)) {
            continue;
        }
        if (!ce::overlay_submit_queue_policy::CanReserveOverlayQueue(families[entry.queueFamilyIndex].queueCount,
                                                                     entry.queueCount)) {
            continue;
        }
        widenEntry = i;
        break;
    }

    if (widenEntry < queueCreateInfos.size()) {
        VkDeviceQueueCreateInfo& entry = queueCreateInfos[widenEntry];
        widenedPriorities.assign(entry.pQueuePriorities, entry.pQueuePriorities + entry.queueCount);
        widenedPriorities.push_back(
            ce::overlay_submit_queue_policy::ReservedOverlayQueuePriority(entry.pQueuePriorities, entry.queueCount));
        reservation.queueFamilyIndex = entry.queueFamilyIndex;
        reservation.queueIndex = ce::overlay_submit_queue_policy::ReservedOverlayQueueIndex(entry.queueCount);
        entry.queueCount += 1;
        entry.pQueuePriorities = widenedPriorities.data();
        reservation.reserved = true;
        LayerLog(
            "Vulkan Layer: Reserved overlay submit queue family=%u index=%u by widening the game's own "
            "graphics queue request (game asked for %u)",
            reservation.queueFamilyIndex, reservation.queueIndex, reservation.queueIndex);
        return true;
    }

    // The game created no widenable graphics queue entry. A graphics family it
    // does not touch at all is still ours to ask for.
    for (uint32_t family = 0; family < familyCount; ++family) {
        if ((families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT) == 0 || families[family].queueCount == 0) {
            continue;
        }
        bool gameUsesFamily = false;
        for (const VkDeviceQueueCreateInfo& entry : queueCreateInfos) {
            if (entry.queueFamilyIndex == family) {
                gameUsesFamily = true;
                break;
            }
        }
        if (gameUsesFamily) {
            continue;
        }

        widenedPriorities.assign(1, ce::overlay_submit_queue_policy::ReservedOverlayQueuePriority(
                                        createInfo.queueCreateInfoCount > 0
                                            ? createInfo.pQueueCreateInfos[0].pQueuePriorities
                                            : nullptr,
                                        createInfo.queueCreateInfoCount > 0
                                            ? createInfo.pQueueCreateInfos[0].queueCount
                                            : 0));
        VkDeviceQueueCreateInfo added = {VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        added.queueFamilyIndex = family;
        added.queueCount = 1;
        added.pQueuePriorities = widenedPriorities.data();
        queueCreateInfos.push_back(added);
        reservation.queueFamilyIndex = family;
        reservation.queueIndex = 0;
        reservation.reserved = true;
        LayerLog("Vulkan Layer: Reserved overlay submit queue family=%u index=0 from a graphics family the game "
                 "does not use",
                 family);
        return true;
    }

    LayerLog(
        "Vulkan Layer: No spare graphics queue to reserve for the overlay; a game that presents from a "
        "non-graphics queue will fall back to borrowing one of its own graphics queues under CE's submission "
        "lock");
    return false;
}

void InitializeReservedOverlayQueue(VkDevice device, DeviceDispatch* dispatch,
                                    const OverlayQueueReservation& reservation,
                                    PFN_vkSetDeviceLoaderData setDeviceLoaderData) {
    if (!reservation.reserved)
        return;
    if (!dispatch || !dispatch->fp_vkGetDeviceQueue) {
        LayerLog(
            "Vulkan Layer: [Warning] Reserved overlay queue family=%u index=%u cannot be fetched because "
            "vkGetDeviceQueue is unavailable",
            reservation.queueFamilyIndex, reservation.queueIndex);
        return;
    }

    VkQueue queue = VK_NULL_HANDLE;
    dispatch->fp_vkGetDeviceQueue(device, reservation.queueFamilyIndex, reservation.queueIndex, &queue);
    if (queue == VK_NULL_HANDLE) {
        LayerLog("Vulkan Layer: [Warning] Reserved overlay queue family=%u index=%u could not be fetched",
                 reservation.queueFamilyIndex, reservation.queueIndex);
        return;
    }

    const ce::vulkan_loader_data::InitializationResult loaderData =
        ce::vulkan_loader_data::InitializeDeviceObject(setDeviceLoaderData, device, queue);
    if (!loaderData.initialized()) {
        LayerLog(
            "Vulkan Layer: [Warning] Reserved overlay queue family=%u index=%u loader data initialization "
            "failed (outcome=%s callbackResult=%d); disabling the reserved queue",
            reservation.queueFamilyIndex, reservation.queueIndex,
            ce::vulkan_loader_data::ToString(loaderData.outcome), static_cast<int>(loaderData.callbackResult));
        return;
    }

    dispatch->overlayQueue = queue;
    dispatch->overlayQueueFamilyIndex = reservation.queueFamilyIndex;
    VulkanLayerState::Get().RegisterQueue(queue, device, reservation.queueFamilyIndex);
    LayerLog("Vulkan Layer: Overlay submit queue ready (queue=%p family=%u index=%u loaderData=%s)",
             (void*)queue, reservation.queueFamilyIndex, reservation.queueIndex,
             ce::vulkan_loader_data::ToString(loaderData.outcome));
}

// Resolve which graphics queue the overlay's render pass belongs on for a
// present that arrived on `presentQueue`. `gameSubmitsConcurrently` is the
// layer's async-present evidence: while it is false the game submits and
// presents on one thread, so nothing of the game's can slip onto its graphics
// queue between the frame CE is overlaying and CE's own submit.
OverlaySubmitTarget ResolveOverlaySubmitTarget(VkDevice device, DeviceDispatch* disp, VkQueue presentQueue,
                                               uint32_t presentQueueFamily, bool gameSubmitsConcurrently,
                                               bool independentOffscreenWork) {
    OverlaySubmitTarget target;
    if (VulkanLayerState::Get().QueueSupportsGraphics(presentQueue)) {
        target.queue = presentQueue;
        target.queueFamilyIndex = presentQueueFamily;
        target.valid = true;
        return target;
    }

    const VkQueue reserved = disp ? disp->overlayQueue : VK_NULL_HANDLE;
    if (independentOffscreenWork && reserved != VK_NULL_HANDLE) {
        target.queue = reserved;
        target.queueFamilyIndex = disp->overlayQueueFamilyIndex;
        target.valid = target.queueFamilyIndex != VK_QUEUE_FAMILY_IGNORED;
        return target;
    }
    // On the direct render-pass path the borrow candidate is resolved even when
    // a reserved queue exists, and published even on a call that then picks the
    // reserved queue. The tier is evidence-driven and a swapchain recreate
    // re-arms that evidence, so a process can move onto the borrowed queue at
    // any later present. Independent offscreen work returned above: it owns the
    // reserved queue and must not put a mutex on the game's graphics submits.
    VkQueue borrowed = g_BorrowedOverlayQueue.load(std::memory_order_acquire);
    if (borrowed == VK_NULL_HANDLE) {
        // Prefer the exact queue the game last submitted graphics work on -
        // that is the queue that produced the image the overlay draws over, so
        // appending to it is in-order rather than another cross-queue wait.
        borrowed = VulkanLayerState::Get().FindLastGameGraphicsSubmitQueue(device);
        if (borrowed == VK_NULL_HANDLE) {
            borrowed = VulkanLayerState::Get().FindGameGraphicsQueue(device);
        }
        if (borrowed != VK_NULL_HANDLE) {
            // Published before the first borrowed submit, so no game submission
            // can race the decision to serialize this queue. The choice is made
            // once and never revised, so the lock's identity stays stable for
            // the lifetime of the device.
            SetBorrowedOverlaySubmitQueue(borrowed);
        }
    }

    ce::overlay_submit_queue_policy::SubmitQueueAvailability availability;
    availability.presentQueueSupportsGraphics = false;
    availability.reservedQueueAvailable = reserved != VK_NULL_HANDLE;
    availability.borrowedQueueAvailable = borrowed != VK_NULL_HANDLE;
    availability.gameSubmitsConcurrently = gameSubmitsConcurrently;

    const ce::overlay_submit_queue_policy::OverlaySubmitQueue selected =
        independentOffscreenWork
            ? ce::overlay_submit_queue_policy::ChooseIndependentGraphicsQueue(reserved != VK_NULL_HANDLE,
                                                                               borrowed != VK_NULL_HANDLE)
            : ce::overlay_submit_queue_policy::ChooseOverlaySubmitQueue(availability);
    switch (selected) {
        case ce::overlay_submit_queue_policy::OverlaySubmitQueue::kReservedQueue:
            target.queue = reserved;
            target.queueFamilyIndex = disp->overlayQueueFamilyIndex;
            break;
        case ce::overlay_submit_queue_policy::OverlaySubmitQueue::kBorrowedQueue:
            target.queue = borrowed;
            target.queueFamilyIndex = VulkanLayerState::Get().GetQueueFamilyIndex(borrowed);
            target.borrowed = true;
            break;
        default: {
            static std::atomic<int> s_noGraphicsQueueLogCount{0};
            if (s_noGraphicsQueueLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
                LayerLog(
                    "Vulkan Layer: Overlay skipped on non-graphics present queue family %u - no reserved or "
                    "borrowable graphics queue on this device",
                    presentQueueFamily);
            }
            return target;
        }
    }

    if (target.queueFamilyIndex == VK_QUEUE_FAMILY_IGNORED) {
        return target;
    }
    target.valid = true;

    // The tier is the whole performance story on a compute-only present family,
    // so it has to be readable from the log without a profiler: "borrowed" is
    // the in-order path, "reserved" costs two engine context switches per frame
    // and is only chosen because the game presents asynchronously.
    static std::atomic<int> s_offPresentQueueLogCount{0};
    const int logNum = s_offPresentQueueLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (logNum <= 3 || (logNum % 10000) == 0) {
        LayerLog(
            "Vulkan Layer: Present queue family %u has no graphics support - submitting the overlay on the %s "
            "graphics queue %p (family %u, gameSubmitsConcurrently=%d, use #%d)",
            presentQueueFamily, target.borrowed ? "borrowed" : "reserved", (void*)target.queue,
            target.queueFamilyIndex, gameSubmitsConcurrently ? 1 : 0, logNum);
    }
    return target;
}

// A borrowed queue belongs to one device. Nothing else clears the publication,
// so a device teardown has to, or the next device inherits a dangling VkQueue
// as the handle CE serializes its submissions against.
void ForgetBorrowedOverlaySubmitQueue(VkDevice device) {
    const VkQueue borrowed = g_BorrowedOverlayQueue.load(std::memory_order_acquire);
    if (borrowed == VK_NULL_HANDLE)
        return;
    if (VulkanLayerState::Get().GetVkDeviceFromQueue(borrowed) != device)
        return;
    std::lock_guard<std::mutex> lock(BorrowedQueueSubmissionLock());
    g_BorrowedOverlayQueue.store(VK_NULL_HANDLE, std::memory_order_release);
    LayerLog("Vulkan Layer: Released the borrowed overlay submit queue with its device %p", (void*)device);
}
