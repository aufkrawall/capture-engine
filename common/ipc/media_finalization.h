#pragma once

// Media finalization result: how a disposable media child tells whoever spawned it what became of
// its recording. The controller side only learns "stop accepted, finalization asynchronous"; the
// child then finalizes on its own and exits. This is the package-private contract that carries the
// outcome back, and it deliberately does not depend on logging: with log_level=none there is no
// logs directory and no recording manifest, yet the outcome and output path must still arrive.
//
// Two channels, both always available:
//  1. The child's process exit code. It carries the status and the degraded flags, survives a
//     missing or unmappable slot, and is the only channel a crashed child can leave behind.
//  2. A result slot: a small anonymous file mapping the spawner creates per child and the child
//     inherits (`--media-result-handle=`), the same shape as the session latency channel. It adds
//     the published output path(s), which an exit code cannot carry. It is anonymous and inherited,
//     so nothing outside the spawned child can write it, and it is one slot per child, so a retired
//     child's result can never be mistaken for the next recording's.
//
// The layout and the pure logic below are the cross-process contract, header-only so they are
// unit-testable and consumable without including anything from captureengine/**. The Win32
// create/map wrappers live in media_finalization_slot.cpp.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace ce::media_result {

enum class Status : uint32_t {
    kNone = 0,      // no recording finalization ran (idle child shut down, or never finished a recording)
    kSaved = 1,     // an output was published; paths name it unless kFlagLiveStream / kFlagPathOmitted
    kFailed = 2,    // finalization ran and published no output
    kCanceled = 3,  // stop before the recording went live: nothing was captured, no output exists
    kLost = 4,      // spawner-side only: the child ended without publishing any result (crash, kill, init failure)
};

inline constexpr uint32_t kFlagVideoDegraded = 1u << 0;  // the published file lost video content or metadata
inline constexpr uint32_t kFlagAudioDegraded = 1u << 1;  // the published file lost audio content
inline constexpr uint32_t kFlagLiveStream = 1u << 2;     // the output was a live stream: no file, never a path
inline constexpr uint32_t kFlagPathOmitted = 1u << 3;    // an output exists but its path is unknown or did not fit

inline const char* StatusName(Status status) {
    switch (status) {
        case Status::kNone:
            return "none";
        case Status::kSaved:
            return "saved";
        case Status::kFailed:
            return "failed";
        case Status::kCanceled:
            return "canceled";
        case Status::kLost:
            return "lost";
    }
    return "unknown";
}

// What one finalization produced, as the spawner consumes it.
struct Result {
    Status status = Status::kNone;
    uint32_t flags = 0;
    // Finalizations the child published. More than one means earlier results were overwritten; a child
    // that never published reads as zero and Resolve() then falls back to the exit code.
    uint32_t finalizationCount = 0;
    std::vector<std::string> paths;  // UTF-8, published output file(s) in a stable order
};

// ---- Exit code ---------------------------------------------------------------------------------

// 0 was and stays "the loop ended without a finalization"; 1 was and stays "initialization failed"
// (it is also the code the controller's forced TerminateProcess uses, so it never names an outcome).
// A coded result sets a magic in the high half so it cannot be mistaken for an OS crash code
// (0xC.......) or for a TerminateProcess value.
inline constexpr uint32_t kExitCodeInitFailed = 1u;
inline constexpr uint32_t kExitCodeMagic = 0x43450000u;  // 'C' 'E'
inline constexpr uint32_t kExitCodeMagicMask = 0xFFFF0000u;
inline constexpr uint32_t kExitCodeFlagsShift = 8;

enum class ExitKind : uint8_t {
    kClean,     // 0: the child ended normally with no finalization
    kResult,    // a coded finalization result
    kAbnormal,  // anything else: init failure, forced termination, a crash
};

struct DecodedExit {
    ExitKind kind = ExitKind::kAbnormal;
    Status status = Status::kLost;
    uint32_t flags = 0;
};

inline uint32_t EncodeExitCode(Status status, uint32_t flags) {
    switch (status) {
        case Status::kNone:
            return 0u;
        case Status::kSaved:
        case Status::kFailed:
        case Status::kCanceled:
            return kExitCodeMagic | ((flags & 0xFFu) << kExitCodeFlagsShift) | static_cast<uint32_t>(status);
        case Status::kLost:
        default:
            return kExitCodeInitFailed;  // kLost is the spawner's verdict about a child that cannot say it
    }
}

inline DecodedExit DecodeExitCode(uint32_t exitCode) {
    if (exitCode == 0u)
        return {ExitKind::kClean, Status::kNone, 0u};
    if ((exitCode & kExitCodeMagicMask) == kExitCodeMagic) {
        const uint32_t status = exitCode & 0xFFu;
        if (status >= static_cast<uint32_t>(Status::kSaved) && status <= static_cast<uint32_t>(Status::kCanceled))
            return {ExitKind::kResult, static_cast<Status>(status), (exitCode >> kExitCodeFlagsShift) & 0xFFu};
    }
    return {ExitKind::kAbnormal, Status::kLost, 0u};
}

// ---- The engine's output path ------------------------------------------------------------------

// Copies `path` into a caller buffer under the MediaEngine_GetLastOutputPath contract: returns the
// length in bytes without the NUL, and writes the path plus its NUL only when both fit, so a call with
// (nullptr, 0) sizes the buffer and a too-small buffer is never partially filled.
inline uint32_t CopyOutputPath(const std::string& path, char* buffer, uint32_t capacity) {
    const uint32_t length = static_cast<uint32_t>(path.size() < 0xFFFFFFFFu ? path.size() : 0xFFFFFFFEu);
    if (buffer && capacity > length) {
        std::memcpy(buffer, path.data(), length);
        buffer[length] = '\0';
    }
    return length;
}

// ---- What a finalization means -----------------------------------------------------------------

// The media child's facts at the end of a recording, before they become a Result.
struct FinalizationInputs {
    bool canceled = false;     // the stop arrived before the recording went live
    bool outputSaved = false;  // the media engine published an output
    bool liveStream = false;   // the output target was a stream endpoint, not a file
    bool videoDegraded = false;
    bool audioDegraded = false;
    std::string outputPath;  // UTF-8 path the engine published; may be empty
};

// A published file is reported as saved even when the stop was a cancel request that arrived after the
// first frame committed (the engine then keeps the recording): what exists on disk decides, not the
// request. A live stream never reports a path, so an endpoint (and the key inside it) cannot leak.
inline Result BuildResult(const FinalizationInputs& inputs, uint32_t previousCount) {
    Result result;
    result.status = inputs.outputSaved ? Status::kSaved : (inputs.canceled ? Status::kCanceled : Status::kFailed);
    if (inputs.videoDegraded && inputs.outputSaved)
        result.flags |= kFlagVideoDegraded;
    if (inputs.audioDegraded && inputs.outputSaved)
        result.flags |= kFlagAudioDegraded;
    if (inputs.liveStream)
        result.flags |= kFlagLiveStream;
    if (inputs.outputSaved && !inputs.liveStream) {
        if (inputs.outputPath.empty())
            result.flags |= kFlagPathOmitted;
        else
            result.paths.push_back(inputs.outputPath);
    }
    result.finalizationCount = previousCount + 1;
    return result;
}

// ---- Result slot -------------------------------------------------------------------------------

inline constexpr uint32_t kSlotMagic = 0x52464543;  // "CEFR"
inline constexpr uint32_t kSlotVersion = 1;
inline constexpr size_t kMaxPaths = 8;
// NUL-terminated UTF-8 paths, packed back to back. Windows allows 32767 UTF-16 units but real output
// paths are far shorter; one that cannot fit intact is omitted, never truncated, because a truncated
// path names a different file.
inline constexpr size_t kPathCapacity = 4000;

struct Slot {
    uint32_t magic;
    uint32_t version;
    uint32_t structSize;
    uint32_t reserved0;
    std::atomic<uint64_t> sequence;  // even = stable, odd = write in progress
    uint32_t finalizationCount;      // 0 = the child never published
    uint32_t status;                 // Status
    uint32_t flags;
    uint32_t pathCount;
    uint32_t pathBytes;  // bytes used in `paths`, NULs included
    uint32_t reserved1;
    char paths[kPathCapacity];
};

static_assert(std::atomic<uint64_t>::is_always_lock_free,
              "the result slot sequence must be lock-free to be shared across processes");
static_assert(sizeof(Slot) <= 4096, "the result slot must stay within one page");

inline void InitSlot(Slot& slot) {
    std::memset(slot.paths, 0, sizeof(slot.paths));
    slot.magic = kSlotMagic;
    slot.version = kSlotVersion;
    slot.structSize = static_cast<uint32_t>(sizeof(Slot));
    slot.reserved0 = 0;
    slot.finalizationCount = 0;
    slot.status = static_cast<uint32_t>(Status::kNone);
    slot.flags = 0;
    slot.pathCount = 0;
    slot.pathBytes = 0;
    slot.reserved1 = 0;
    slot.sequence.store(0, std::memory_order_release);
}

inline bool IsSlotCompatible(const Slot& slot) {
    return slot.magic == kSlotMagic && slot.version == kSlotVersion &&
           slot.structSize == static_cast<uint32_t>(sizeof(Slot));
}

// Child side. Returns true when the result was stored intact. A path list that cannot be stored intact
// (too many, too long, empty or NUL-bearing entries) is dropped whole and kFlagPathOmitted is set, so
// the status and flags still arrive; the return is then false so the caller can log it.
inline bool PublishResult(Slot& slot, const Result& result) {
    if (!IsSlotCompatible(slot))
        return false;

    bool intact = result.paths.size() <= kMaxPaths;
    size_t bytes = 0;
    for (const std::string& path : result.paths) {
        if (path.empty() || path.find('\0') != std::string::npos) {
            intact = false;
            break;
        }
        bytes += path.size() + 1;
    }
    intact = intact && bytes <= kPathCapacity;

    uint32_t flags = result.flags;
    if (!intact)
        flags |= kFlagPathOmitted;

    slot.sequence.fetch_add(1, std::memory_order_acq_rel);  // -> odd: write in progress
    std::atomic_thread_fence(std::memory_order_release);
    std::memset(slot.paths, 0, sizeof(slot.paths));
    uint32_t pathCount = 0;
    size_t offset = 0;
    if (intact) {
        for (const std::string& path : result.paths) {
            std::memcpy(slot.paths + offset, path.data(), path.size());
            offset += path.size() + 1;
            ++pathCount;
        }
    }
    slot.status = static_cast<uint32_t>(result.status);
    slot.flags = flags;
    slot.pathCount = pathCount;
    slot.pathBytes = static_cast<uint32_t>(intact ? offset : 0);
    slot.finalizationCount = result.finalizationCount;
    std::atomic_thread_fence(std::memory_order_release);
    slot.sequence.fetch_add(1, std::memory_order_acq_rel);  // -> even: stable again
    return intact;
}

// Spawner side. False when the slot is incompatible, was never published, or raced a write; the caller
// then falls back to the exit code (a later read can succeed, so a race is not a verdict).
inline bool ReadResult(const Slot& slot, Result& out) {
    const uint64_t before = slot.sequence.load(std::memory_order_acquire);
    if ((before & 1ULL) != 0ULL || !IsSlotCompatible(slot))
        return false;

    Result copy;
    copy.finalizationCount = slot.finalizationCount;
    const uint32_t status = slot.status;
    copy.flags = slot.flags;
    const uint32_t pathCount = slot.pathCount;
    const uint32_t pathBytes = slot.pathBytes;
    char paths[kPathCapacity];
    std::memcpy(paths, slot.paths, sizeof(paths));
    std::atomic_thread_fence(std::memory_order_acquire);
    if (slot.sequence.load(std::memory_order_acquire) != before)
        return false;  // raced with a write

    if (copy.finalizationCount == 0)
        return false;
    if (status < static_cast<uint32_t>(Status::kNone) || status > static_cast<uint32_t>(Status::kCanceled))
        return false;
    copy.status = static_cast<Status>(status);
    if (pathCount > kMaxPaths || pathBytes > kPathCapacity)
        return false;

    size_t offset = 0;
    for (uint32_t i = 0; i < pathCount; ++i) {
        if (offset >= pathBytes)
            return false;
        const void* end = std::memchr(paths + offset, '\0', pathBytes - offset);
        if (!end)
            return false;
        const size_t length = static_cast<size_t>(static_cast<const char*>(end) - (paths + offset));
        if (length == 0)
            return false;
        copy.paths.emplace_back(paths + offset, length);
        offset += length + 1;
    }
    if (offset != pathBytes)
        return false;
    out = std::move(copy);
    return true;
}

// ---- Resolution --------------------------------------------------------------------------------

struct Resolution {
    Result result;
    bool fromSlot = false;         // status, flags and paths come from the slot (authoritative)
    bool abnormalExit = false;     // the process ended with a code that is neither 0 nor a coded result
    bool statusDisagreed = false;  // slot and exit code both spoke and named different outcomes
};

// Combines the two channels once the child has exited. The slot wins when it holds a result: it is the
// only channel with the path, and what the child published is true even if the process then crashed
// during teardown. Without a slot result the exit code decides; a coded result there has no path, so a
// saved output is then marked kFlagPathOmitted. A child that exited with neither is simply lost.
inline Resolution Resolve(uint32_t exitCode, const Result* slotResult) {
    Resolution resolution;
    const DecodedExit exit = DecodeExitCode(exitCode);
    resolution.abnormalExit = exit.kind == ExitKind::kAbnormal;

    if (slotResult && slotResult->finalizationCount > 0) {
        resolution.result = *slotResult;
        resolution.fromSlot = true;
        resolution.statusDisagreed = exit.kind == ExitKind::kResult && exit.status != slotResult->status;
        return resolution;
    }

    resolution.result.status = exit.status;
    resolution.result.flags = exit.flags;
    if (exit.kind == ExitKind::kResult && exit.status == Status::kSaved &&
        (exit.flags & kFlagLiveStream) == 0)
        resolution.result.flags |= kFlagPathOmitted;
    return resolution;
}

// ---- Win32 mapping (media_finalization_slot.cpp) ------------------------------------------------

// Spawner side: one slot per media child. Anonymous, so nothing outside the spawned child can open it.
class ResultSlot {
public:
    // nullptr when the mapping cannot be created; the child then reports through its exit code alone.
    static std::unique_ptr<ResultSlot> Create();
    ~ResultSlot();
    ResultSlot(const ResultSlot&) = delete;
    ResultSlot& operator=(const ResultSlot&) = delete;

    // The handle to hand the child. Created non-inheritable; SpawnChildProcess makes it inheritable only
    // for the duration of the launch. Valid for the life of this object.
    void* ChildHandle() const;
    // The child's published result, or false when it published none (yet).
    bool Read(Result& out) const;

private:
    ResultSlot() = default;
    void* mapping_ = nullptr;
    Slot* slot_ = nullptr;
};

// Child side: map the slot handle inherited from the spawner. Takes ownership of the handle and
// closes it either way (the view outlives it). nullptr when the handle is absent or the slot is not
// compatible. The pointer stays valid until UnmapInheritedSlot.
Slot* MapInheritedSlot(void* inheritedHandle);
void UnmapInheritedSlot(Slot* slot);

}  // namespace ce::media_result
