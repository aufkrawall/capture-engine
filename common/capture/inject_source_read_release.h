#pragma once

#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>

#include "inject_frame_ring_lease.h"

namespace ce {

// An encoder accepts an inject frame once its GPU reads of the producer's
// shared texture are submitted, not executed. Releasing the ring lease at that
// point lets the producer write the slot again; the encoder and the producer
// run on different devices, so that write can execute first and the encoded
// frame then shows newer pixels than its timestamp (visible as frames jumping
// back and forth when the producer outruns the output rate and reuses a just
// released slot). This queue holds such leases until the read token completes.
//
// Token requirements (move-only is fine):
//   bool Completed() const      - non-blocking; true once the reads finished
//   bool ArmEvent(HANDLE) const - signal the event when the reads finish
struct SourceReadReleaseWindow {
    uint32_t deferred = 0;
    uint32_t immediate = 0;
    uint32_t released = 0;
    uint32_t armFailures = 0;
    uint32_t maxPending = 0;
    int64_t maxHoldQpc = 0;
};

template <typename Token>
class SourceReadReleaseQueue final {
public:
    SourceReadReleaseQueue() noexcept : event_(CreateEventW(nullptr, FALSE, FALSE, nullptr)) {}
    ~SourceReadReleaseQueue() {
        // An incomplete token is not permission to reuse a texture, even at
        // teardown. Avoid publishing completion or touching an unmapped ring.
        for (Entry& entry : entries_)
            entry.lease.Abandon();
        entries_.clear();
        if (event_)
            CloseHandle(event_);
    }
    SourceReadReleaseQueue(const SourceReadReleaseQueue&) = delete;
    SourceReadReleaseQueue& operator=(const SourceReadReleaseQueue&) = delete;

    // Auto-reset event signaled when an armed token completes; owners wait on
    // it next to their own wake events and then call ReleaseCompleted.
    HANDLE CompletionEvent() const { return event_; }

    // Takes over `lease` until `token` completes. An already completed token
    // releases at once, so the common fast-GPU case costs no extra hold.
    void Defer(InjectFrameRingLease&& lease, Token token, int64_t nowQpc) {
        if (!lease)
            return;
        InjectFrameRingLease owned = std::move(lease);
        std::lock_guard<std::mutex> lock(mutex_);
        if (token.Completed()) {
            ++window_.immediate;
            ++total_.immediate;
            owned.Reset();
            return;
        }
        if (!event_ || !token.ArmEvent(event_)) {
            // Still released by the next ReleaseCompleted pass of either owner.
            ++window_.armFailures;
            ++total_.armFailures;
        }
        entries_.push_back(Entry{std::move(token), std::move(owned), nowQpc});
        ++window_.deferred;
        ++total_.deferred;
        const uint32_t pending = static_cast<uint32_t>(entries_.size());
        window_.maxPending = std::max(window_.maxPending, pending);
        total_.maxPending = std::max(total_.maxPending, pending);
    }

    // Releases every lease whose token completed. Tokens of one encoder fence
    // complete in order, but a device change starts a new fence, so each entry
    // is tested on its own; the ring publishes only the contiguous prefix.
    size_t ReleaseCompleted(int64_t nowQpc) {
        std::lock_guard<std::mutex> lock(mutex_);
        size_t released = 0;
        for (auto it = entries_.begin(); it != entries_.end();) {
            if (!it->token.Completed()) {
                ++it;
                continue;
            }
            NoteHold(*it, nowQpc);
            it->lease.Reset();
            it = entries_.erase(it);
            ++released;
        }
        window_.released += static_cast<uint32_t>(released);
        total_.released += static_cast<uint32_t>(released);
        return released;
    }

    // Call after both submitting and ingest threads joined. Normal capture
    // never waits; teardown must also prove the final GPU reads completed.
    // A timeout preserves ownership instead of allowing an unsafe ring reset.
    bool Drain(DWORD timeoutMs) {
        const ULONGLONG start = GetTickCount64();
        while (true) {
            LARGE_INTEGER now{};
            QueryPerformanceCounter(&now);
            ReleaseCompleted(now.QuadPart);
            if (Pending() == 0)
                return true;
            const ULONGLONG elapsed = GetTickCount64() - start;
            if (!event_ || elapsed >= timeoutMs)
                return false;
            const DWORD wait = WaitForSingleObject(event_, timeoutMs - static_cast<DWORD>(elapsed));
            if (wait == WAIT_FAILED)
                return false;
        }
    }

    size_t Pending() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return entries_.size();
    }

    SourceReadReleaseWindow TakeWindow() {
        std::lock_guard<std::mutex> lock(mutex_);
        SourceReadReleaseWindow window = window_;
        window_ = {};
        return window;
    }

    SourceReadReleaseWindow Totals() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return total_;
    }

private:
    struct Entry {
        Token token;
        InjectFrameRingLease lease;
        int64_t deferredQpc = 0;
    };

    void NoteHold(const Entry& entry, int64_t nowQpc) {
        if (nowQpc <= 0 || entry.deferredQpc <= 0 || nowQpc < entry.deferredQpc)
            return;
        const int64_t hold = nowQpc - entry.deferredQpc;
        window_.maxHoldQpc = std::max(window_.maxHoldQpc, hold);
        total_.maxHoldQpc = std::max(total_.maxHoldQpc, hold);
    }

    HANDLE event_ = nullptr;
    mutable std::mutex mutex_;
    std::deque<Entry> entries_;
    SourceReadReleaseWindow window_;
    SourceReadReleaseWindow total_;
};

}  // namespace ce
