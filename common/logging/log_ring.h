#pragma once

#include <atomic>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "common/logging/log_ring_wakeup.h"

namespace ce::logging {

enum class LogRingPush { Published, Full, TooLarge };

template <class Ring>
bool TryReserveLogSlot(Ring& ring, uint32_t& index) {
    static_assert(Ring::SLOT_COUNT > 0 && (Ring::SLOT_COUNT & (Ring::SLOT_COUNT - 1)) == 0,
                  "Power-of-two slots preserve indexing when the 32-bit cursor wraps");
    for (;;) {
        // Read the released consumer cursor before taking a fresh producer
        // snapshot. Reusing an old CAS value can put read ahead of write and
        // turn unsigned subtraction into a false full-ring verdict.
        const uint32_t read = ring.readIndex.load(std::memory_order_acquire);
        uint32_t write = ring.writeIndex.load(std::memory_order_relaxed);
        if (static_cast<uint32_t>(write - read) >= Ring::SLOT_COUNT)
            return false;
        if (ring.writeIndex.compare_exchange_weak(write, write + 1, std::memory_order_acq_rel,
                                                  std::memory_order_relaxed)) {
            index = write;
            return true;
        }
    }
}

template <class Ring>
LogRingPush TryPushLogRecord(Ring& ring, std::string_view filename, std::string_view message) {
    static_assert(Ring::SLOT_SIZE >= 4);
    if (filename.size() > Ring::SLOT_SIZE - 4 || message.size() > Ring::SLOT_SIZE - 4 - filename.size())
        return LogRingPush::TooLarge;
    uint32_t index = 0;
    if (!TryReserveLogSlot(ring, index)) {
        ring.overflowCount.fetch_add(1, std::memory_order_relaxed);
        return LogRingPush::Full;
    }
    const uint32_t slot = index % Ring::SLOT_COUNT;
    char* output = ring.buffer[slot];
    *output++ = '[';
    if (!filename.empty())
        std::memcpy(output, filename.data(), filename.size());
    output += filename.size();
    *output++ = ']';
    *output++ = ' ';
    if (!message.empty())
        std::memcpy(output, message.data(), message.size());
    output[message.size()] = '\0';
    ring.committed[slot].store(1, std::memory_order_release);
    // Signal every commit, including one behind a reserved head: only waking on empty->nonempty can strand
    // the consumer after it observes an uncommitted earlier slot. The event retains wakeups before the wait.
    NotifyLogConsumer();
    return LogRingPush::Published;
}

template <class Ring, size_t Size>
bool TryPopLogRecord(Ring& ring, uint32_t& read, char (&record)[Size]) {
    static_assert(Ring::SLOT_COUNT > 0 && (Ring::SLOT_COUNT & (Ring::SLOT_COUNT - 1)) == 0);
    static_assert(Size >= Ring::SLOT_SIZE);
    if (read == ring.writeIndex.load(std::memory_order_acquire))
        return false;
    const uint32_t slot = read % Ring::SLOT_COUNT;
    if (!ring.committed[slot].load(std::memory_order_acquire))
        return false;
    std::memcpy(record, ring.buffer[slot], Ring::SLOT_SIZE);
    record[Ring::SLOT_SIZE - 1] = '\0';
    ring.committed[slot].store(0, std::memory_order_release);
    ring.readIndex.store(++read, std::memory_order_release);
    return true;
}

}  // namespace ce::logging
