#pragma once

// Transactional owner of config.ini (llm-wiki/library/03 "Settings", 04 "runtime/settings").
//
// Readers take immutable snapshots. A commit applies typed edits to the file's own text, so comments,
// order, unknown lines and the line endings of untouched lines stay byte for byte; the candidate is
// validated with the production loader; the file is then replaced atomically (temp file in the same
// directory, MoveFileEx with write-through). External edits are picked up by Poll with the debounce and
// coherent-read rules of configuration_state.h, and a candidate that validates with errors keeps the
// published snapshot.
//
// Threads: Current, Revision and Value may be called from any thread. Poll, NextPollDelayMs, TakeRejection
// and Commit are serialized against each other.

#include "common/config/config.h"
#include "common/config/config_diagnostics.h"
#include "common/config/config_ini_reader.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace ce::runtime {

// One change to config.ini. Section and key match the way a lookup does (case-insensitive, blanks around
// them ignored). An empty optional value removes the key; a value is one line of UTF-8 text.
struct SettingsEdit {
    std::string section;
    std::string key;
    std::optional<std::string> value;
};

enum class CommitStatus { Committed, Invalid, Conflict, IoError };

struct CommitResult {
    CommitStatus status = CommitStatus::IoError;
    // Invalid: why (the errors, and the warnings of the candidate). Committed: the candidate's warnings.
    // Conflict and IoError: one error entry describing the failure.
    ConfigDiagnostics diagnostics;
    uint64_t revision = 0;  // the revision after the call: new on Committed, unchanged otherwise
};

class SettingsStore {
public:
    // `path` is the config.ini file; a missing file gets the default template (as LoadConfig always did).
    // `nowMs` is a monotonic clock in milliseconds (GetTickCount64 when null).
    SettingsStore(std::wstring path, uint64_t (*nowMs)() noexcept = nullptr);
    ~SettingsStore();
    SettingsStore(const SettingsStore&) = delete;
    SettingsStore& operator=(const SettingsStore&) = delete;

    // The published settings. The snapshot never changes; a later publish replaces the pointer.
    std::shared_ptr<const AppConfig> Current() const;
    // Bumps on every published snapshot (external edit or commit). Starts at 1.
    uint64_t Revision() const;
    // The stored text of a key (UTF-8, quotes removed as the loader does), not the default. Nullopt when absent.
    std::optional<std::string> Value(std::string_view section, std::string_view key) const;

    // Checks the file for external edits. True when a new snapshot was published. A stable external edit that
    // validates with errors is refused: the published snapshot stays, and TakeRejection reports it.
    bool Poll();
    // Milliseconds until the next Poll can publish a change (0 when a check is due).
    uint32_t NextPollDelayMs() const;
    // The refusal of the last external edit Poll refused, once: nullopt when there is none to report.
    std::optional<std::string> TakeRejection();

    // Pure, any thread: the production loader in diagnostic mode over the candidate, with a process name that
    // matches no profile. An error is an entry with kError severity; every rejected value is one.
    static ConfigDiagnostics Validate(const ce::config_text::IniDocument& candidate);

    // Applies the edits to the file's text and validates the result. Conflict when the revision moved since
    // `baseRevision` or the file is no longer the text this store published. Invalid (file unchanged) for an
    // edit the file format cannot hold, a multi-line list key, a file that is not UTF-8, or a candidate with
    // errors. A commit that changes nothing writes nothing and keeps the revision.
    CommitResult Commit(const std::vector<SettingsEdit>& edits, uint64_t baseRevision);

private:
    class Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace ce::runtime
