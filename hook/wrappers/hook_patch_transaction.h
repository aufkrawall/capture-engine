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
    ThreadQuiescence(const void* patchAddress, size_t patchSize);
    ~ThreadQuiescence();

    ThreadQuiescence(const ThreadQuiescence&) = delete;
    ThreadQuiescence& operator=(const ThreadQuiescence&) = delete;

    bool IsReady() const {
        return ready_;
    }

private:
    struct SuspendedThread {
        HANDLE handle = nullptr;
        DWORD threadId = 0;
        bool suspended = false;
    };

    std::vector<SuspendedThread> threads_;
    bool ready_ = false;
};

}  // namespace ce::hook_patch
