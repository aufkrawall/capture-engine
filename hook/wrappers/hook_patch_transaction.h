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

    // Reported by the destructor, after every peer has resumed. Logging inside
    // the suspended window can deadlock: the logger takes a lock and may
    // allocate, and a suspended peer can be holding either one.
    uint64_t quiesceElapsedMs_ = 0;
    int quiescePasses_ = 0;
    bool usedSystemSnapshot_ = false;
};

}  // namespace ce::hook_patch
