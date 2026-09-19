#include "hook_patch_transaction.h"

#include "../common/process_thread_walk.h"

#include <algorithm>
#include <atomic>

void HookLog(const char* fmt, ...);

namespace ce::hook_patch {

namespace {
constexpr DWORD kQuiesceThreadAccess =
    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | SYNCHRONIZE;
}  // namespace

ThreadQuiescence::ThreadQuiescence(UnstableSnapshotPolicy unstablePolicy) {
    Quiesce(unstablePolicy);
}

ThreadQuiescence::ThreadQuiescence(const void* patchAddress, size_t patchSize,
                                   UnstableSnapshotPolicy unstablePolicy) {
    if (!patchAddress || patchSize == 0)
        return;
    Quiesce(unstablePolicy);
    if (ready_ && !IsRangeSafe(patchAddress, patchSize)) {
        ready_ = false;
        failure_ = QuiesceFailure::kRangeUnsafe;
    }
}

void ThreadQuiescence::Quiesce(UnstableSnapshotPolicy unstablePolicy) {
    const DWORD currentThreadId = GetCurrentThreadId();
    const ULONGLONG enterMs = GetTickCount64();
    try {
        threads_.reserve(1024);
    } catch (...) {
        failure_ = QuiesceFailure::kAllocation;
        return;
    }

    bool usedSystemSnapshot = false;
    bool stableSnapshot = false;
    int passesRun = 0;
    for (int pass = 0; pass < 4; ++pass) {
        ++passesRun;
        const size_t previousCount = threads_.size();

        // Adopt one peer thread. Returning false aborts the walk, which fails
        // the transaction closed exactly as an enumeration error does.
        auto adopt = [&](HANDLE thread, DWORD threadId) -> bool {
            const bool tracked = std::any_of(threads_.begin(), threads_.end(), [&](const SuspendedThread& known) {
                return known.handle && known.threadId == threadId;
            });
            if (tracked) {
                CloseHandle(thread);
                return true;
            }
            if (threads_.size() == threads_.capacity()) {
                CloseHandle(thread);
                return false;
            }
            threads_.push_back({thread, threadId, false, 0, false});
            return true;
        };

        ce::process_threads::WalkResult walk =
            ce::process_threads::WalkCurrentProcessThreads(currentThreadId, kQuiesceThreadAccess, adopt);
        if (walk == ce::process_threads::WalkResult::kUnavailable) {
            usedSystemSnapshot = true;
            walk = ce::process_threads::WalkCurrentProcessThreadsViaSystemSnapshot(currentThreadId,
                                                                                   kQuiesceThreadAccess, adopt);
        }
        if (walk != ce::process_threads::WalkResult::kCompleted) {
            failure_ = QuiesceFailure::kEnumeration;
            return;
        }

        for (size_t i = previousCount; i < threads_.size(); ++i) {
            auto& thread = threads_[i];
            if (SuspendThread(thread.handle) == static_cast<DWORD>(-1)) {
                if (WaitForSingleObject(thread.handle, 0) == WAIT_OBJECT_0) {
                    CloseHandle(thread.handle);
                    thread.handle = nullptr;
                    continue;
                }
                failure_ = QuiesceFailure::kSuspend;
                return;
            }
            thread.suspended = true;
        }
        if (threads_.size() == previousCount) {
            stableSnapshot = true;
            break;
        }
    }
    if (!stableSnapshot) {
        // Peers were still being created on every pass. NvPresent64 spawns its
        // pacer/interpolation/capture workers exactly while CE is installing the
        // Present body hook, which is what made this the observed failure under
        // Smooth Motion.
        if (unstablePolicy != UnstableSnapshotPolicy::kAcceptSuspendedSet) {
            failure_ = QuiesceFailure::kUnstableSnapshot;
            return;
        }
        // Proceed on the set actually suspended. IsRangeSafe() below still has to
        // prove none of them is executing the bytes about to change; what is given
        // up is only the guarantee that no FURTHER thread exists, which a thread
        // created after the final walk breaks in the stable case too.
        acceptedUnstableSnapshot_ = true;
    }

    // A grouped transaction keeps every tracked thread suspended throughout,
    // so its control context cannot change between related entry patches.
    // Capture it once instead of issuing one GetThreadContext call per thread
    // per target.
    for (auto& thread : threads_) {
        if (!thread.handle)
            continue;
        CONTEXT context = {};
        context.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(thread.handle, &context)) {
            if (WaitForSingleObject(thread.handle, 0) == WAIT_OBJECT_0)
                continue;
            failure_ = QuiesceFailure::kContext;
            return;
        }
#ifdef _WIN64
        thread.instructionPointer = static_cast<uintptr_t>(context.Rip);
#else
        thread.instructionPointer = static_cast<uintptr_t>(context.Eip);
#endif
        thread.contextCaptured = true;
    }
    ready_ = true;
    failure_ = QuiesceFailure::kNone;

    // Measure here, report from the destructor. Nothing on this path may log:
    // the logger takes a lock and can allocate, and a suspended peer thread may
    // be holding either.
    quiesceElapsedMs_ = GetTickCount64() - enterMs;
    quiescePasses_ = passesRun;
    usedSystemSnapshot_ = usedSystemSnapshot;
}

bool ThreadQuiescence::IsRangeSafe(const void* patchAddress, size_t patchSize) const {
    if (!ready_ || !patchAddress || patchSize == 0)
        return false;

    const uintptr_t patchStart = reinterpret_cast<uintptr_t>(patchAddress);
    for (const auto& thread : threads_) {
        if (!thread.contextCaptured)
            continue;
        if (IsInstructionPointerInsidePatchRange(thread.instructionPointer, patchStart, patchSize))
            return false;
    }
    return true;
}

ThreadQuiescence::~ThreadQuiescence() {
    const size_t suspendedCount = threads_.size();
    for (auto it = threads_.rbegin(); it != threads_.rend(); ++it) {
        if (it->suspended)
            ResumeThread(it->handle);
        if (it->handle)
            CloseHandle(it->handle);
    }

    // Every peer thread was frozen for the quiescence window, so a slow one is
    // a whole-process stall that is otherwise invisible - the hook log only
    // shows an unexplained gap between the trampoline write and the entry
    // patch. Report the outliers, bounded, with the route that produced them,
    // now that every peer is running again.
    if (quiesceElapsedMs_ >= 8) {
        static std::atomic<uint32_t> s_slowQuiescenceLogs{0};
        const uint32_t index = s_slowQuiescenceLogs.fetch_add(1, std::memory_order_relaxed);
        if (index < 8 || (index % 64) == 0) {
            HookLog(
                "ThreadQuiescence: %zu peer thread(s) suspended for %llu ms over %d pass(es) route=%s (#%u) - the "
                "whole process was frozen for that window",
                suspendedCount, static_cast<unsigned long long>(quiesceElapsedMs_), quiescePasses_,
                usedSystemSnapshot_ ? "system-snapshot" : "process-scoped", index + 1);
        }
    }
}

}  // namespace ce::hook_patch
