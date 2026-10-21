#pragma once

// The journal walk of the recording recovery (libcengine 11, part 3): every staging file the in-flight journal names
// is handed to `recoverOne` (mediaengine's MediaEngine_RecoverRecording in production); entries whose outcome is
// terminal leave the journal, the others (writer still alive, a failed attempt) stay for the next run.

#include "captureengine/roles/role_entries.h"
#include "common/capture/inflight_journal.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace ce::role {

// Returns a ce::media::RecoveryOutcome value; fills `output` and `endUs` for a repaired or kept file.
using RecoverOneFn = std::function<int(const std::filesystem::path& staging, std::wstring* output, int64_t* endUs)>;

struct RecoveryPassSummary {
    uint32_t entries = 0;
    uint32_t recovered = 0;
    uint32_t discarded = 0;
    uint32_t keptIncomplete = 0;
    uint32_t writerAlive = 0;
    uint32_t missing = 0;
    uint32_t failed = 0;
    uint32_t canceled = 0;  // entries not attempted because `cancel` was set
};

std::vector<RecoveredRecording> RecoverJournalEntries(const ce::capture_output::InFlightJournal& journal,
                                                      const RecoverOneFn& recoverOne, const volatile long* cancel,
                                                      RecoveryPassSummary* summary);

}  // namespace ce::role
