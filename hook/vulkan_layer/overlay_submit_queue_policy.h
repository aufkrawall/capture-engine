#pragma once

#include <stdint.h>

// Which queue the CE Vulkan overlay records and submits its graphics work on.
//
// The overlay is a render pass, so its command buffer needs a queue family with
// VK_QUEUE_GRAPHICS_BIT. Nothing says the queue a game presents from has one.
// DOOM Eternal (idTech 7) presents from queue family 2 - compute + transfer, no
// graphics - from five different threads once the real render loop starts, and
// CE simply gave up: session `20260819_030710` shows the overlay drawn for 239
// frames and then "Overlay skipped on non-graphics present queue family 2" for
// the rest of the run.
//
// Which graphics queue CE may use is the only question, and the answer must
// never be "the game's queue, unsynchronized" - VkQueue is externally
// synchronized, and a game that presents from several threads submits from
// several more.
//
// **The direct overlay is never parallel work.** It waits on exactly the
// semaphores the present was already going to wait on, and the rewritten
// present then waits on the overlay. It therefore sits on the present's
// critical path by construction, and a queue of its own cannot make it overlap
// with anything. That matters most on NVIDIA, where every VkQueue in the
// graphics family shares one hardware engine. DOOM Eternal's 2026-08-22 async
// present switch proves the remaining case: CE's CPU and overlay GPU costs stay
// flat, but a compute-produced present forces the direct path through compute
// -> graphics -> compute. The compute-present compositor below renders only
// the overlay texture independently and performs the final blend on the
// original compute/present queue, removing that round trip.
//
// For the direct path the preference order is "join the game's own graphics
// timeline", and CE's reserved queue is the fallback for the one case where
// joining it is unsafe:
//
//   1. The present queue itself, when it supports graphics. Unchanged behaviour
//      for every title that works today - and note this is already the game's
//      own queue, which is why that path never had a penalty.
//   2. A graphics queue borrowed from the game, with every submission to that
//      one queue - the game's and CE's - serialized through a lock CE holds
//      across the down-call. In-order behind the work it depends on: no
//      cross-queue semaphore, no engine context switch. This is also the only
//      option on hardware that exposes a single graphics queue (AMD).
//   3. A queue CE reserved for itself at vkCreateDevice by asking for one more
//      queue than the game did in a graphics family. Used when CE has evidence
//      the game acquires or submits from a thread other than the one it
//      presents on: CE is then inside the present while the game may already be
//      submitting the next frame, so appending the overlay to the game's queue
//      could land it behind a whole frame of work and delay this present by
//      that much. Paying two context switches beats paying a frame.

namespace ce::overlay_submit_queue_policy {

enum class OverlaySubmitQueue : uint32_t {
    // Nothing graphics-capable is reachable; the overlay cannot be drawn.
    kNone = 0,
    // The queue the game presented on.
    kPresentQueue = 1,
    // A queue CE reserved for itself at device creation.
    kReservedQueue = 2,
    // One of the game's graphics queues, under CE's submission lock.
    kBorrowedQueue = 3,
};

struct SubmitQueueAvailability {
    bool presentQueueSupportsGraphics = false;
    bool reservedQueueAvailable = false;
    bool borrowedQueueAvailable = false;
    // True when CE has evidence that the game acquires or submits from a thread
    // other than the one it presents on - the same evidence the layer already
    // uses to stand its limiters down. While that is true, the game can be
    // submitting the next frame's work concurrently with this present hook, so
    // CE's overlay is no longer guaranteed to be the next thing on the game's
    // queue.
    bool gameSubmitsConcurrently = false;
};

struct ComputePresentAvailability {
    bool presentQueueSupportsGraphics = false;
    bool presentQueueSupportsCompute = false;
    bool swapchainSupportsStorage = false;
    bool storageReadWithoutFormatAvailable = false;
    bool storageWriteWithoutFormatAvailable = false;
};

// A compute compositor is the only route that keeps a present-from-compute
// dependency on its original engine. It is used strictly when the application
// created storage-capable swapchain images and enabled the two formatless image
// features needed to preserve arbitrary WSI formats. Every other topology keeps
// the proven graphics-render-pass path.
inline bool ShouldUseComputePresent(const ComputePresentAvailability& availability) {
    return !availability.presentQueueSupportsGraphics && availability.presentQueueSupportsCompute &&
           availability.swapchainSupportsStorage && availability.storageReadWithoutFormatAvailable &&
           availability.storageWriteWithoutFormatAvailable;
}

inline OverlaySubmitQueue ChooseOverlaySubmitQueue(const SubmitQueueAvailability& availability) {
    if (availability.presentQueueSupportsGraphics) {
        return OverlaySubmitQueue::kPresentQueue;
    }
    if (availability.borrowedQueueAvailable && !availability.gameSubmitsConcurrently) {
        return OverlaySubmitQueue::kBorrowedQueue;
    }
    if (availability.reservedQueueAvailable) {
        return OverlaySubmitQueue::kReservedQueue;
    }
    if (availability.borrowedQueueAvailable) {
        return OverlaySubmitQueue::kBorrowedQueue;
    }
    return OverlaySubmitQueue::kNone;
}

// Offscreen overlay work has no dependency on the game's graphics timeline.
// Prefer CE's queue so an async game cannot enqueue its next frame in front of
// the overlay texture the current compute-present is about to sample.
inline OverlaySubmitQueue ChooseIndependentGraphicsQueue(bool reservedQueueAvailable, bool borrowedQueueAvailable) {
    if (reservedQueueAvailable)
        return OverlaySubmitQueue::kReservedQueue;
    if (borrowedQueueAvailable)
        return OverlaySubmitQueue::kBorrowedQueue;
    return OverlaySubmitQueue::kNone;
}

// CE may reserve a queue only from a graphics family that still has one free
// beyond what the game asked for. Asking for more than the family exposes fails
// device creation outright, which would take the game down with it.
inline bool CanReserveOverlayQueue(uint32_t familyQueueCount, uint32_t gameRequestedQueueCount) {
    return gameRequestedQueueCount < familyQueueCount;
}

// The reserved queue is the first index past the game's own, so the game can
// never receive CE's queue from vkGetDeviceQueue for an index it created.
inline uint32_t ReservedOverlayQueueIndex(uint32_t gameRequestedQueueCount) {
    return gameRequestedQueueCount;
}

// A protected-capable queue-create entry cannot simply be widened: protection is
// a property of every queue in the entry, and CE's overlay work is not
// protected content. The spec allows a second entry for the same family only
// when the protected flag differs, which is exactly the case here.
inline bool CanWidenQueueCreateEntry(bool entryIsProtected) {
    return !entryIsProtected;
}

// CE must never outrank the game's own queues. Requesting the highest priority
// the game already asked for keeps CE's queue inside the scheduling band the
// game chose for itself.
inline float ReservedOverlayQueuePriority(const float* gameRequestedPriorities, uint32_t priorityCount) {
    if (!gameRequestedPriorities || priorityCount == 0) {
        return 1.0f;
    }
    float highest = gameRequestedPriorities[0];
    for (uint32_t i = 1; i < priorityCount; ++i) {
        if (gameRequestedPriorities[i] > highest) {
            highest = gameRequestedPriorities[i];
        }
    }
    return highest;
}

// The submission lock exists only for the one borrowed queue. Every other queue
// in the process - including CE's reserved one - keeps the uncontended path.
inline bool ShouldSerializeSubmissionsOnQueue(bool borrowActive, bool queueIsBorrowedQueue) {
    return borrowActive && queueIsBorrowedQueue;
}

}  // namespace ce::overlay_submit_queue_policy
