#pragma once

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

std::filesystem::path GetExecutableDirectory();
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
    static ReservedCaptureOutput ReserveForTesting(const std::filesystem::path& directory,
                                                   const std::wstring& prefix, const std::wstring& extension,
                                                   const OutputNameSeed& seed);

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

    // Removes the output only if its current Windows file identity still matches the
    // placeholder created by this reservation.
    bool CleanupOwnedFile();

private:
    struct FileIdentity {
        DWORD volumeSerial = 0;
        uint64_t fileIndex = 0;
        bool valid = false;
    };

    static ReservedCaptureOutput ReserveWithSeed(const std::filesystem::path& directory,
                                                 const std::wstring& prefix, const std::wstring& extension,
                                                 const OutputNameSeed& seed);
    static bool QueryIdentity(HANDLE handle, FileIdentity& identity);
    bool CurrentPathMatchesReservation() const;
    void Reset() noexcept;

    std::filesystem::path path_;
    HANDLE reservationHandle_ = INVALID_HANDLE_VALUE;
    FileIdentity identity_;
    bool published_ = false;
};

}  // namespace ce::capture_output
