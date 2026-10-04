#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <type_traits>

// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall

namespace ce::log_meter {

// Metering helper for diagnostic logging on hot or frequently repeated paths.
//
// `callIndex` is 1-based; callers keep a static counter (std::atomic<int> on
// multi-threaded paths) and pass the next value. Returns true for the first
// `firstBurstCount` calls and then for every `stride`-th call (callIndex ==
// stride, 2*stride, ...). A stride of 0 logs every call.
//
// This keeps the "first N entries plus a periodic heartbeat" policy used across
// the codebase in one testable place instead of a new magic-number condition at
// every call site.
inline bool ShouldLogCadence(uint32_t callIndex, uint32_t firstBurstCount, uint32_t stride) {
    if (stride == 0) {
        return true;
    }
    return callIndex <= firstBurstCount || (callIndex % stride) == 0;
}

// The fields of a log line that make it a different line, folded into one key (FNV-1a over each
// field's bytes). Leave counters, sequence numbers and rotating back-buffer pointers out: a key
// should change exactly when a reader would say the state changed.
template <typename Field>
inline void MixFieldBytes(uint64_t& hash, const Field& field) noexcept {
    static_assert(std::is_trivially_copyable_v<Field>, "FieldKey hashes raw field bytes");
    unsigned char bytes[sizeof(Field)];
    std::memcpy(bytes, static_cast<const void*>(std::addressof(field)), sizeof(Field));
    for (unsigned char byte : bytes) {
        hash ^= byte;
        hash *= 1099511628211ull;
    }
}

template <typename... Fields>
inline uint64_t FieldKey(const Fields&... fields) noexcept {
    uint64_t hash = 14695981039346656037ull;
    (MixFieldBytes(hash, fields), ...);
    return hash == UINT64_MAX ? hash - 1 : hash;  // UINT64_MAX is ChangeGate's "nothing seen yet"
}

// Gate for a line that repeats with the same content: it is logged when its key changes, and
// optionally once per heartbeat while it stays the same. Each logged line learns how many identical
// repeats were swallowed since the previous one, so suppression loses no information. Lock-free;
// racing callers can at worst log one extra line.
class ChangeGate {
public:
    struct Verdict {
        bool log = false;
        uint64_t suppressed = 0;  // identical repeats swallowed since the previous logged line
        explicit operator bool() const { return log; }
    };

    constexpr ChangeGate() = default;
    explicit constexpr ChangeGate(uint64_t heartbeatMs) : heartbeatMs_(heartbeatMs) {}

    // `nowMs` only matters with a heartbeat; any monotonic millisecond clock works.
    Verdict Observe(uint64_t key, uint64_t nowMs = 0) noexcept { return Decide(key, nowMs, false); }

    // Always logs (for a line the caller must emit anyway, e.g. the end of a diagnostic window)
    // and still reports the repeats swallowed since the previous logged line.
    Verdict Force(uint64_t key, uint64_t nowMs = 0) noexcept { return Decide(key, nowMs, true); }

    // The per-frame form: logs on the first call, on every key change and on every `stride`-th call
    // (`callIndex` 1-based, as for ShouldLogCadence), which keeps a heartbeat in long steady runs.
    Verdict ObserveOrEvery(uint64_t key, uint32_t callIndex, uint32_t stride) noexcept {
        return Decide(key, 0, stride != 0 && callIndex % stride == 0);
    }

    // Forgets the last key and the swallowed repeats (a StreamChangeGate slot changing owner).
    void Reset() noexcept {
        lastKey_.store(UINT64_MAX, std::memory_order_release);
        suppressed_.store(0, std::memory_order_release);
    }

private:
    Verdict Decide(uint64_t key, uint64_t nowMs, bool force) noexcept {
        const uint64_t previous = lastKey_.exchange(key, std::memory_order_acq_rel);
        const bool heartbeat =
            heartbeatMs_ != 0 && nowMs - lastLogMs_.load(std::memory_order_relaxed) >= heartbeatMs_;
        if (previous == key && !heartbeat && !force) {
            suppressed_.fetch_add(1, std::memory_order_relaxed);
            return {};
        }
        lastLogMs_.store(nowMs, std::memory_order_relaxed);
        return {true, suppressed_.exchange(0, std::memory_order_acq_rel)};
    }

    std::atomic<uint64_t> lastKey_{UINT64_MAX};
    std::atomic<uint64_t> lastLogMs_{0};
    std::atomic<uint64_t> suppressed_{0};
    uint64_t heartbeatMs_ = 0;
};

// " (+N unchanged)" when a ChangeGate swallowed repeats, "" otherwise; for a trailing "%s".
struct SuppressedNote {
    char text[40] = {};
    explicit SuppressedNote(uint64_t suppressed) noexcept {
        if (suppressed != 0)
            std::snprintf(text, sizeof(text), " (+%llu unchanged)", static_cast<unsigned long long>(suppressed));
    }
    const char* c_str() const noexcept { return text; }
};

// One ChangeGate per stream, for a line that interleaved sources share (several queues, threads or API
// entry points): a single gate keyed by source and state together sees a change at every alternation
// and logs them all. `stream` picks the gate, `key` is the state as for ChangeGate. Streams hash onto
// `Capacity` slots; a stream that finds its slot owned by another takes it over and logs its next line
// (the evicted stream's swallowed-repeat count is dropped), so a collision logs too much, never too little.
template <size_t Capacity>
class StreamChangeGate {
public:
    ChangeGate::Verdict Observe(uint64_t stream, uint64_t key, uint64_t nowMs = 0) noexcept {
        return SlotFor(stream).Observe(key, nowMs);
    }

    ChangeGate::Verdict ObserveOrEvery(uint64_t stream, uint64_t key, uint32_t callIndex, uint32_t stride) noexcept {
        return SlotFor(stream).ObserveOrEvery(key, callIndex, stride);
    }

private:
    struct Slot {
        std::atomic<uint64_t> owner{UINT64_MAX};  // unowned; FieldKey never yields UINT64_MAX
        ChangeGate gate;
    };

    ChangeGate& SlotFor(uint64_t stream) noexcept {
        const size_t idx0 = ((stream * 0x9E3779B97F4A7C15ull) >> 32) % Capacity;
        Slot& s0 = slots_[idx0];
        if (s0.owner.load(std::memory_order_acquire) == stream) {
            return s0.gate;
        }
        if constexpr (Capacity > 1) {
            const size_t idx1 = (idx0 + 1) % Capacity;
            Slot& s1 = slots_[idx1];
            if (s1.owner.load(std::memory_order_acquire) == stream) {
                return s1.gate;
            }
            uint64_t expected = UINT64_MAX;
            if (s0.owner.compare_exchange_strong(expected, stream, std::memory_order_acq_rel)) {
                s0.gate.Reset();
                return s0.gate;
            }
            expected = UINT64_MAX;
            if (s1.owner.compare_exchange_strong(expected, stream, std::memory_order_acq_rel)) {
                s1.gate.Reset();
                return s1.gate;
            }
        }
        if (s0.owner.exchange(stream, std::memory_order_acq_rel) != stream) {
            s0.gate.Reset();
        }
        return s0.gate;
    }

    Slot slots_[Capacity];
};

// Remembers the first `Capacity` distinct keys, for "log once per module/handle/target" lines.
// When full it reports every further unseen key as new: it may log too much, never too little.
template <size_t Capacity>
class KeyedOnce {
public:
    bool FirstTime(uint64_t key) noexcept {
        const uint64_t stored = key == 0 ? 1 : key;  // 0 marks an empty slot
        for (auto& slot : slots_) {
            uint64_t current = slot.load(std::memory_order_acquire);
            if (current == stored)
                return false;
            if (current == 0 && slot.compare_exchange_strong(current, stored, std::memory_order_acq_rel))
                return true;
            if (current == stored)
                return false;
        }
        return true;
    }

private:
    std::atomic<uint64_t> slots_[Capacity] = {};
};

}  // namespace ce::log_meter
