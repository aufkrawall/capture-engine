#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <unordered_map>

// NVIDIA's display driver announces, before it programs a flip, the QPC that
// flip is scheduled to reach the screen. That announcement is what separates a
// present series from a display series on this hardware: under frame generation
// one render produces several paced flips, the driver programs them within a
// fraction of a millisecond of each other, and each is then held until its own
// scheduled time. Measured on a 2x DLSS-G capture of `dx12_dlss_fg_test`,
// publishing the flip event timestamp gives alternating 0.36 ms / 14.6 ms
// intervals - the sawtooth - while the announced times give 7.21 ms +/- 0.08 ms
// over the same 2768 flips.
//
// The provider carries no registered manifest on this machine (nothing under
// WINEVT\Publishers, and TdhGetEventInformation answers ERROR_NOT_FOUND), so
// property names cannot be resolved and the payload has to be read
// positionally. Rather than hardcode an offset a driver update may move, the
// decoder locates the announcement by what it *is*: the only payload slot
// holding a QPC near the timestamp of the event carrying it. Every read is
// validated against the same rule, and discovery re-arms if the located slot
// stops answering, so an unrecognised payload yields no correction rather than
// a wrong one.

class NvidiaFlipAnnouncementDecoder {
public:
    // A payload slot must look like an announcement in nearly every sample of a
    // discovery window before it is trusted. Counters, handles and misaligned
    // reads across field boundaries all fall outside the window by many orders
    // of magnitude, so the surviving slot is unambiguous.
    static constexpr std::size_t kDiscoverySamples = 64;
    static constexpr std::size_t kDiscoveryQuorum = 58;
    static constexpr std::size_t kMaxDiscoveryAttempts = 8;
    static constexpr std::size_t kMaxConsecutiveRejects = 64;
    static constexpr std::size_t kMaxPayloadSlots = 64;
    // Frame generation schedules within a frame interval; the bound is loose on
    // purpose because its job is to exclude non-timestamp fields, not to model
    // a pacing policy.
    static constexpr int64_t kMaxAheadUs = 200'000;
    static constexpr int64_t kMaxBehindUs = 1'000;

    void SetQpcFrequency(int64_t frequency) { qpcFrequency_ = frequency; }

    // Returns the announced screen time, or 0 when this payload carries none.
    int64_t Decode(const void* payload, std::size_t payloadBytes, int64_t eventQpc) {
        if (!payload || qpcFrequency_ <= 0 || payloadBytes < sizeof(uint64_t))
            return 0;
        if (located_)
            return DecodeLocated(payload, payloadBytes, eventQpc);
        if (attempts_ >= kMaxDiscoveryAttempts)
            return 0;
        return Discover(payload, payloadBytes, eventQpc);
    }

    void Reset() {
        located_ = false;
        offset_ = 0;
        attempts_ = 0;
        rejected_ = 0;
        consecutiveRejects_ = 0;
        ClearDiscoveryWindow();
    }

    bool located() const noexcept { return located_; }
    bool abandoned() const noexcept { return !located_ && attempts_ >= kMaxDiscoveryAttempts; }
    std::size_t offset() const noexcept { return offset_; }
    uint64_t rejected() const noexcept { return rejected_; }

private:
    static int64_t ReadAt(const void* payload, std::size_t payloadBytes, std::size_t offset) {
        if (offset + sizeof(uint64_t) > payloadBytes)
            return 0;
        uint64_t value = 0;
        std::memcpy(&value, static_cast<const unsigned char*>(payload) + offset, sizeof(value));
        return value > static_cast<uint64_t>(INT64_MAX) ? 0 : static_cast<int64_t>(value);
    }

    bool Plausible(int64_t candidate, int64_t eventQpc) const {
        if (candidate <= 0)
            return false;
        const int64_t delta = candidate - eventQpc;
        return delta <= (kMaxAheadUs * qpcFrequency_) / 1'000'000 &&
               delta >= -(kMaxBehindUs * qpcFrequency_) / 1'000'000;
    }

    int64_t DecodeLocated(const void* payload, std::size_t payloadBytes, int64_t eventQpc) {
        const int64_t candidate = ReadAt(payload, payloadBytes, offset_);
        if (Plausible(candidate, eventQpc)) {
            consecutiveRejects_ = 0;
            return candidate;
        }
        ++rejected_;
        // A payload that stops answering at the located slot means the layout
        // moved, not that this one frame is odd; look for it again.
        if (++consecutiveRejects_ >= kMaxConsecutiveRejects) {
            located_ = false;
            consecutiveRejects_ = 0;
            ClearDiscoveryWindow();
        }
        return 0;
    }

    int64_t Discover(const void* payload, std::size_t payloadBytes, int64_t eventQpc) {
        const std::size_t slots = std::min(kMaxPayloadSlots, (payloadBytes - sizeof(uint64_t)) / sizeof(uint32_t) + 1);
        std::size_t plausibleSlots = 0;
        int64_t onlyCandidate = 0;
        for (std::size_t slot = 0; slot < slots; ++slot) {
            const int64_t candidate = ReadAt(payload, payloadBytes, slot * sizeof(uint32_t));
            if (Plausible(candidate, eventQpc)) {
                ++hits_[slot];
                ++plausibleSlots;
                onlyCandidate = candidate;
            }
        }
        // A payload in which exactly one slot looks like an announcement is
        // already unambiguous, so it is used while the quorum below is still
        // being gathered. Without this the first window of frames would keep
        // the uncorrected sawtooth every time the service starts.
        const int64_t provisional = plausibleSlots == 1 ? onlyCandidate : 0;
        if (++observed_ < kDiscoverySamples)
            return provisional;

        std::size_t bestSlot = 0;
        uint16_t bestHits = 0;
        for (std::size_t slot = 0; slot < kMaxPayloadSlots; ++slot) {
            if (hits_[slot] > bestHits) {
                bestHits = hits_[slot];
                bestSlot = slot;
            }
        }
        if (bestHits >= kDiscoveryQuorum) {
            located_ = true;
            offset_ = bestSlot * sizeof(uint32_t);
            consecutiveRejects_ = 0;
        } else {
            ++attempts_;
        }
        ClearDiscoveryWindow();
        return provisional;
    }

    void ClearDiscoveryWindow() {
        hits_.fill(0);
        observed_ = 0;
    }

    std::array<uint16_t, kMaxPayloadSlots> hits_{};
    std::size_t observed_ = 0;
    std::size_t attempts_ = 0;
    std::size_t offset_ = 0;
    std::size_t consecutiveRejects_ = 0;
    uint64_t rejected_ = 0;
    int64_t qpcFrequency_ = 0;
    bool located_ = false;
};

struct NvidiaFlipDelay {
    int64_t delay = 0;
    bool matched = false;
};

// Pairs each announcement with the flip that consumes it. Keyed on the driver
// thread like PresentMon's NVTraceConsumer, which degrades to no correction
// rather than a wrong one when the pairing cannot be made.
class NvidiaFlipDelayTracker {
public:
    void ObserveAnnouncement(uint32_t threadId, int64_t eventQpc, int64_t announcedQpc) {
        if (announcedQpc <= 0)
            return;
        const int64_t delay = announcedQpc > eventQpc ? announcedQpc - eventQpc : 0;
        // Insert, never overwrite: an announcement already outstanding on this
        // thread belongs to the flip the driver is about to program, and the
        // consuming flip below drops the rest. Overwriting would hand that flip
        // the following frame's screen time.
        requests_.emplace(threadId, PendingRequest{{delay, true}, eventQpc});
    }

    // The announcement belongs to the flip being programmed right now. On a hit
    // the whole table is dropped, not just this thread's entry: every other
    // outstanding announcement describes a flip that has already been programmed
    // and must never be applied to a later one.
    NvidiaFlipDelay TakeFlipDelay(uint32_t threadId) {
        const auto it = requests_.find(threadId);
        if (it == requests_.end())
            return {};
        const NvidiaFlipDelay result = it->second.delay;
        requests_.clear();
        return result;
    }

    void PruneBefore(int64_t cutoff) {
        for (auto it = requests_.begin(); it != requests_.end();)
            it = it->second.eventTimestamp < cutoff ? requests_.erase(it) : std::next(it);
    }

    void Clear() { requests_.clear(); }

    std::size_t pendingRequestCount() const noexcept { return requests_.size(); }

private:
    struct PendingRequest {
        NvidiaFlipDelay delay;
        int64_t eventTimestamp = 0;
    };

    std::unordered_map<uint32_t, PendingRequest> requests_;
};
