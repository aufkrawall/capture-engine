#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace ce {

// A best-effort history for diagnostics that may be read during exception
// handling. Neither readers nor writers wait for a slot held by another
// thread, or by the interrupted code on the same thread.
template <typename Record, size_t Capacity>
class BoundedDiagnosticHistory {
    static_assert(Capacity > 0 && std::is_trivially_copyable_v<Record>);

public:
    void Append(const Record& record) {
        const uint64_t sequence = newest_.fetch_add(1, std::memory_order_relaxed) + 1;
        Slot& slot = slots_[sequence % Capacity];
        if (slot.busy.test_and_set(std::memory_order_acquire))
            return;
        if (sequence > slot.sequence) {
            slot.record = record;
            slot.sequence = sequence;
        }
        slot.busy.clear(std::memory_order_release);
    }

    template <typename Visitor>
    void VisitRecent(Visitor&& visit) {
        const uint64_t end = newest_.load(std::memory_order_relaxed);
        const uint64_t first = end < Capacity ? 1 : end - Capacity + 1;
        for (uint64_t sequence = first; sequence <= end; ++sequence) {
            Slot& slot = slots_[sequence % Capacity];
            if (slot.busy.test_and_set(std::memory_order_acquire))
                continue;
            if (slot.sequence != sequence) {
                slot.busy.clear(std::memory_order_release);
                continue;
            }
            const Record record = slot.record;
            slot.busy.clear(std::memory_order_release);
            visit(sequence, record);
        }
    }

    // Tests reset only while the producers and readers are quiescent.
    void ResetForTesting() {
        newest_.store(0, std::memory_order_relaxed);
        for (Slot& slot : slots_)
            slot.sequence = 0;
    }

private:
    struct Slot {
        std::atomic_flag busy{};
        uint64_t sequence = 0;
        Record record{};
    };

    Slot slots_[Capacity];
    std::atomic<uint64_t> newest_{0};
};

}  // namespace ce
