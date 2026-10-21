#pragma once

#include "inflight_journal.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>

namespace ce::capture_output {

struct OutputNameSeed {
    uint64_t utcMilliseconds = 0;
    uint32_t processId = 0;
    uint64_t sequence = 0;
};

// Fresh name seed for an output publication. Several outputs produced from one
// capture share a seed so their published names differ only by extension.
OutputNameSeed MakeOutputNameSeed();

// Expands %VIDEOS%, %DOCUMENTS% (Windows library folders, wherever they were
// moved) and %VAR% environment references in a configured directory. Undefined
// references are left in place.
std::wstring ExpandConfiguredPathReferences(const std::wstring& value);

// The runtime package directory: the directory of the binary this code runs in, not of the client executable.
std::filesystem::path GetPackageDirectory();
std::filesystem::path ResolveCaptureDirectory(const std::string& configuredDirectory,
                                              const std::filesystem::path& executableDirectory);

class ReservedCaptureOutput {
public:
    ReservedCaptureOutput() = default;
    ~ReservedCaptureOutput();

    ReservedCaptureOutput(const ReservedCaptureOutput&) = delete;
    ReservedCaptureOutput& operator=(const ReservedCaptureOutput&) = delete;
    ReservedCaptureOutput(ReservedCaptureOutput&& other) noexcept;
    ReservedCaptureOutput& operator=(ReservedCaptureOutput&& other) noexcept;

    static ReservedCaptureOutput Reserve(const std::filesystem::path& directory, const std::wstring& prefix,
                                         const std::wstring& extension);
    static ReservedCaptureOutput ReserveForTesting(const std::filesystem::path& directory, const std::wstring& prefix,
                                                   const std::wstring& extension, const OutputNameSeed& seed);

    explicit operator bool() const {
        return reservationHandle_ != INVALID_HANDLE_VALUE && !path_.empty();
    }

    const std::filesystem::path& Path() const {
        return path_;
    }
    std::string Utf8Path() const;

    // Validate the CREATE_NEW placeholder before the writer opens it. The reservation
    // handle remains open for the writer's entire lifetime so the path cannot be
    // deleted or replaced between validation, open, and final publication.
    bool ReleaseToWriter();

    // The writer has flushed and closed the final file successfully. Ownership cleanup
    // is disabled, but the path remains available for post-mux probing and diagnostics.
    void Publish();

    // Replace the owned zero-byte placeholder with a completely written and closed
    // staging reservation. Both identities are checked before their delete-blocking
    // handles are closed for the atomic ReplaceFile operation.
    bool CommitStagingFile(ReservedCaptureOutput& staging);

    // Atomically rename this completely written staging reservation to a fresh,
    // collision-safe output name. Unlike CommitStagingFile(), this does not expose a
    // zero-byte final-extension placeholder while a slow encoder is still running.
    bool PublishToNewPath(const std::filesystem::path& directory, const std::wstring& prefix,
                          const std::wstring& extension);

    // Same publication with a caller-supplied name seed, so that two outputs
    // encoded from one capture are published under one name and differ only by
    // extension. Distinct extensions cannot collide under a shared seed.
    bool PublishToNewPathWithSeed(const std::filesystem::path& directory, const std::wstring& prefix,
                                  const std::wstring& extension, const OutputNameSeed& seed);
    bool PublishToNewPathForTesting(const std::filesystem::path& directory, const std::wstring& prefix,
                                    const std::wstring& extension, const OutputNameSeed& seed);

    // Removes the output only if its current Windows file identity still matches the
    // placeholder created by this reservation.
    bool CleanupOwnedFile();

    // Records this unpublished file in `journal` until it is published, committed or removed, so a writer that dies
    // first leaves an entry the recording recovery finds (inflight_journal.h). False when nothing was recorded.
    bool TrackInFlight(const InFlightJournal& journal);

private:
    struct FileIdentity {
        DWORD volumeSerial = 0;
        uint64_t fileIndex = 0;
        bool valid = false;
    };

    static ReservedCaptureOutput ReserveWithSeed(const std::filesystem::path& directory, const std::wstring& prefix,
                                                 const std::wstring& extension, const OutputNameSeed& seed);
    static bool QueryIdentity(HANDLE handle, FileIdentity& identity);
    bool CurrentPathMatchesReservation() const;
    void ForgetInFlight();
    void Reset() noexcept;

    std::filesystem::path path_;
    HANDLE reservationHandle_ = INVALID_HANDLE_VALUE;
    FileIdentity identity_;
    bool published_ = false;
    std::filesystem::path inFlightJournal_;  // the journal directory while this file has an entry; empty otherwise
};

}  // namespace ce::capture_output
