#pragma once

#include <cstdint>

// The GPU-completion rules behind the sharpen passes, with the device taken out
// so they can be tested.
//
// Every resource the passes defer - which command allocator or command buffer may
// be recorded into, when a replaced pipeline state may be released, whether the
// single source texture may be rebuilt - is answered from "has the GPU passed
// this point yet". Three bugs lived in exactly that answer, and none of them
// needs a GPU to demonstrate:
//
//  * The D3D12 pass is driven from two call sites that submit on different queues
//    (dx12_hook_process_session_draw_main.cpp on the game's queue,
//    dx12_hook_postsl_render_submit.cpp on whichever queue that frame's overlay
//    work went to, which can be Streamline's own). One fence signalled by two
//    queues produces completion values that are not ordered against each other,
//    so `completed >= slotValue` stopped meaning "the GPU is done with that
//    slot" - and a DLSS-G activation is enough to switch routes mid-swapchain.
//  * A replaced pipeline state was released immediately. D3D12 keeps no
//    reference for a submitted command list, so that is a use-after-free for as
//    long as the GPU is behind.
//  * The Vulkan pass left a slot marked in flight after a submit that never
//    happened, against a fence nothing would ever signal. That slot was never
//    reclaimed, and enough such failures retired the whole ring - sharpening
//    then stopped for the rest of the session.
namespace ce::sharpen {

// What GetCompletedValue() reports for a removed device. Nothing is executing on
// it, so every slot is free and every deferred release may happen.
inline constexpr uint64_t kCompletedValueDeviceRemoved = UINT64_MAX;

// A slot value that no completion can ever reach. Used for work that was queued
// but whose completion cannot be proven, which must never be recycled.
inline constexpr uint64_t kSlotValueUnprovable = UINT64_MAX;

// A slot that has never carried a submission.
inline constexpr uint64_t kSlotValueFree = 0;

// True when the GPU has passed `value`. A removed device has passed everything.
inline constexpr bool CompletedPast(uint64_t completed, uint64_t value) {
    if (completed == kCompletedValueDeviceRemoved)
        return true;
    if (value == kSlotValueFree)
        return true;
    if (value == kSlotValueUnprovable)
        return false;
    return completed >= value;
}

// True when the GPU has passed every submission made so far, which is the only
// point at which a single shader-visible descriptor may be rewritten: unlike a
// resource, a descriptor cannot be kept alive until the GPU catches up.
inline constexpr bool TimelineIsIdle(uint64_t completed, uint64_t lastSubmittedValue) {
    return lastSubmittedValue == 0 || CompletedPast(completed, lastSubmittedValue);
}

// Index of a slot the GPU has finished with, or -1 when every slot is still in
// flight. Never expresses a wait: a skipped frame is one unfiltered frame, while
// a wait here would be a stall inside the game's Present.
inline int SelectFreeSlot(const uint64_t* slotValues, uint32_t slotCount, uint64_t completed) {
    if (!slotValues)
        return -1;
    for (uint32_t slot = 0; slot < slotCount; ++slot) {
        if (CompletedPast(completed, slotValues[slot]))
            return static_cast<int>(slot);
    }
    return -1;
}

// How a submission attempt ended. The distinction that matters is not
// success/failure but whether anything was handed to the GPU at all.
enum class SubmissionOutcome : uint8_t {
    // Queued, and its completion will be signalled.
    kSubmitted,
    // Never queued (the submit call itself failed, or the fence could not be
    // reset first). Nothing will ever signal for it, so the slot is free again -
    // keeping it busy is what drains a ring one failure at a time.
    kNotQueued,
    // Queued, but the signal that would prove its completion failed. The GPU may
    // still be reading the slot and nothing will ever say otherwise, so it is
    // retired for good rather than recycled into work the GPU is still doing.
    kUnprovable,
};

// The slot's new value on a timeline-based ring (D3D12).
inline constexpr uint64_t SlotValueAfter(SubmissionOutcome outcome, uint64_t submittedValue) {
    switch (outcome) {
        case SubmissionOutcome::kSubmitted:
            return submittedValue;
        case SubmissionOutcome::kNotQueued:
            return kSlotValueFree;
        case SubmissionOutcome::kUnprovable:
        default:
            return kSlotValueUnprovable;
    }
}

// Whether the slot is still busy on a per-slot-fence ring (Vulkan), where the
// fence itself answers completion and the flag only says whether to ask it.
inline constexpr bool SlotIsBusyAfter(SubmissionOutcome outcome) {
    return outcome != SubmissionOutcome::kNotQueued;
}

// True when a change of submitting queue must be ordered before the timeline can
// be trusted again. Nothing to order when no submission has been made yet, or
// when the queue has not actually changed.
inline constexpr bool QueueChangeNeedsOrdering(const void* lastQueue, const void* queue,
                                               uint64_t lastSubmittedValue) {
    if (lastQueue == queue)
        return false;
    return lastQueue != nullptr && queue != nullptr && lastSubmittedValue > 0;
}

}  // namespace ce::sharpen
