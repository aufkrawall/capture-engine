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

struct ComputeCompositeBounds {
    int32_t x = 0;
    int32_t y = 0;
    uint32_t width = 0;
    uint32_t height = 0;
};

struct SubmissionSlotChoice {
    uint32_t index = 0;
    bool valid = false;
    bool waitForCompletion = false;
    // No slot could be reused and the ring may still be extended. The caller
    // creates one more slot instead of blocking the game's present thread.
    bool growRing = false;
};

// How deep the submission ring has to be, which is *not* the swapchain image
// count.
//
// An overlay submission waits on the present's own wait semaphores and retires
// only once they are signalled. A frame-generation runtime signals those from
// its pacer, one display interval apart, while it hands CE the whole generated
// group as a burst - so submissions arrive at burst cadence and retire at
// display cadence. A ring sized at the image count is then exhausted every
// group, and because the wait sits *before* the present down-call, CE's own
// resource recycling becomes the frame pacer.
//
// Portal RTX session `20260829_153457` is that failure, measured: with 6 images,
// 6 slots and 4x multi-frame generation, `perf_metrics_11660.csv` shows
// `fence_wait_us` blocking the game's present thread for 941 ms of every
// second - 7.5 ms on the first present of each group and 19-26 ms on the last -
// and `vulkan_layer.log` reports "All 6 overlay submission slots are in flight"
// hundreds of times. The presented rate stayed exactly at the 143 Hz refresh
// rate, but the *rendered* frame period beat between 2 and 6 vertical blanks in
// a repeating six-group pattern, which is the 6-slot ring aliasing against the
// 4-present group. Generated frames carry scene time, so an uneven base period
// is visible judder even when every vertical blank gets a new image.
//
// The extra depth is one full generated group beyond everything the runtime can
// already have outstanding: a rendered frame becomes at most four presented
// frames (DLSS multi-frame generation tops out at 4x), and those four are
// submitted together while retiring one display interval apart. That is a
// protocol maximum, not a tuned depth, and it is deliberately small: on the
// compute-composite path each slot also owns a full-resolution offscreen target.
inline constexpr uint32_t kGeneratedFrameBurstSlots = 4;

inline uint32_t ResolveSubmissionSlotCount(uint32_t imageCount) noexcept {
    if (imageCount == 0)
        return 0;
    if (imageCount > UINT32_MAX - kGeneratedFrameBurstSlots)
        return UINT32_MAX;
    return imageCount + kGeneratedFrameBurstSlots;
}

// **A fixed depth was still the wrong shape.** `20260829_153457` was answered by
// widening the ring from `imageCount` to `imageCount + 4`, and the follow-up
// sessions `20260830_175147` and `20260830_182939` show the same failure at the
// wider size: with 6 images and 10 slots, `fence_wait_us` has a median of 2.9 ms
// and a p99 of 15 ms, and `All 10 overlay submission slots are in flight` is
// logged past its 1024th occurrence inside twelve seconds - more than half of
// every present. Any fixed ring is a second rate limiter in the present path
// whose period (the slot count) aliases against the generated group size, and
// beating one against the other is exactly the uneven rendered-frame period
// that reads as judder even while every vertical blank receives a new image.
//
// So the depth is no longer predicted. The ring is extended by one slot
// whenever a present finds none reusable, which costs a command buffer, a fence
// and a binary semaphore, and it converges within a few frames on whatever the
// runtime actually keeps outstanding. `kMaxSubmissionSlots` is a memory-safety
// bound rather than a tuned depth: reaching it means something is wrong (a
// swapchain that stopped presenting, a lost device), and the old blocking
// behaviour is retained there so the failure is bounded rather than unbounded.
// Bounded by the overlay backend's per-frame vertex/index pool, whose index is
// free-running: more submissions in flight than that pool has entries and the
// CPU overwrites geometry the GPU is still reading. The two constants are
// asserted against each other where both headers are visible.
inline constexpr uint32_t kMaxSubmissionSlots = 32;

// **Slot reuse needs two facts, and the fence only proves one of them.** The
// fence proves CE's own submission retired, so its command buffer may be
// re-recorded. It says nothing about the *binary semaphore* that submission
// signalled: the present that waits on it is a queue operation that may still
// be pending long after the submission itself completed, and re-signalling a
// binary semaphore whose wait has not executed is undefined behaviour.
//
// Without `VK_EXT_swapchain_maintenance1`'s present fence, the only sound proof
// available is the swapchain itself: `vkAcquireNextImageKHR` returning image i
// means the presentation engine has finished with image i, which means every
// present of image i - including the one that waited on this slot's semaphore -
// has executed its wait. Comparing the image's acquire generation against the
// value recorded when the slot was used is therefore exact, and it stays exact
// when a generated-frame runtime presents one image several times without
// re-acquiring it: those presents all complete before the image comes back.
inline bool IsSubmissionSlotReusable(bool fenceRetired, bool everUsed, uint64_t acquireGenerationAtUse,
                                     uint64_t currentAcquireGeneration) noexcept {
    if (!fenceRetired)
        return false;
    if (!everUsed)
        return true;
    return currentAcquireGeneration != acquireGenerationAtUse;
}

// A generated-output runtime may present the same swapchain image several
// times in succession. Submission resources therefore cannot be selected by
// image identity: doing so immediately reuses that image's in-flight fence,
// command buffer and binary semaphore while every other slot sits idle.
// Prefer the next retired slot, scan the rest only when necessary, and apply
// backpressure to the oldest slot only when the entire ring is genuinely busy.
template <typename IsReady>
inline SubmissionSlotChoice ChooseSubmissionSlot(uint32_t slotCount, uint32_t nextSlot, IsReady&& isReady,
                                                 bool ringMayGrow = false) {
    if (slotCount == 0)
        return {};
    const uint32_t first = nextSlot % slotCount;
    for (uint32_t offset = 0; offset < slotCount; ++offset) {
        const uint32_t candidate = (first + offset) % slotCount;
        if (isReady(candidate))
            return {candidate, true, false, false};
    }
    if (ringMayGrow && slotCount < kMaxSubmissionSlots)
        return {slotCount, true, false, true};
    return {first, true, true, false};
}

inline uint32_t ComputeCompositeResourceIndex(uint32_t submissionSlot, uint32_t imageIndex,
                                              uint32_t imageCount) noexcept {
    return submissionSlot * imageCount + imageIndex;
}

// The final compute command buffer contains only per-image resources and this
// occupied rectangle. Once its fence has retired, Vulkan permits resubmitting
// the executable command buffer without recording it again. Dynamic overlay
// contents do not invalidate it: those pixels live in the independently
// rendered offscreen image.
inline bool CanReuseComputeCompositeCommand(bool commandRecorded, const ComputeCompositeBounds& recorded,
                                             const ComputeCompositeBounds& current) {
    return commandRecorded && recorded.x == current.x && recorded.y == current.y &&
           recorded.width == current.width && recorded.height == current.height;
}

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
