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

    void ObserveRuntimePresent(uint32_t processId, uint32_t threadId, int64_t timestamp) {
        auto& pending = pendingPresents_[processId];
        if (pending.size() >= kMaxPendingPresentsPerProcess)
            pending.pop_front();
        pending.push_back({threadId, timestamp});
        ++observedPresents_;
    }

    // The submitting thread belongs to the presenting process even when it is
    // not the thread that called Present, so the process is the key and the
    // thread only refines the choice within it.
    bool Associate(uint32_t processId, uint32_t threadId, uint32_t submitSequence, int64_t timestamp) {
        const auto process = pendingPresents_.find(processId);
        if (process == pendingPresents_.end() || process->second.empty())
            return false;
        auto& pending = process->second;
        std::array<uint32_t, kMaxPendingPresentsPerProcess> pendingThreadIds = {};
        const std::size_t pendingCount = std::min(pending.size(), pendingThreadIds.size());
        for (std::size_t i = 0; i < pendingCount; ++i)
            pendingThreadIds[i] = pending[i].threadId;
        const std::size_t selected =
            SelectDisplaySubmissionPresent(pendingThreadIds.data(), pendingCount, threadId);
        if (selected == kNoPendingDisplayPresent)
            return false;
        associations_[submitSequence].push_back(
            {processId, timestamp, nextAssociationId_++, pending[selected].timestamp});
        pending.erase(pending.begin() + static_cast<std::deque<PendingRuntimePresent>::difference_type>(selected));
        if (pending.empty())
            pendingPresents_.erase(process);
        ++observedAssociations_;
        return true;
    }

    const SubmitAssociation* Find(uint32_t submitSequence) const {
        const auto association = associations_.find(submitSequence);
        if (association != associations_.end() && !association->second.empty())
            return &association->second.front();
        return nullptr;
    }

    void Erase(uint32_t submitSequence) {
        const auto association = associations_.find(submitSequence);
        if (association == associations_.end())
            return;
        association->second.pop_front();
        if (association->second.empty())
            associations_.erase(association);
    }

    void PruneBefore(int64_t cutoff) {
        for (auto it = pendingPresents_.begin(); it != pendingPresents_.end();) {
            auto& presents = it->second;
            while (!presents.empty() && presents.front().timestamp < cutoff)
                presents.pop_front();
            it = presents.empty() ? pendingPresents_.erase(it) : std::next(it);
        }
        for (auto it = associations_.begin(); it != associations_.end();) {
            auto& associations = it->second;
            while (!associations.empty() && associations.front().timestamp < cutoff)
                associations.pop_front();
            it = associations.empty() ? associations_.erase(it) : std::next(it);
        }
    }

    void Clear() {
        pendingPresents_.clear();
        associations_.clear();
    }

    uint64_t observedPresents() const noexcept { return observedPresents_; }
    uint64_t observedAssociations() const noexcept { return observedAssociations_; }

private:
    std::unordered_map<uint32_t, std::deque<PendingRuntimePresent>> pendingPresents_;
    std::unordered_map<uint32_t, std::deque<SubmitAssociation>> associations_;
    uint64_t nextAssociationId_ = 1;
    uint64_t observedPresents_ = 0;
    uint64_t observedAssociations_ = 0;
};
