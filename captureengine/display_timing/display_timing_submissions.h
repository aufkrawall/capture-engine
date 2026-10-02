#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <iterator>
#include <unordered_map>

#include "display_timing_correlation.h"
#include "display_timing_policy.h"

// Associates each runtime present with the graphics-kernel submission that
// carries it, which is what turns a later flip completion back into "this
// process's frame". Kept beside the other reducers rather than inside the
// service so the service translation unit stays about plumbing.
class DisplaySubmissionTracker {
public:
    // Only bounds a runaway producer: a present that never reaches a kernel
    // submission is dropped by age in PruneBefore.
    static constexpr std::size_t kMaxPendingPresentsPerProcess = 16;

    void ObserveRuntimePresent(uint32_t processId, uint32_t threadId, int64_t timestamp,
                               int32_t syncInterval = kUnknownSyncInterval) {
        auto& pending = pendingPresents_[processId];
        if (pending.size() >= kMaxPendingPresentsPerProcess)
            pending.pop_front();
        pending.push_back({threadId, timestamp, syncInterval});
        ++observedPresents_;
    }

    // The submitting thread belongs to the presenting process even when it is
    // not the thread that called Present, so the process is the key and the
    // thread only refines the choice within it.
    // When no user-mode runtime present exists (e.g. Vulkan / non-DXGI swapchains),
    // associate the kernel submission directly using the queue packet timestamp.
    bool Associate(uint32_t processId, uint32_t threadId, uint32_t submitSequence, int64_t timestamp,
                   bool* outIsFallback = nullptr) {
        if (outIsFallback)
            *outIsFallback = false;
        const auto process = pendingPresents_.find(processId);
        if (process != pendingPresents_.end() && !process->second.empty()) {
            auto& pending = process->second;
            std::array<uint32_t, kMaxPendingPresentsPerProcess> pendingThreadIds = {};
            const std::size_t pendingCount = std::min(pending.size(), pendingThreadIds.size());
            for (std::size_t i = 0; i < pendingCount; ++i)
                pendingThreadIds[i] = pending[i].threadId;
            const std::size_t selected =
                SelectDisplaySubmissionPresent(pendingThreadIds.data(), pendingCount, threadId);
            if (selected != kNoPendingDisplayPresent) {
                associations_[submitSequence].push_back({processId, timestamp, nextAssociationId_++,
                                                         pending[selected].timestamp, pending[selected].syncInterval});
                pending.erase(pending.begin() +
                              static_cast<std::deque<PendingRuntimePresent>::difference_type>(selected));
                if (pending.empty())
                    pendingPresents_.erase(process);
                ++observedAssociations_;
                return true;
            }
        }
        associations_[submitSequence].push_back(
            {processId, timestamp, nextAssociationId_++, timestamp});
        if (outIsFallback)
            *outIsFallback = true;
        ++observedAssociations_;
        ++observedFallbackAssociations_;
        return true;
    }

    // Bounds how long a submission may wait for its completion; see
    // kMaxSubmitToCompletionUs. Zero leaves it unbounded.
    void SetMaxCompletionAge(int64_t maxAge) noexcept {
        maxCompletionAge_ = maxAge;
    }

    const SubmitAssociation* Find(uint32_t submitSequence) const {
        const auto association = associations_.find(submitSequence);
        if (association != associations_.end() && !association->second.empty())
            return &association->second.front();
        return nullptr;
    }

    // The association a completion at completionTimestamp can actually
    // belong to. A submit sequence is only a 32-bit number and different
    // engines count independently, so an entry the completion could not have
    // come from - older than the completion bound - is a stale present that
    // never flipped, and it is dropped here rather than reported as a frame
    // that took seconds to reach the screen. An entry newer than the
    // completion is a later submission reusing the number; it stays for its
    // own completion.
    const SubmitAssociation* FindForCompletion(uint32_t submitSequence, int64_t completionTimestamp) {
        const auto found = associations_.find(submitSequence);
        if (found == associations_.end())
            return nullptr;
        auto& queue = found->second;
        while (!queue.empty() && completionTimestamp >= queue.front().timestamp &&
               !IsPlausibleSubmitCompletion(queue.front().timestamp, completionTimestamp, maxCompletionAge_)) {
            queue.pop_front();
            ++rejectedStaleCompletions_;
        }
        if (queue.empty()) {
            associations_.erase(found);
            return nullptr;
        }
        if (!IsPlausibleSubmitCompletion(queue.front().timestamp, completionTimestamp, maxCompletionAge_))
            return nullptr;
        ++matchedCompletions_;
        return &queue.front();
    }

    // Removes exactly that association, and reports whether it was still
    // waiting: a composed frame is published only if its own flip has not
    // already completed or expired it.
    bool EraseAssociation(uint32_t submitSequence, uint64_t associationId) {
        const auto found = associations_.find(submitSequence);
        if (found == associations_.end())
            return false;
        auto& queue = found->second;
        for (auto it = queue.begin(); it != queue.end(); ++it) {
            if (it->associationId != associationId)
                continue;
            queue.erase(it);
            if (queue.empty())
                associations_.erase(found);
            return true;
        }
        return false;
    }

    // Drops a process's outstanding submissions without counting them as
    // expired, for a process that is no longer followed (the compositor).
    void EraseProcess(uint32_t processId) {
        for (auto it = associations_.begin(); it != associations_.end();) {
            auto& queue = it->second;
            for (auto entry = queue.begin(); entry != queue.end();)
                entry = entry->processId == processId ? queue.erase(entry) : std::next(entry);
            it = queue.empty() ? associations_.erase(it) : std::next(it);
        }
    }

    void Erase(uint32_t submitSequence) {
        const auto association = associations_.find(submitSequence);
        if (association == associations_.end())
            return;
        association->second.pop_front();
        if (association->second.empty())
            associations_.erase(association);
    }

    // Returns how many submissions expired without ever being completed: a
    // flip-model present replaced before its blank, or one that never flips at
    // all because the frame is composed or copied to the screen.
    std::size_t PruneBefore(int64_t cutoff) {
        for (auto it = pendingPresents_.begin(); it != pendingPresents_.end();) {
            auto& presents = it->second;
            while (!presents.empty() && presents.front().timestamp < cutoff)
                presents.pop_front();
            it = presents.empty() ? pendingPresents_.erase(it) : std::next(it);
        }
        std::size_t expired = 0;
        for (auto it = associations_.begin(); it != associations_.end();) {
            auto& associations = it->second;
            while (!associations.empty() && associations.front().timestamp < cutoff) {
                lastExpiredProcessId_ = associations.front().processId;
                associations.pop_front();
                ++expired;
            }
            it = associations.empty() ? associations_.erase(it) : std::next(it);
        }
        expiredAssociations_ += expired;
        return expired;
    }

    void Clear() {
        pendingPresents_.clear();
        associations_.clear();
    }

    uint64_t observedPresents() const noexcept { return observedPresents_; }
    uint64_t observedAssociations() const noexcept { return observedAssociations_; }
    uint64_t observedFallbackAssociations() const noexcept { return observedFallbackAssociations_; }
    uint64_t matchedCompletions() const noexcept { return matchedCompletions_; }
    uint64_t rejectedStaleCompletions() const noexcept { return rejectedStaleCompletions_; }
    uint64_t expiredAssociations() const noexcept { return expiredAssociations_; }
    uint32_t lastExpiredProcessId() const noexcept { return lastExpiredProcessId_; }

private:
    std::unordered_map<uint32_t, std::deque<PendingRuntimePresent>> pendingPresents_;
    std::unordered_map<uint32_t, std::deque<SubmitAssociation>> associations_;
    int64_t maxCompletionAge_ = 0;
    uint64_t nextAssociationId_ = 1;
    uint64_t observedPresents_ = 0;
    uint64_t observedAssociations_ = 0;
    uint64_t observedFallbackAssociations_ = 0;
    uint64_t matchedCompletions_ = 0;
    uint64_t rejectedStaleCompletions_ = 0;
    uint64_t expiredAssociations_ = 0;
    uint32_t lastExpiredProcessId_ = 0;
};

// Turns per-prune counts into the two transitions worth a log line: tracked
// presents stop completing as flips (a composed or copied present), and flips
// resume. Expiry alone is not that state - a flip-model present replaced by a
// newer one before its blank never completes either - so it starts only when
// a prune saw submissions expire and none complete, and ends at the first
// completion. The state changes at most once per minimum interval, so a mode
// alternating between the two reports once instead of on every prune, and the
// reported state is always the state held.
class DisplaySubmissionExpiryMonitor {
public:
    enum class Transition : uint8_t {
        None,
        Started,
        Stopped,
    };

    static constexpr uint64_t kMinTransitionIntervalMs = 10'000;

    // expired is this prune's count; matchedTotal is the tracker's cumulative
    // completion count, turned into a per-prune delta here.
    Transition Observe(uint64_t expired, uint64_t matchedTotal, uint64_t nowMs) {
        const uint64_t completed = matchedTotal - matchedAtLastObserve_;
        matchedAtLastObserve_ = matchedTotal;
        lastCompleted_ = completed;
        const bool changeDue = !transitioned_ || nowMs - lastTransitionMs_ >= kMinTransitionIntervalMs;
        if (!expiring_ && expired != 0 && completed == 0 && changeDue)
            return Change(true, nowMs);
        if (expiring_ && completed != 0 && changeDue)
            return Change(false, nowMs);
        return Transition::None;
    }

    bool expiring() const noexcept { return expiring_; }
    uint64_t lastCompleted() const noexcept { return lastCompleted_; }

private:
    Transition Change(bool expiring, uint64_t nowMs) {
        expiring_ = expiring;
        transitioned_ = true;
        lastTransitionMs_ = nowMs;
        return expiring ? Transition::Started : Transition::Stopped;
    }

    bool expiring_ = false;
    bool transitioned_ = false;
    uint64_t lastTransitionMs_ = 0;
    uint64_t matchedAtLastObserve_ = 0;
    uint64_t lastCompleted_ = 0;
};
