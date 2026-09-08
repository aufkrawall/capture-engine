#include "present_callback_association.h"

#include <algorithm>
#include <array>

namespace ce::present_association {
namespace {

// One second of history at any output rate this path reaches. Display pairs are
// consumed within a few frames of their Present, so the ring only has to outlive
// the display-timing service's flush interval.
constexpr size_t kCapacity = 256;

struct Slot {
    // Even sequence means readable, odd means a write is in progress. The
    // producer is single-threaded per swapchain, so this is a plain seqlock.
    std::atomic<uint64_t> sequence{0};
    int64_t presentEntryUs = 0;
    int64_t callbackEndUs = 0;
    bool generated = false;
};

std::array<Slot, kCapacity> slots;
std::atomic<uint64_t> writeIndex{0};
std::atomic<uint64_t> generation{0};

// Staged per thread: only the thread that ran the callback may commit it, so a
// second presenter thread cannot steal another's pending association.
thread_local int64_t stagedCallbackEndUs = 0;
thread_local bool stagedGenerated = false;
thread_local uint64_t stagedGeneration = 0;

}  // namespace

void NoteCallbackEnd(int64_t callbackEndUs, bool generated) {
    stagedCallbackEndUs = callbackEndUs;
    stagedGenerated = generated;
    stagedGeneration = generation.load(std::memory_order_acquire);
}

void NotePresentEntry(int64_t presentEntryUs) {
    if (stagedCallbackEndUs <= 0 || presentEntryUs < stagedCallbackEndUs)
        return;
    // A reset between the callback and its Present invalidates the pair rather
    // than publishing an association that spans a frame-generation transition.
    if (stagedGeneration != generation.load(std::memory_order_acquire)) {
        stagedCallbackEndUs = 0;
        return;
    }
    const uint64_t index = writeIndex.fetch_add(1, std::memory_order_relaxed);
    Slot& slot = slots[index % kCapacity];
    const uint64_t sequence = slot.sequence.load(std::memory_order_relaxed);
    slot.sequence.store(sequence + 1, std::memory_order_release);
    std::atomic_thread_fence(std::memory_order_release);
    slot.presentEntryUs = presentEntryUs;
    slot.callbackEndUs = stagedCallbackEndUs;
    slot.generated = stagedGenerated;
    slot.sequence.store(sequence + 2, std::memory_order_release);
    stagedCallbackEndUs = 0;
}

bool Find(int64_t presentStartUs, Association& out) {
    // Entries are committed in Present order, so walking back from the newest
    // reaches the answer within a few slots and can stop as soon as the history
    // predates the tolerance window.
    const uint64_t end = writeIndex.load(std::memory_order_acquire);
    const uint64_t first = end > kCapacity ? end - kCapacity : 0;
    int64_t bestDistance = kAssociationToleranceUs + 1;
    bool found = false;
    for (uint64_t index = end; index > first; --index) {
        Slot& slot = slots[(index - 1) % kCapacity];
        const uint64_t before = slot.sequence.load(std::memory_order_acquire);
        if (before == 0 || (before & 1u) != 0)
            continue;
        const Association candidate{slot.presentEntryUs, slot.callbackEndUs, slot.generated};
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.sequence.load(std::memory_order_acquire) != before || candidate.presentEntryUs <= 0)
            continue;
        if (candidate.presentEntryUs < presentStartUs - kAssociationToleranceUs)
            break;
        const int64_t distance = std::abs(candidate.presentEntryUs - presentStartUs);
        if (distance > kAssociationToleranceUs || distance >= bestDistance)
            continue;
        bestDistance = distance;
        out = candidate;
        found = true;
    }
    return found;
}

void Reset() {
    generation.fetch_add(1, std::memory_order_acq_rel);
    for (auto& slot : slots) {
        const uint64_t sequence = slot.sequence.load(std::memory_order_relaxed);
        if ((sequence & 1u) != 0)
            continue;
        slot.sequence.store(sequence + 1, std::memory_order_release);
        slot.presentEntryUs = 0;
        slot.callbackEndUs = 0;
        slot.generated = false;
        slot.sequence.store(sequence + 2, std::memory_order_release);
    }
}

}  // namespace ce::present_association
