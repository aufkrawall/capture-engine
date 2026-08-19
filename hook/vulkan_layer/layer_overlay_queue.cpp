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

// Resolve which graphics queue the overlay's render pass belongs on for a
// present that arrived on `presentQueue`.
OverlaySubmitTarget ResolveOverlaySubmitTarget(VkDevice device, DeviceDispatch* disp, VkQueue presentQueue,
                                               uint32_t presentQueueFamily) {
    OverlaySubmitTarget target;
    if (VulkanLayerState::Get().QueueSupportsGraphics(presentQueue)) {
        target.queue = presentQueue;
        target.queueFamilyIndex = presentQueueFamily;
        target.valid = true;
        return target;
    }

    const VkQueue reserved = disp ? disp->overlayQueue : VK_NULL_HANDLE;
    VkQueue borrowed = VK_NULL_HANDLE;
    if (reserved == VK_NULL_HANDLE) {
        borrowed = g_BorrowedOverlayQueue.load(std::memory_order_acquire);
        if (borrowed == VK_NULL_HANDLE) {
            borrowed = VulkanLayerState::Get().FindGameGraphicsQueue(device);
            if (borrowed != VK_NULL_HANDLE) {
                // Published before the first borrowed submit, so no game
                // submission can race the decision to serialize this queue.
                SetBorrowedOverlaySubmitQueue(borrowed);
            }
        }
    }

    switch (ce::overlay_submit_queue_policy::ChooseOverlaySubmitQueue(false, reserved != VK_NULL_HANDLE,
                                                                     borrowed != VK_NULL_HANDLE)) {
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

    static std::atomic<int> s_offPresentQueueLogCount{0};
    const int logNum = s_offPresentQueueLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (logNum <= 3 || (logNum % 10000) == 0) {
        LayerLog(
            "Vulkan Layer: Present queue family %u has no graphics support - submitting the overlay on the %s "
            "graphics queue %p (family %u, use #%d)",
            presentQueueFamily, target.borrowed ? "borrowed" : "reserved", (void*)target.queue,
            target.queueFamilyIndex, logNum);
    }
    return target;
}
