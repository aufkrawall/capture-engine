#pragma once

// The bookkeeping behind FinalizedMedia (llm-wiki/library/04 "runtime/children"). The supervisor gives every
// media child a result slot at spawn and reports the child's exit through Closing(), the only point where the
// exit code and the slot can still be read. The Win32 calls stay in host_children.cpp; everything here is
// plain data, so the attribution rules are unit-tested with real slots and exit codes and no process.

#include "common/ipc/media_finalization.h"
#include "common/logging/logging.h"
#include "runtime/children/child_process_lifecycle.h"
#include "runtime/children/host_children.h"

#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <utility>
#include <vector>

namespace ce::runtime::detail {

class MediaFinalizations {
public:
    // A media child was spawned. The spawner keeps its result slot until the child has exited and been closed.
    void Adopt(ChildToken process, std::unique_ptr<ce::media_result::ResultSlot> slot) {
        entries_.insert_or_assign(process, Entry{std::move(slot), 0, false});
    }

    // The media child was retired for `recordingRequest`: its exit finalizes that recording.
    void Retire(ChildToken process, uint64_t recordingRequest) {
        const auto it = entries_.find(process);
        if (it == entries_.end())
            return;
        it->second.recordingRequest = recordingRequest;
        it->second.retired = true;
    }

    // The child has exited and is about to be closed; `exitCode` is its GetExitCodeProcess value. A child that
    // was never retired died while active: that is a crash for health recovery, not a finalization.
    void Closing(ChildToken process, uint32_t exitCode) {
        const auto it = entries_.find(process);
        if (it == entries_.end())
            return;
        if (it->second.retired) {
            ce::media_result::Result published;
            const bool hasResult = it->second.slot && it->second.slot->Read(published);
            FinalizedMedia finalized;
            finalized.recordingRequest = it->second.recordingRequest;
            finalized.resolution = ce::media_result::Resolve(exitCode, hasResult ? &published : nullptr);
            LogFinalized(finalized, exitCode);
            finalized_.push_back(std::move(finalized));
        }
        entries_.erase(it);
    }

    // Media children with an open record: the active one plus every retired one that is still running.
    size_t Tracked() const {
        return entries_.size();
    }

    std::vector<FinalizedMedia> Take() {
        std::vector<FinalizedMedia> taken;
        taken.swap(finalized_);
        return taken;
    }

private:
    struct Entry {
        std::unique_ptr<ce::media_result::ResultSlot> slot;
        uint64_t recordingRequest = 0;
        bool retired = false;
    };

    static void LogFinalized(const FinalizedMedia& finalized, uint32_t exitCode) {
        const ce::media_result::Result& result = finalized.resolution.result;
        LogInfo("[Runtime] recording request=%llu finalized result=%s files=%zu exitCode=0x%08X",
                static_cast<unsigned long long>(finalized.recordingRequest),
                ce::media_result::StatusName(result.status), result.paths.size(), exitCode);
        if (finalized.resolution.statusDisagreed)
            LogWarn("[Runtime] recording request=%llu result channels disagree (slot=%s exitCode=0x%08X)",
                    static_cast<unsigned long long>(finalized.recordingRequest),
                    ce::media_result::StatusName(result.status), exitCode);
    }

    std::map<ChildToken, Entry> entries_;
    std::vector<FinalizedMedia> finalized_;
};

}  // namespace ce::runtime::detail
