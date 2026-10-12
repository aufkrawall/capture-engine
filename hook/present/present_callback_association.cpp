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
    // CAS excludes colliding producers; atomic payloads also make reader retries lawful C++.
    std::atomic<uint64_t> sequence{0};
    std::atomic<int64_t> presentEntryUs{0};
    std::atomic<int64_t> callbackEndUs{0};
    std::atomic<bool> generated{false};
    std::atomic<uint64_t> generatorFrameToken{0};
    std::atomic<uint64_t> generation{0};
};

std::array<Slot, kCapacity> slots;
std::atomic<uint64_t> writeIndex{0};
std::atomic<uint64_t> generation{0};

// Staged per thread: only the thread that ran the callback may commit it, so a
// second presenter thread cannot steal another's pending association.
thread_local int64_t stagedCallbackEndUs = 0;
thread_local bool stagedGenerated = false;
thread_local uint64_t stagedGeneratorFrameToken = 0;
thread_local uint64_t stagedGeneration = 0;

// The committed callback's verdict for the Present now running on this thread.
thread_local bool verdictPending = false;
thread_local bool verdictGenerated = false;
thread_local uint64_t verdictGeneration = 0;

}  // namespace

void NoteCallbackEnd(int64_t callbackEndUs, bool generated, uint64_t generatorFrameToken) {
    stagedCallbackEndUs = callbackEndUs;
    stagedGenerated = generated;
    stagedGeneratorFrameToken = generatorFrameToken;
    stagedGeneration = generation.load(std::memory_order_acquire);
    // A new callback means the previous frame's Present is over, whether or
    // not anything consumed its verdict.
    verdictPending = false;
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
    verdictPending = true;
    verdictGenerated = stagedGenerated;
    verdictGeneration = stagedGeneration;
    Slot& slot = slots[index % kCapacity];
    uint64_t sequence = slot.sequence.load(std::memory_order_relaxed);
    if ((sequence & 1u) || !slot.sequence.compare_exchange_strong(sequence, sequence + 1,
            std::memory_order_acq_rel, std::memory_order_relaxed)) {
        stagedCallbackEndUs = 0;
        return;
    }
    std::atomic_thread_fence(std::memory_order_release);
    slot.presentEntryUs.store(presentEntryUs, std::memory_order_relaxed);
    slot.callbackEndUs.store(stagedCallbackEndUs, std::memory_order_relaxed);
    slot.generated.store(stagedGenerated, std::memory_order_relaxed);
    slot.generatorFrameToken.store(stagedGeneratorFrameToken, std::memory_order_relaxed);
    slot.generation.store(stagedGeneration, std::memory_order_relaxed);
    slot.sequence.store(sequence + 2, std::memory_order_release);
    stagedCallbackEndUs = 0;
    verdictPending = true;
    verdictGenerated = stagedGenerated;
    verdictGeneration = stagedGeneration;
}

PresentFrameVerdict PeekPresentFrameVerdict() {
    PresentFrameVerdict verdict;
    if (verdictPending && verdictGeneration == generation.load(std::memory_order_acquire)) {
        verdict.known = true;
        verdict.generated = verdictGenerated;
    }
    return verdict;
}

PresentFrameVerdict ConsumePresentFrameVerdict() {
    const PresentFrameVerdict verdict = PeekPresentFrameVerdict();
    verdictPending = false;
    return verdict;
}

bool Find(int64_t presentStartUs, Association& out) {
    // Entries are committed in Present order, so walking back from the newest
    // reaches the answer within a few slots and can stop as soon as the history
    // predates the tolerance window.
    const uint64_t currentGeneration = generation.load(std::memory_order_acquire);
    const uint64_t end = writeIndex.load(std::memory_order_acquire);
    const uint64_t first = end > kCapacity ? end - kCapacity : 0;
    int64_t bestDistance = kAssociationToleranceUs + 1;
    bool found = false;
    for (uint64_t index = end; index > first; --index) {
        Slot& slot = slots[(index - 1) % kCapacity];
        const uint64_t before = slot.sequence.load(std::memory_order_acquire);
        if (before == 0 || (before & 1u) != 0)
            continue;
        const Association candidate{slot.presentEntryUs.load(std::memory_order_relaxed),
            slot.callbackEndUs.load(std::memory_order_relaxed), slot.generated.load(std::memory_order_relaxed),
            slot.generatorFrameToken.load(std::memory_order_relaxed)};
        const uint64_t candidateGeneration = slot.generation.load(std::memory_order_relaxed);
        std::atomic_thread_fence(std::memory_order_acquire);
        if (slot.sequence.load(std::memory_order_acquire) != before || candidate.presentEntryUs <= 0 ||
            candidateGeneration != currentGeneration || generation.load(std::memory_order_acquire) != currentGeneration)
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
    // Generation invalidation never competes with slot writers. Old in-flight
    // publications carry the retired generation and readers reject them.

}

}  // namespace ce::present_association
