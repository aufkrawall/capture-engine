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
// Submitting elsewhere costs nothing: the overlay already signals a semaphore
// that the rewritten present waits on, and semaphores are queue-agnostic. The
// only question is which graphics queue CE may use, and the answer must never
// be "the game's queue, unsynchronized" - VkQueue is externally synchronized,
// and a game that presents from five threads submits from several more.
//
//   1. The present queue itself, when it supports graphics. Unchanged behaviour
//      for every title that works today.
//   2. A queue CE reserved for itself at vkCreateDevice by asking for one more
//      queue than the game did in a graphics family. CE owns it outright, so
//      the overlay submit runs concurrently with the game's own graphics work
//      instead of queueing behind it: no added latency, no serialization.
//   3. A graphics queue borrowed from the game, with every submission to that
//      one queue - the game's and CE's - serialized through a lock CE holds
//      across the down-call. AMD exposes exactly one graphics queue, so a game
//      there can leave no queue to reserve; borrowing is what keeps the overlay
//      alive on that hardware. Ordering is safe because CE's submit only ever
//      waits on the semaphores the present was already going to wait on, which
//      are signalled by work submitted before it.

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

inline OverlaySubmitQueue ChooseOverlaySubmitQueue(bool presentQueueSupportsGraphics, bool reservedQueueAvailable,
                                                   bool borrowedQueueAvailable) {
    if (presentQueueSupportsGraphics) {
        return OverlaySubmitQueue::kPresentQueue;
    }
    if (reservedQueueAvailable) {
        return OverlaySubmitQueue::kReservedQueue;
    }
    if (borrowedQueueAvailable) {
        return OverlaySubmitQueue::kBorrowedQueue;
    }
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
