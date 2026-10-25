#pragma once

#include <windows.h>

#include <cstdint>
#include <utility>
#include <vector>

namespace ce::third_party_load {

// RAII suspension of the threads owned by previously loaded tools. Their
// background threads must not hold their loader-hook mutexes/critical sections
// while CE's next tool DllMain runs under the loader lock (see
// third_party_load_policy.h). Threads are identified by their start address
// falling inside a loaded tool module, so game and driver threads are never
// suspended.
class ToolThreadSuspension {
public:
    // Loaded tool modules as {base, size}.
    using ModuleRanges = std::vector<std::pair<uintptr_t, uintptr_t>>;

    explicit ToolThreadSuspension(const ModuleRanges& moduleRanges) : moduleRanges_(moduleRanges) {}
    ~ToolThreadSuspension() { ResumeAll(); }
    ToolThreadSuspension(const ToolThreadSuspension&) = delete;
    ToolThreadSuspension& operator=(const ToolThreadSuspension&) = delete;

    bool HasModules() const { return !moduleRanges_.empty(); }

    // Suspends every tool thread. A thread that is exiting, or that another
    // owner already holds suspended at the maximum count, is stopped and left
    // alone. False when a running tool thread refused suspension or the walk
    // failed; everything suspended so far is resumed again in that case.
    bool Suspend();

    // Undoes exactly the suspensions this object made; Suspend() may run again.
    void ResumeAll();

    // Tool threads this object currently holds suspended.
    size_t SuspendedThreads() const;

    // Tool threads the last Suspend() found held by another owner.
    uint32_t HeldThreads() const { return heldThreads_; }

private:
    bool IsToolThread(const void* startAddress) const;

    struct ToolThread {
        HANDLE handle = nullptr;
        DWORD threadId = 0;
        bool suspended = false;
    };

    ModuleRanges moduleRanges_;
    std::vector<ToolThread> threads_;
    size_t processedCount_ = 0;
    uint32_t heldThreads_ = 0;
    DWORD lastHeldThreadId_ = 0;
};

}  // namespace ce::third_party_load
