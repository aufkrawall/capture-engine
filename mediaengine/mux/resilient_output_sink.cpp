// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
#include "resilient_output_sink.h"

#include "common/logging/log_privacy.h"
#include "mediaengine/engine/mediaengine.h"

#include <shlobj.h>

extern "C" {
#include <libavformat/avio.h>
#include <libavutil/error.h>
#include <libavutil/mem.h>
}

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <system_error>
#include <vector>

namespace ce::mux {
namespace {

#if defined(CE_UNIT_TESTS)
constexpr wchar_t kProductFolder[] = L"CaptureEngine_UT";
#else
constexpr wchar_t kProductFolder[] = L"CaptureEngine";
#endif
constexpr wchar_t kSpoolExtension[] = L".spool";

constexpr uint32_t kSpoolFileMagic = 0x4C505343u;    // "CPSL"
constexpr uint32_t kSpoolFileVersion = 1u;
constexpr uint32_t kSpoolRecordMagic = 0x524C5053u;  // "SPLR"
constexpr int kAvioBufferSize = 64 * 1024;
constexpr size_t kReplayCopyChunkBytes = 256 * 1024;

struct SpoolFileHeader {
    uint32_t magic = kSpoolFileMagic;
    uint32_t version = kSpoolFileVersion;
    uint64_t baseFileSize = 0;
    uint64_t logicalFileSize = 0;
    uint32_t finalized = 0;
    uint32_t reserved = 0;
};
static_assert(sizeof(SpoolFileHeader) == 32, "SpoolFileHeader layout must be 32 bytes");

struct SpoolRecordHeader {
    uint32_t magic = kSpoolRecordMagic;
    uint32_t payloadBytes = 0;
    uint64_t targetOffset = 0;
};
static_assert(sizeof(SpoolRecordHeader) == 16, "SpoolRecordHeader layout must be 16 bytes");

std::string CollapsePath(const std::filesystem::path& path) {
    const std::wstring wide = path.wstring();
    if (wide.empty())
        return {};
    const int bytes =
        WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), nullptr, 0, nullptr, nullptr);
    if (bytes <= 0)
        return {};
    std::string utf8(static_cast<size_t>(bytes), '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), static_cast<int>(wide.size()), utf8.data(), bytes, nullptr, nullptr);
    return ce::privacy::CollapsePathForLog(utf8);
}

uint64_t PathHash(const std::filesystem::path& path) {
    std::error_code ec;
    const std::filesystem::path absolute = std::filesystem::absolute(path, ec);
    std::wstring folded = (ec ? path : absolute).lexically_normal().wstring();
    if (!folded.empty())
        CharLowerBuffW(folded.data(), static_cast<DWORD>(folded.size()));
    uint64_t hash = 1469598103934665603ull;
    for (const wchar_t c : folded) {
        hash ^= static_cast<uint16_t>(c);
        hash *= 1099511628211ull;
    }
    return hash;
}

bool WriteAll(HANDLE handle, const void* data, size_t size) {
    const auto* cursor = static_cast<const uint8_t*>(data);
    size_t remaining = size;
    while (remaining > 0) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(remaining, 16u * 1024u * 1024u));
        DWORD written = 0;
        if (!WriteFile(handle, cursor, chunk, &written, nullptr) || written != chunk)
            return false;
        cursor += written;
        remaining -= written;
    }
    return true;
}

bool ReadExact(HANDLE handle, void* data, size_t size, size_t* bytesReadOut = nullptr) {
    auto* cursor = static_cast<uint8_t*>(data);
    size_t totalRead = 0;
    while (totalRead < size) {
        const DWORD chunk = static_cast<DWORD>(std::min<size_t>(size - totalRead, 16u * 1024u * 1024u));
        DWORD got = 0;
        if (!ReadFile(handle, cursor + totalRead, chunk, &got, nullptr)) {
            if (bytesReadOut)
                *bytesReadOut = totalRead;
            return false;
        }
        if (got == 0)
            break;
        totalRead += got;
    }
    if (bytesReadOut)
        *bytesReadOut = totalRead;
    return totalRead == size;
}

bool SeekHandle(HANDLE handle, uint64_t offset, DWORD moveMethod = FILE_BEGIN, uint64_t* newPosOut = nullptr) {
    LARGE_INTEGER distance{};
    distance.QuadPart = static_cast<LONGLONG>(offset);
    LARGE_INTEGER newPos{};
    if (!SetFilePointerEx(handle, distance, &newPos, moveMethod) || newPos.QuadPart < 0)
        return false;
    if (newPosOut)
        *newPosOut = static_cast<uint64_t>(newPos.QuadPart);
    return true;
}

bool TruncateHandleAt(HANDLE handle, uint64_t size) {
    return SeekHandle(handle, size) && SetEndOfFile(handle) != FALSE;
}

SpoolReplayResult ReplaySpoolInternal(const std::filesystem::path& stagingPath,
                                      const std::filesystem::path& spoolPath,
                                      DWORD targetShareMode,
                                      const volatile long* cancel,
                                      const std::function<void()>& onProgress) {
    SpoolReplayResult result;
    if (spoolPath.empty() || stagingPath.empty()) {
        result.outcome = SpoolReplayOutcome::kNoSpool;
        return result;
    }

    HANDLE spool = CreateFileW(spoolPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (spool == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
            result.outcome = SpoolReplayOutcome::kNoSpool;
        } else {
            result.outcome = SpoolReplayOutcome::kCorruptSpool;
            result.detail = "spool open error " + std::to_string(err);
        }
        return result;
    }
    struct HandleGuard {
        HANDLE h = INVALID_HANDLE_VALUE;
        ~HandleGuard() {
            if (h != INVALID_HANDLE_VALUE)
                CloseHandle(h);
        }
    } spoolGuard{spool};

    LARGE_INTEGER spoolFileSizeLi{};
    if (!GetFileSizeEx(spool, &spoolFileSizeLi) ||
        spoolFileSizeLi.QuadPart < static_cast<LONGLONG>(sizeof(SpoolFileHeader))) {
        CloseHandle(spoolGuard.h);
        spoolGuard.h = INVALID_HANDLE_VALUE;
        DeleteFileW(spoolPath.c_str());
        result.outcome = SpoolReplayOutcome::kCorruptSpool;
        result.detail = "truncated spool header";
        return result;
    }
    const uint64_t spoolFileSize = static_cast<uint64_t>(spoolFileSizeLi.QuadPart);

    SpoolFileHeader header{};
    if (!ReadExact(spool, &header, sizeof(header)) || header.magic != kSpoolFileMagic ||
        header.version != kSpoolFileVersion) {
        CloseHandle(spoolGuard.h);
        spoolGuard.h = INVALID_HANDLE_VALUE;
        DeleteFileW(spoolPath.c_str());
        result.outcome = SpoolReplayOutcome::kCorruptSpool;
        result.detail = "invalid spool header";
        return result;
    }
    result.finalized = (header.finalized != 0);

    const DWORD creationDisposition = (header.baseFileSize == 0) ? OPEN_ALWAYS : OPEN_EXISTING;
    HANDLE target = CreateFileW(stagingPath.c_str(), GENERIC_READ | GENERIC_WRITE, targetShareMode, nullptr,
                                creationDisposition, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (target == INVALID_HANDLE_VALUE) {
        const DWORD err = GetLastError();
        if (err == ERROR_SHARING_VIOLATION || err == ERROR_LOCK_VIOLATION) {
            result.outcome = SpoolReplayOutcome::kTargetBusy;
            result.detail = "target busy";
        } else {
            result.outcome = SpoolReplayOutcome::kTargetUnreachable;
            result.detail = "target open error " + std::to_string(err);
        }
        return result;
    }
    HandleGuard targetGuard{target};

    LARGE_INTEGER targetSizeLi{};
    if (!GetFileSizeEx(target, &targetSizeLi) || targetSizeLi.QuadPart < 0) {
        result.outcome = SpoolReplayOutcome::kTargetUnreachable;
        result.detail = "target size query failed";
        return result;
    }
    const uint64_t currentTargetSize = static_cast<uint64_t>(targetSizeLi.QuadPart);
    if (currentTargetSize < header.baseFileSize) {
        result.outcome = SpoolReplayOutcome::kCorruptSpool;
        result.detail = "target file shorter than spool base offset";
        return result;
    }
    if (currentTargetSize > header.baseFileSize && !TruncateHandleAt(target, header.baseFileSize)) {
        result.outcome = SpoolReplayOutcome::kTargetUnreachable;
        result.detail = "target base truncate failed";
        return result;
    }

    std::vector<uint8_t> copyBuf(kReplayCopyChunkBytes);
    uint64_t spoolCursor = sizeof(SpoolFileHeader);
    uint64_t highestEndOffset = header.baseFileSize;

    while (spoolCursor + sizeof(SpoolRecordHeader) <= spoolFileSize) {
        if (cancel && *cancel != 0) {
            result.outcome = SpoolReplayOutcome::kCanceled;
            result.detail = "canceled";
            return result;
        }
        if (onProgress)
            onProgress();

        SpoolRecordHeader record{};
        size_t headerBytesRead = 0;
        if (!ReadExact(spool, &record, sizeof(record), &headerBytesRead)) {
            if (headerBytesRead == 0)
                break;
            break;  // unfinalized tail record
        }
        spoolCursor += sizeof(record);
        if (record.magic != kSpoolRecordMagic || spoolCursor + record.payloadBytes > spoolFileSize) {
            break;  // truncated tail record in unfinalized crash spool
        }
        if (!SeekHandle(target, record.targetOffset)) {
            result.outcome = SpoolReplayOutcome::kTargetUnreachable;
            result.detail = "target seek failed";
            return result;
        }

        uint32_t remaining = record.payloadBytes;
        while (remaining > 0) {
            if (cancel && *cancel != 0) {
                result.outcome = SpoolReplayOutcome::kCanceled;
                result.detail = "canceled";
                return result;
            }
            if (onProgress)
                onProgress();
            const size_t chunk = std::min<size_t>(remaining, copyBuf.size());
            if (!ReadExact(spool, copyBuf.data(), chunk)) {
                result.outcome = SpoolReplayOutcome::kCorruptSpool;
                result.detail = "spool payload read failed";
                return result;
            }
            if (!WriteAll(target, copyBuf.data(), chunk)) {
                result.outcome = SpoolReplayOutcome::kTargetUnreachable;
                result.detail = "target payload write failed";
                return result;
            }
            remaining -= static_cast<uint32_t>(chunk);
        }

        spoolCursor += record.payloadBytes;
        highestEndOffset = std::max(highestEndOffset, record.targetOffset + record.payloadBytes);
        ++result.replayedRecords;
        result.replayedBytes += record.payloadBytes;
    }

    const uint64_t desiredFinalSize = std::max(highestEndOffset, header.logicalFileSize);
    if (desiredFinalSize > highestEndOffset && !TruncateHandleAt(target, desiredFinalSize)) {
        result.outcome = SpoolReplayOutcome::kTargetUnreachable;
        result.detail = "target final size extend failed";
        return result;
    }
    if (onProgress)
        onProgress();
    if (!FlushFileBuffers(target)) {
        result.outcome = SpoolReplayOutcome::kTargetUnreachable;
        result.detail = "target flush failed";
        return result;
    }

    LARGE_INTEGER finalSizeLi{};
    if (GetFileSizeEx(target, &finalSizeLi) && finalSizeLi.QuadPart >= 0)
        result.finalFileSize = static_cast<uint64_t>(finalSizeLi.QuadPart);
    else
        result.finalFileSize = desiredFinalSize;

    CloseHandle(targetGuard.h);
    targetGuard.h = INVALID_HANDLE_VALUE;
    CloseHandle(spoolGuard.h);
    spoolGuard.h = INVALID_HANDLE_VALUE;
    DeleteFileW(spoolPath.c_str());

    result.outcome = SpoolReplayOutcome::kReplayed;
    return result;
}

}  // namespace

std::filesystem::path SpoolDirectoryForThisUser() {
    PWSTR localAppData = nullptr;
    std::filesystem::path directory;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_LocalAppData, KF_FLAG_DEFAULT, nullptr, &localAppData)) &&
        localAppData) {
        directory = std::filesystem::path(localAppData) / kProductFolder / L"spool";
    }
    if (localAppData)
        CoTaskMemFree(localAppData);
    return directory;
}

std::filesystem::path SpoolPathForStaging(const std::filesystem::path& stagingPath,
                                          const std::filesystem::path& spoolDirectory) {
    if (stagingPath.empty())
        return {};
    const std::filesystem::path dir = spoolDirectory.empty() ? SpoolDirectoryForThisUser() : spoolDirectory;
    if (dir.empty())
        return {};
    wchar_t name[32];
    swprintf(name, sizeof(name) / sizeof(name[0]), L"%016llx%ls",
             static_cast<unsigned long long>(PathHash(stagingPath)), kSpoolExtension);
    return dir / name;
}

bool HasSpoolForStaging(const std::filesystem::path& stagingPath, const std::filesystem::path& spoolDirectory) {
    const std::filesystem::path spoolPath = SpoolPathForStaging(stagingPath, spoolDirectory);
    if (spoolPath.empty())
        return false;
    std::error_code ec;
    return std::filesystem::is_regular_file(spoolPath, ec);
}

bool CanReplaySpoolWithoutExistingTarget(const std::filesystem::path& stagingPath,
                                         const std::filesystem::path& spoolDirectory) {
    const std::filesystem::path spoolPath = SpoolPathForStaging(stagingPath, spoolDirectory);
    if (spoolPath.empty())
        return false;
    HANDLE spool = CreateFileW(spoolPath.c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (spool == INVALID_HANDLE_VALUE)
        return false;
    SpoolFileHeader header{};
    const bool ok = ReadExact(spool, &header, sizeof(header)) && header.magic == kSpoolFileMagic &&
                    header.version == kSpoolFileVersion && header.baseFileSize == 0;
    CloseHandle(spool);
    return ok;
}

void RemoveSpoolForStaging(const std::filesystem::path& stagingPath, const std::filesystem::path& spoolDirectory) {
    const std::filesystem::path spoolPath = SpoolPathForStaging(stagingPath, spoolDirectory);
    if (!spoolPath.empty())
        DeleteFileW(spoolPath.c_str());
}

SpoolReplayResult ReplaySpoolForStaging(const std::filesystem::path& stagingPath,
                                        const std::filesystem::path& spoolDirectory,
                                        const volatile long* cancel,
                                        const std::function<void()>& onProgress) {
    return ReplaySpoolInternal(stagingPath, SpoolPathForStaging(stagingPath, spoolDirectory), 0, cancel, onProgress);
}

ResilientOutputSink::~ResilientOutputSink() {
    Discard(nullptr);
}

int ResilientOutputSink::Open(const std::filesystem::path& stagingPath, AVIOContext** pbOut, Options options) {
    Discard(pbOut);
    if (stagingPath.empty() || !pbOut)
        return AVERROR(EINVAL);

    stagingPath_ = stagingPath;
    options_ = std::move(options);
    if (options_.spoolDirectory.empty())
        options_.spoolDirectory = SpoolDirectoryForThisUser();
    spoolPath_ = SpoolPathForStaging(stagingPath_, options_.spoolDirectory);
    if (!spoolPath_.empty())
        DeleteFileW(spoolPath_.c_str());

    position_ = 0;
    targetPosition_ = 0;
    logicalSize_ = 0;
    confirmedTargetSize_ = 0;
    spooledPayloadBytes_ = 0;
    spooledRecordCount_ = 0;
    lastReconnectProbeMs_ = 0;
    spooling_ = false;
    usedSpool_ = false;
    inFinalizePhase_ = false;
    spoolExhausted_ = false;
    pendingSpool_ = false;

    targetHandle_ = CreateFileW(stagingPath_.c_str(), GENERIC_READ | GENERIC_WRITE,
                                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL,
                                nullptr);
    if (targetHandle_ == INVALID_HANDLE_VALUE) {
        const DWORD openErr = GetLastError();
        if (!BeginSpooling("open_target", openErr)) {
            return AVERROR(EIO);
        }
    }

    auto* buffer = static_cast<unsigned char*>(av_malloc(kAvioBufferSize));
    if (!buffer) {
        CloseHandles();
        return AVERROR(ENOMEM);
    }
    avioCtx_ = avio_alloc_context(buffer, kAvioBufferSize, 1, this, nullptr, &ResilientOutputSink::AvioWritePacket,
                                  &ResilientOutputSink::AvioSeek);
    if (!avioCtx_) {
        av_free(buffer);
        CloseHandles();
        return AVERROR(ENOMEM);
    }
    avioCtx_->seekable = AVIO_SEEKABLE_NORMAL;
    *pbOut = avioCtx_;
    open_ = true;
    return 0;
}

int ResilientOutputSink::Close(AVIOContext** pbInOut) {
    if (!open_) {
        FreeAvioContext(pbInOut);
        return 0;
    }

    inFinalizePhase_ = true;
    int flushError = 0;
    AVIOContext* pb = (pbInOut && *pbInOut) ? *pbInOut : avioCtx_;
    if (pb) {
        if (options_.onRefreshDeadline)
            options_.onRefreshDeadline();
        avio_flush(pb);
        flushError = pb->error;
    }
    FreeAvioContext(pbInOut);

    if (!spooling_) {
        CloseHandles();
        open_ = false;
        pendingSpool_ = false;
        return flushError < 0 ? flushError : 0;
    }

    if (spoolHandle_ != INVALID_HANDLE_VALUE) {
        WriteSpoolHeader(flushError >= 0);
        FlushFileBuffers(spoolHandle_);
        CloseHandle(spoolHandle_);
        spoolHandle_ = INVALID_HANDLE_VALUE;
    }
    CloseHandles();

    const SpoolReplayResult replay =
        ReplaySpoolInternal(stagingPath_, spoolPath_, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            options_.onRefreshDeadline);
    spooling_ = false;
    open_ = false;
    if (replay.outcome == SpoolReplayOutcome::kReplayed) {
        pendingSpool_ = false;
        DLL_Log("[OutputSpool] Replayed %llu spooled bytes (%llu records) onto '%s' during finalize",
                static_cast<unsigned long long>(replay.replayedBytes),
                static_cast<unsigned long long>(replay.replayedRecords), CollapsePath(stagingPath_).c_str());
    } else {
        pendingSpool_ = true;
        DLL_Log("[OutputSpool] WARN: Target '%s' still unreachable at finalize (%s); keeping finalized local spool "
                "'%s' (%llu bytes, %llu records) for automatic recovery",
                CollapsePath(stagingPath_).c_str(), replay.detail.c_str(), CollapsePath(spoolPath_).c_str(),
                static_cast<unsigned long long>(spooledPayloadBytes_),
                static_cast<unsigned long long>(spooledRecordCount_));
    }
    return flushError < 0 ? flushError : 0;
}

void ResilientOutputSink::Discard(AVIOContext** pbInOut) noexcept {
    FreeAvioContext(pbInOut);
    CloseHandles();
    if (!spoolPath_.empty())
        DeleteFileW(spoolPath_.c_str());
    open_ = false;
    spooling_ = false;
    pendingSpool_ = false;
}

int ResilientOutputSink::AvioWritePacket(void* opaque, const uint8_t* buf, int bufSize) {
    return static_cast<ResilientOutputSink*>(opaque)->WritePacket(buf, bufSize);
}

int64_t ResilientOutputSink::AvioSeek(void* opaque, int64_t offset, int whence) {
    return static_cast<ResilientOutputSink*>(opaque)->Seek(offset, whence);
}

int ResilientOutputSink::WritePacket(const uint8_t* buf, int bufSize) {
    if (bufSize <= 0)
        return 0;
    if (!buf)
        return AVERROR(EINVAL);
    if (options_.isAbortRequested && options_.isAbortRequested())
        return AVERROR_EXIT;

    if (!spooling_) {
        if (targetPosition_ != position_ && !SeekHandle(targetHandle_, position_)) {
            if (!BeginSpooling("seek_before_write", GetLastError()))
                return AVERROR(EIO);
        }
        if (!spooling_) {
            DWORD written = 0;
            if (WriteFile(targetHandle_, buf, static_cast<DWORD>(bufSize), &written, nullptr) &&
                written == static_cast<DWORD>(bufSize)) {
                position_ += static_cast<uint64_t>(bufSize);
                targetPosition_ = position_;
                logicalSize_ = std::max(logicalSize_, position_);
                confirmedTargetSize_ = logicalSize_;
                return bufSize;
            }
            if (!BeginSpooling("write_target", GetLastError()))
                return AVERROR(EIO);
        }
    }

    if (options_.onRefreshDeadline)
        options_.onRefreshDeadline();

    const uint64_t hardLimit = options_.maxLiveSpoolBytes + options_.finalizeReserveBytes;
    if (spooledPayloadBytes_ + static_cast<uint64_t>(bufSize) > hardLimit) {
        spoolExhausted_ = true;
        return AVERROR(ENOSPC);
    }

    const SpoolRecordHeader record{kSpoolRecordMagic, static_cast<uint32_t>(bufSize), position_};
    if (!WriteAll(spoolHandle_, &record, sizeof(record)) ||
        !WriteAll(spoolHandle_, buf, static_cast<size_t>(bufSize))) {
        return AVERROR(EIO);
    }

    position_ += static_cast<uint64_t>(bufSize);
    logicalSize_ = std::max(logicalSize_, position_);
    spooledPayloadBytes_ += static_cast<uint64_t>(bufSize);
    ++spooledRecordCount_;

    if (spooledPayloadBytes_ >= options_.maxLiveSpoolBytes && !spoolExhausted_) {
        spoolExhausted_ = true;
        DLL_Log("[OutputSpool] WARN: Local spool reached live budget (%llu bytes) for '%s'; requesting orderly stop so "
                "trailer can finalize into reserve",
                static_cast<unsigned long long>(spooledPayloadBytes_), CollapsePath(stagingPath_).c_str());
    }

    if (!inFinalizePhase_ && !spoolExhausted_ && options_.reconnectProbeIntervalMs > 0) {
        const uint64_t nowMs = GetTickCount64();
        if (nowMs - lastReconnectProbeMs_ >= options_.reconnectProbeIntervalMs) {
            lastReconnectProbeMs_ = nowMs;
            TryReplayAndResumeDirect();
        }
    }
    return bufSize;
}

int64_t ResilientOutputSink::Seek(int64_t offset, int whence) {
    const int mode = whence & ~AVSEEK_FORCE;
    if (mode == AVSEEK_SIZE)
        return static_cast<int64_t>(logicalSize_);

    int64_t target = -1;
    switch (mode) {
        case SEEK_SET:
            target = offset;
            break;
        case SEEK_CUR:
            target = static_cast<int64_t>(position_) + offset;
            break;
        case SEEK_END:
            target = static_cast<int64_t>(logicalSize_) + offset;
            break;
        default:
            return AVERROR(EINVAL);
    }
    if (target < 0)
        return AVERROR(EINVAL);

    if (!spooling_) {
        if (SeekHandle(targetHandle_, static_cast<uint64_t>(target))) {
            position_ = static_cast<uint64_t>(target);
            targetPosition_ = position_;
            return target;
        }
        if (!BeginSpooling("seek_target", GetLastError()))
            return AVERROR(EIO);
    }

    position_ = static_cast<uint64_t>(target);
    return target;
}

bool ResilientOutputSink::BeginSpooling(const char* operation, DWORD win32Error) {
    if (spooling_)
        return true;
    if (options_.onRefreshDeadline)
        options_.onRefreshDeadline();

    if (targetHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(targetHandle_);
        targetHandle_ = INVALID_HANDLE_VALUE;
    }
    if (spoolPath_.empty())
        return false;

    std::error_code ec;
    std::filesystem::create_directories(spoolPath_.parent_path(), ec);
    spoolHandle_ = CreateFileW(spoolPath_.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (spoolHandle_ == INVALID_HANDLE_VALUE) {
        DLL_Log("[OutputSpool] ERROR: Cannot create local spool '%s' after %s failed (targetError=%lu spoolError=%lu)",
                CollapsePath(spoolPath_).c_str(), operation ? operation : "io", static_cast<unsigned long>(win32Error),
                static_cast<unsigned long>(GetLastError()));
        return false;
    }

    spooling_ = true;
    usedSpool_ = true;
    spooledPayloadBytes_ = 0;
    spooledRecordCount_ = 0;
    lastReconnectProbeMs_ = GetTickCount64();
    if (!WriteSpoolHeader(false)) {
        CloseHandle(spoolHandle_);
        spoolHandle_ = INVALID_HANDLE_VALUE;
        DeleteFileW(spoolPath_.c_str());
        spooling_ = false;
        return false;
    }

    DLL_Log("[OutputSpool] WARN: Target I/O '%s' on '%s' failed (error=%lu at baseOffset=%llu); failing over to local "
            "spool '%s'",
            operation ? operation : "io", CollapsePath(stagingPath_).c_str(), static_cast<unsigned long>(win32Error),
            static_cast<unsigned long long>(confirmedTargetSize_), CollapsePath(spoolPath_).c_str());
    return true;
}

bool ResilientOutputSink::WriteSpoolHeader(bool finalized) {
    if (spoolHandle_ == INVALID_HANDLE_VALUE)
        return false;
    uint64_t previousPos = 0;
    SeekHandle(spoolHandle_, 0, FILE_CURRENT, &previousPos);
    if (!SeekHandle(spoolHandle_, 0, FILE_BEGIN))
        return false;
    const SpoolFileHeader header{
        kSpoolFileMagic,
        kSpoolFileVersion,
        confirmedTargetSize_,
        logicalSize_,
        finalized ? 1u : 0u,
        0u,
    };
    const bool ok = WriteAll(spoolHandle_, &header, sizeof(header));
    if (previousPos > sizeof(header))
        SeekHandle(spoolHandle_, previousPos, FILE_BEGIN);
    return ok;
}

void ResilientOutputSink::TryReplayAndResumeDirect() {
    if (!spooling_ || spoolHandle_ == INVALID_HANDLE_VALUE || stagingPath_.empty())
        return;

    const std::filesystem::path parent = stagingPath_.parent_path();
    if (!parent.empty() && GetFileAttributesW(parent.c_str()) == INVALID_FILE_ATTRIBUTES)
        return;

    WriteSpoolHeader(false);
    FlushFileBuffers(spoolHandle_);
    CloseHandle(spoolHandle_);
    spoolHandle_ = INVALID_HANDLE_VALUE;

    const SpoolReplayResult replay =
        ReplaySpoolInternal(stagingPath_, spoolPath_, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                            options_.onRefreshDeadline);
    if (replay.outcome == SpoolReplayOutcome::kReplayed) {
        HANDLE reopened = CreateFileW(stagingPath_.c_str(), GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING,
                                      FILE_ATTRIBUTE_NORMAL, nullptr);
        if (reopened != INVALID_HANDLE_VALUE && SeekHandle(reopened, position_)) {
            targetHandle_ = reopened;
            targetPosition_ = position_;
            confirmedTargetSize_ = logicalSize_;
            spooling_ = false;
            spooledPayloadBytes_ = 0;
            spooledRecordCount_ = 0;
            DLL_Log("[OutputSpool] Target '%s' reachable again; replayed %llu bytes (%llu records) and resumed direct "
                    "writing",
                    CollapsePath(stagingPath_).c_str(), static_cast<unsigned long long>(replay.replayedBytes),
                    static_cast<unsigned long long>(replay.replayedRecords));
            return;
        }
        if (reopened != INVALID_HANDLE_VALUE)
            CloseHandle(reopened);
        // Replay succeeded and deleted the previous spool, but the target dropped again before resuming direct writes.
        confirmedTargetSize_ = replay.finalFileSize;
        BeginSpooling("reopen_after_replay", GetLastError());
        return;
    }

    spoolHandle_ = CreateFileW(spoolPath_.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (spoolHandle_ != INVALID_HANDLE_VALUE)
        SeekHandle(spoolHandle_, 0, FILE_END);
}

void ResilientOutputSink::CloseHandles() noexcept {
    if (targetHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(targetHandle_);
        targetHandle_ = INVALID_HANDLE_VALUE;
    }
    if (spoolHandle_ != INVALID_HANDLE_VALUE) {
        CloseHandle(spoolHandle_);
        spoolHandle_ = INVALID_HANDLE_VALUE;
    }
}

void ResilientOutputSink::FreeAvioContext(AVIOContext** pbInOut) noexcept {
    AVIOContext* pb = (pbInOut && *pbInOut) ? *pbInOut : avioCtx_;
    if (pb) {
        av_freep(&pb->buffer);
        avio_context_free(&pb);
    }
    if (pbInOut)
        *pbInOut = nullptr;
    avioCtx_ = nullptr;
}

}  // namespace ce::mux
