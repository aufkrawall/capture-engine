// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <string>

struct AVIOContext;

namespace ce::mux {

inline constexpr uint64_t kDefaultMaxLiveSpoolBytes = 2ull * 1024ull * 1024ull * 1024ull;  // 2 GiB
inline constexpr uint64_t kDefaultFinalizeSpoolReserveBytes = 64ull * 1024ull * 1024ull;   // 64 MiB for flush + trailer
inline constexpr uint64_t kTargetReconnectProbeIntervalMs = 5000ull;

// %LOCALAPPDATA%\CaptureEngine\spool (unit tests: CaptureEngine_UT\spool).
std::filesystem::path SpoolDirectoryForThisUser();

// Deterministic .spool path for a staging file inside `spoolDirectory` (defaults to SpoolDirectoryForThisUser()).
std::filesystem::path SpoolPathForStaging(const std::filesystem::path& stagingPath,
                                          const std::filesystem::path& spoolDirectory = {});

bool HasSpoolForStaging(const std::filesystem::path& stagingPath,
                        const std::filesystem::path& spoolDirectory = {});
bool CanReplaySpoolWithoutExistingTarget(const std::filesystem::path& stagingPath,
                                         const std::filesystem::path& spoolDirectory = {});
void RemoveSpoolForStaging(const std::filesystem::path& stagingPath,
                           const std::filesystem::path& spoolDirectory = {});

enum class SpoolReplayOutcome {
    kNoSpool,
    kReplayed,
    kTargetBusy,
    kTargetUnreachable,
    kCorruptSpool,
    kCanceled,
};

struct SpoolReplayResult {
    SpoolReplayOutcome outcome = SpoolReplayOutcome::kNoSpool;
    bool finalized = false;
    uint64_t replayedRecords = 0;
    uint64_t replayedBytes = 0;
    uint64_t finalFileSize = 0;
    std::string detail;
};

// Applies a local .spool file onto `stagingPath` and removes the .spool file when replay succeeds.
SpoolReplayResult ReplaySpoolForStaging(const std::filesystem::path& stagingPath,
                                        const std::filesystem::path& spoolDirectory = {},
                                        const volatile long* cancel = nullptr,
                                        const std::function<void()>& onProgress = {});

struct ResilientOutputSinkOptions {
    std::filesystem::path spoolDirectory;  // empty -> SpoolDirectoryForThisUser()
    uint64_t maxLiveSpoolBytes = kDefaultMaxLiveSpoolBytes;
    uint64_t finalizeReserveBytes = kDefaultFinalizeSpoolReserveBytes;
    uint64_t reconnectProbeIntervalMs = kTargetReconnectProbeIntervalMs;
    std::function<void()> onRefreshDeadline;
    std::function<bool()> isAbortRequested;
};

// Seekable AVIO output sink for local recordings. Writes directly to the staging file while the destination storage
// is healthy; if a write or seek on the destination fails (for example, an SMB network share stalls and drops the
// open handle), transparently fails over to an append-only local write-ahead spool so queued packets, encoder flush
// packets, and av_write_trailer seek-back patches (MKV Cues/SeekHead/Duration, MP4 moov) are preserved and replayed
// onto the destination file as soon as it is reachable again.
class ResilientOutputSink {
public:
    using Options = ResilientOutputSinkOptions;

    ResilientOutputSink() = default;
    ~ResilientOutputSink();

    ResilientOutputSink(const ResilientOutputSink&) = delete;
    ResilientOutputSink& operator=(const ResilientOutputSink&) = delete;

    int Open(const std::filesystem::path& stagingPath, AVIOContext** pbOut, Options options = {});

    void EnterFinalizePhase() noexcept { inFinalizePhase_ = true; }

    int Close(AVIOContext** pbInOut);
    void Discard(AVIOContext** pbInOut = nullptr) noexcept;

    bool IsOpen() const noexcept { return open_; }
    bool IsSpooling() const noexcept { return spooling_; }
    bool UsedSpool() const noexcept { return usedSpool_; }
    bool SpoolExhausted() const noexcept { return spoolExhausted_; }
    bool HasPendingSpool() const noexcept { return pendingSpool_; }
    uint64_t SpooledPayloadBytes() const noexcept { return spooledPayloadBytes_; }
    uint64_t LogicalSize() const noexcept { return logicalSize_; }
    const std::filesystem::path& SpoolPath() const noexcept { return spoolPath_; }

private:
    static int AvioWritePacket(void* opaque, const uint8_t* buf, int bufSize);
    static int64_t AvioSeek(void* opaque, int64_t offset, int whence);

    int WritePacket(const uint8_t* buf, int bufSize);
    int64_t Seek(int64_t offset, int whence);
    bool BeginSpooling(const char* operation, DWORD win32Error);
    bool WriteSpoolHeader(bool finalized);
    void TryReplayAndResumeDirect();
    void CloseHandles() noexcept;
    void FreeAvioContext(AVIOContext** pbInOut) noexcept;

    std::filesystem::path stagingPath_;
    std::filesystem::path spoolPath_;
    Options options_;
    AVIOContext* avioCtx_ = nullptr;
    HANDLE targetHandle_ = INVALID_HANDLE_VALUE;
    HANDLE spoolHandle_ = INVALID_HANDLE_VALUE;
    uint64_t position_ = 0;
    uint64_t targetPosition_ = 0;
    uint64_t logicalSize_ = 0;
    uint64_t confirmedTargetSize_ = 0;
    uint64_t spooledPayloadBytes_ = 0;
    uint64_t spooledRecordCount_ = 0;
    uint64_t lastReconnectProbeMs_ = 0;
    bool open_ = false;
    bool spooling_ = false;
    bool usedSpool_ = false;
    bool inFinalizePhase_ = false;
    bool spoolExhausted_ = false;
    bool pendingSpool_ = false;
};

}  // namespace ce::mux
