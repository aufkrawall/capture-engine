#pragma once

// The one record of recordings that are still being written (libcengine 11, part 3). A recording's staging file gets
// an entry when it is reserved and loses it when the file is published or discarded; an entry that outlives its
// writer (a crashed media helper, or a crashed client running the media role on a thread) names a file the recording
// recovery repairs. Entries live per Windows user (%LOCALAPPDATA%\CaptureEngine\inflight), independent of log_level,
// of the package folder's permissions and of later output_dir/container changes. One small file per entry, holding
// only the staging file's absolute path as UTF-8: nothing else to parse.

#include <filesystem>
#include <string>
#include <vector>

namespace ce::capture_output {

class InFlightJournal {
public:
    explicit InFlightJournal(std::filesystem::path directory);

    // %LOCALAPPDATA%\CaptureEngine\inflight (unit tests: CaptureEngine_UT, DR-48). Empty when the folder is unknown.
    static InFlightJournal ForThisUser();

    // Records `staging`; false when the entry could not be written (logged once per process).
    bool Add(const std::filesystem::path& staging) const;
    // Forgets `staging`; a missing entry is not an error.
    void Remove(const std::filesystem::path& staging) const;
    // Every staging path the journal names, in no particular order. Unreadable entries are skipped.
    std::vector<std::filesystem::path> Entries() const;

    const std::filesystem::path& Directory() const { return directory_; }
    explicit operator bool() const { return !directory_.empty(); }

    // The entry file for `staging` (the journal directory plus a hash of the path), exposed for tests.
    std::filesystem::path EntryPath(const std::filesystem::path& staging) const;

private:
    std::filesystem::path directory_;
};

}  // namespace ce::capture_output
