#pragma once

#include <windows.h>

#include <cstddef>
#include <cstdint>
#include <vector>

namespace ce::hook_patch {

inline bool IsInstructionPointerInsidePatchRange(uintptr_t instructionPointer, uintptr_t patchStart,
                                                 size_t patchSize) {
    return patchSize > 0 && instructionPointer >= patchStart && instructionPointer - patchStart < patchSize;
}

// Why a quiescence transaction failed closed. The caller's decision is the same
// either way - it must not patch - but the reasons are not interchangeable
// diagnostically: an unstable snapshot is a transient race worth retrying, while
// an enumeration or suspend failure says something about the process that a
// retry will not change. Witcher 3 + NVIDIA Smooth Motion, session
// 20260919_182155, refused the Present body patch with all three folded into one
// message, so the log could not say which had happened.
enum class QuiesceFailure : uint8_t {
    kNone = 0,
    kAllocation,       // the thread vector could not be reserved
    kEnumeration,      // the thread walk did not complete
    kSuspend,          // a live peer thread could not be suspended
    kUnstableSnapshot, // peers kept appearing across every pass - transient
    kContext,          // a live peer thread's control context was unreadable
    kRangeUnsafe,      // a peer is executing inside the bytes about to change
};

inline const char* GetQuiesceFailureName(QuiesceFailure failure) {
    switch (failure) {
        case QuiesceFailure::kNone:
            return "none";
        case QuiesceFailure::kAllocation:
            return "allocation";
        case QuiesceFailure::kEnumeration:
            return "thread-enumeration";
        case QuiesceFailure::kSuspend:
            return "suspend";
        case QuiesceFailure::kUnstableSnapshot:
            return "unstable-thread-snapshot";
        case QuiesceFailure::kContext:
            return "thread-context";
        case QuiesceFailure::kRangeUnsafe:
            return "peer-executing-in-patch-range";
        default:
            return "unknown";
    }
}

// Only an unstable snapshot describes a condition that can differ on the very
// next attempt: peers were still being created while the walk ran. Everything
// else is a property of the process or of where its threads are parked.
inline bool IsRetryableQuiesceFailure(QuiesceFailure failure) {
    return failure == QuiesceFailure::kUnstableSnapshot || failure == QuiesceFailure::kRangeUnsafe;
}

// Suspends every existing peer thread and proves none is executing the bytes
// about to change. Construction can fail closed; destruction always resumes
// every thread successfully suspended by this transaction.
class ThreadQuiescence {
public:
    // Quiesce every peer thread for a group of patches. Each patch range must
    // still be checked with IsRangeSafe() before its bytes are changed.
    ThreadQuiescence();
    ThreadQuiescence(const void* patchAddress, size_t patchSize);
    ~ThreadQuiescence();

    ThreadQuiescence(const ThreadQuiescence&) = delete;
    ThreadQuiescence& operator=(const ThreadQuiescence&) = delete;

    bool IsReady() const {
        return ready_;
    }

    // Meaningful only while !IsReady().
    QuiesceFailure FailureReason() const {
        return failure_;
    }

    bool IsRangeSafe(const void* patchAddress, size_t patchSize) const;

private:
    void Quiesce();

    struct SuspendedThread {
        HANDLE handle = nullptr;
        DWORD threadId = 0;
        bool suspended = false;
        uintptr_t instructionPointer = 0;
        bool contextCaptured = false;
    };

    std::vector<SuspendedThread> threads_;
    bool ready_ = false;
    QuiesceFailure failure_ = QuiesceFailure::kNone;

    // Reported by the destructor, after every peer has resumed. Logging inside
    // the suspended window can deadlock: the logger takes a lock and may
    // allocate, and a suspended peer can be holding either one.
    uint64_t quiesceElapsedMs_ = 0;
    int quiescePasses_ = 0;
    bool usedSystemSnapshot_ = false;
};

}  // namespace ce::hook_patch
