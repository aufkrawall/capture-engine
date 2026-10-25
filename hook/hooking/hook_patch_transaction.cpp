#include "hook_patch_transaction.h"
#include "hook/runtime/hook_clock.h"

#include "hook/runtime/process_thread_walk.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstring>

void HookLog(const char* fmt, ...);

namespace ce::hook_patch {

namespace {
constexpr DWORD kQuiesceThreadAccess =
    THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION | SYNCHRONIZE;

DWORD SuspendPeerThread(HANDLE thread) {
    return SuspendThread(thread);
}

// "module+0xoffset" for a thread start address; called after every peer resumed.
void DescribeStartAddress(const void* address, char* out, size_t outSize) {
    HMODULE module = nullptr;
    char path[MAX_PATH] = {};
    if (address &&
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(address), &module) &&
        GetModuleFileNameA(module, path, sizeof(path))) {
        const char* base = strrchr(path, '\\');
        snprintf(out, outSize, "%s+0x%llX", base ? base + 1 : path,
                 static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(address) -
                                                 reinterpret_cast<uintptr_t>(module)));
        return;
    }
    snprintf(out, outSize, "%p", address);
}
}  // namespace

const PeerThreadOps& DefaultPeerThreadOps() {
    static const PeerThreadOps ops{&SuspendPeerThread, &ce::process_threads::QueryThreadExitState,
                                   &ce::process_threads::QueryThreadSuspendCount};
    return ops;
}

ThreadQuiescence::ThreadQuiescence(UnstableSnapshotPolicy unstablePolicy, const PeerThreadOps& ops) {
    Quiesce(unstablePolicy, ops);
}

ThreadQuiescence::ThreadQuiescence(const void* patchAddress, size_t patchSize,
                                   UnstableSnapshotPolicy unstablePolicy, const PeerThreadOps& ops) {
    if (!patchAddress || patchSize == 0)
        return;
    Quiesce(unstablePolicy, ops);
    if (ready_ && !IsRangeSafe(patchAddress, patchSize)) {
        ready_ = false;
        failure_ = QuiesceFailure::kRangeUnsafe;
    }
}

void ThreadQuiescence::Quiesce(UnstableSnapshotPolicy unstablePolicy, const PeerThreadOps& ops) {
    const DWORD currentThreadId = GetCurrentThreadId();
    const ULONGLONG enterMs = ce::hook_clock::TickCount64();
    try {
        threads_.reserve(1024);
    } catch (...) {
        failure_ = QuiesceFailure::kAllocation;
        return;
    }
    // Before anything is suspended: resolving the query can take a loader lock.
    ce::process_threads::PrepareThreadStateQueries();

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
            if (ops.suspend(thread.handle) == static_cast<DWORD>(-1)) {
                const DWORD suspendError = GetLastError();
                // The kernel refuses SuspendThread for two kinds of peer that are not
                // running: one that has started exiting, and one another owner holds
                // at the 127 maximum (ERROR_SIGNAL_REFUSED). Heroes of the Storm keeps
                // one of its own threads there; treating that as a live refusal gave
                // up every patch of a late injection (20261025_230718: tid 8976,
                // error 156, Present hooked only when the DX11 route won a race).
                if (ClassifyRefusal(thread, ops) != RefusalVerdict::kLive)
                    continue;
                refused_ = {thread.threadId, suspendError, ce::process_threads::QueryThreadStartAddress(thread.handle)};
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
    if (!ConfirmHeldPeersStillStopped(ops))
        return;

    // A grouped transaction keeps every tracked thread suspended throughout,
    // so its control context cannot change between related entry patches.
    // Capture it once instead of issuing one GetThreadContext call per thread
    // per target.
    for (auto& thread : threads_) {
        if (!thread.handle || thread.departed)
            continue;
        CONTEXT context = {};
        context.ContextFlags = CONTEXT_CONTROL;
        if (!GetThreadContext(thread.handle, &context)) {
            // Terminated after it was suspended: as above, it never runs user code again.
            if (ops.exitState(thread.handle) != ce::process_threads::ThreadExitState::kRunning)
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
    quiesceElapsedMs_ = ce::hook_clock::TickCount64() - enterMs;
    quiescePasses_ = passesRun;
    usedSystemSnapshot_ = usedSystemSnapshot;
}

// Decides a peer that refused SuspendThread from the kernel's view of it. Runs
// while peers are suspended: no allocation, no lock, no logging.
ThreadQuiescence::RefusalVerdict ThreadQuiescence::ClassifyRefusal(SuspendedThread& thread,
                                                                   const PeerThreadOps& ops) {
    // An exiting thread is refused from the moment it starts exiting until its
    // handle is signaled, and it never runs user code again.
    const auto exitState = ops.exitState(thread.handle);
    if (exitState != ce::process_threads::ThreadExitState::kRunning) {
        if (exitState == ce::process_threads::ThreadExitState::kExiting) {
            ++exitingPeersSkipped_;
            lastExitingPeerId_ = thread.threadId;
            lastExitingPeerStart_ = ce::process_threads::QueryThreadStartAddress(thread.handle);
        }
        // Stays tracked with its handle open: the next pass must not rediscover it
        // as a new peer (four passes of that read as an unstable snapshot), and an
        // open handle keeps its thread id from being reused within this transaction.
        thread.departed = true;
        return RefusalVerdict::kDeparted;
    }
    // Already suspended by another owner: stopped exactly like a peer this
    // transaction suspended, except that its suspension is not ours to undo.
    // Tracked with its handle open for the same reasons as above.
    DWORD suspendCount = 0;
    if (ops.suspendCount(thread.handle, &suspendCount) && suspendCount > 0) {
        thread.held = true;
        lastHeldPeerId_ = thread.threadId;
        lastHeldPeerCount_ = suspendCount;
        lastHeldPeerStart_ = ce::process_threads::QueryThreadStartAddress(thread.handle);
        return RefusalVerdict::kHeld;
    }
    return RefusalVerdict::kLive;
}

// A held peer's owner may have resumed it before the owner itself was
// suspended. With every other peer suspended nothing in this process can
// resume it any more, so what it is now holds for the whole transaction.
bool ThreadQuiescence::ConfirmHeldPeersStillStopped(const PeerThreadOps& ops) {
    uint32_t held = 0;
    for (auto& thread : threads_) {
        if (!thread.held)
            continue;
        thread.held = false;
        if (ops.suspend(thread.handle) != static_cast<DWORD>(-1)) {
            thread.suspended = true;  // released meanwhile: now ours to resume like any other peer
            continue;
        }
        const DWORD suspendError = GetLastError();
        switch (ClassifyRefusal(thread, ops)) {
            case RefusalVerdict::kDeparted:
                break;
            case RefusalVerdict::kHeld:
                ++held;
                break;
            case RefusalVerdict::kLive:
                refused_ = {thread.threadId, suspendError,
                            ce::process_threads::QueryThreadStartAddress(thread.handle)};
                failure_ = QuiesceFailure::kSuspend;
                return false;
        }
    }
    heldPeers_ = held;
    return true;
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

    // Peers that refused suspension are named now, with every peer running
    // again: the module lookup takes loader locks.
    if (failure_ == QuiesceFailure::kSuspend && refused_.threadId != 0) {
        static std::atomic<uint32_t> s_refusedPeerLogs{0};
        const uint32_t index = s_refusedPeerLogs.fetch_add(1, std::memory_order_relaxed);
        if (index < 8 || (index % 64) == 0) {
            char start[MAX_PATH + 32] = {};
            DescribeStartAddress(refused_.startAddress, start, sizeof(start));
            HookLog("ThreadQuiescence: live peer tid=%lu refused suspension (error=%lu start=%s, #%u) - the patch is "
                    "refused; it was neither exiting nor held suspended by another owner",
                    static_cast<unsigned long>(refused_.threadId), static_cast<unsigned long>(refused_.error), start,
                    index + 1);
        }
    }
    if (heldPeers_ != 0) {
        static std::atomic<uint32_t> s_heldPeerLogs{0};
        const uint32_t index = s_heldPeerLogs.fetch_add(1, std::memory_order_relaxed);
        if (index < 8 || (index % 64) == 0) {
            char start[MAX_PATH + 32] = {};
            DescribeStartAddress(lastHeldPeerStart_, start, sizeof(start));
            HookLog("ThreadQuiescence: %u peer(s) already held suspended by another owner (last tid=%lu count=%lu "
                    "start=%s, #%u) - range-checked as stopped, left suspended; ready=%d",
                    heldPeers_, static_cast<unsigned long>(lastHeldPeerId_),
                    static_cast<unsigned long>(lastHeldPeerCount_), start, index + 1, ready_ ? 1 : 0);
        }
    }
    if (exitingPeersSkipped_ != 0) {
        static std::atomic<uint32_t> s_exitingPeerLogs{0};
        const uint32_t index = s_exitingPeerLogs.fetch_add(1, std::memory_order_relaxed);
        if (index < 8 || (index % 64) == 0) {
            char start[MAX_PATH + 32] = {};
            DescribeStartAddress(lastExitingPeerStart_, start, sizeof(start));
            HookLog("ThreadQuiescence: skipped %u exiting peer(s) the kernel would not suspend (last tid=%lu start=%s, "
                    "#%u) - they never run user code again; ready=%d",
                    exitingPeersSkipped_, static_cast<unsigned long>(lastExitingPeerId_), start, index + 1,
                    ready_ ? 1 : 0);
        }
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
