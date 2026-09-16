#pragma once

#include <windows.h>

#include <type_traits>

// Enumerating this process's own threads.
//
// The historical walk was CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0).
// That flag ignores the process id it is given and always builds a snapshot of
// EVERY thread on the machine, so its cost tracks total system load, not this
// process. CE pays it inside ThreadQuiescence, once per inline-hook entry
// patch, while every peer thread in the game is suspended - measured at
// 37-533 ms per patch and 279-1249 ms per launch across the Gothic II sessions
// in 20260916_*, with a 5x spread between runs doing identical work. Right
// after a boot, when system thread count is highest and most volatile, it is
// worst.
//
// NtGetNextThread walks one process, so the cost is proportional to this
// process's thread count and nothing else. It is available from Windows 8.1
// onward; the system snapshot remains the fallback so coverage is never a
// subset of the process's threads.
namespace ce::process_threads {

enum class WalkResult {
    // Every thread of this process was visited.
    kCompleted,
    // This walk cannot run here; the caller should use the other one.
    kUnavailable,
    // The visitor asked to stop, or the walk could not be completed. The
    // caller must treat the thread set as incomplete.
    kAborted,
};

// Visits every thread of the current process except `excludeThreadId`, which
// callers set to GetCurrentThreadId() so a transaction never suspends itself.
//
// Each visit receives an owned handle opened with `desiredAccess`; the visitor
// takes ownership and must CloseHandle it. Returning false aborts the walk
// (the handle passed to that final call is still the visitor's to close).
//
// A thread that exits mid-walk is skipped, not an error: the set is always a
// snapshot of a live process.
//
// Deliberately a non-owning callable view rather than std::function: a walk
// that needs a second pass runs with peer threads already suspended, and one
// of them may hold the heap lock. Nothing on this path may allocate.
class ThreadVisitor {
public:
    template <typename Fn, typename = std::enable_if_t<
                               !std::is_same_v<std::decay_t<Fn>, ThreadVisitor>>>
    // NOLINTNEXTLINE(bugprone-forwarding-reference-overload) - constrained above; this is a
    // function-view constructor, and the referent outlives the call by full-expression lifetime.
    ThreadVisitor(Fn&& fn) noexcept
        : context_(static_cast<void*>(&fn)),
          invoke_([](void* context, HANDLE handle, DWORD threadId) -> bool {
              return (*static_cast<std::remove_reference_t<Fn>*>(context))(handle, threadId);
          }) {}

    bool operator()(HANDLE threadHandle, DWORD threadId) const {
        return invoke_(context_, threadHandle, threadId);
    }

private:
    void* context_ = nullptr;
    bool (*invoke_)(void*, HANDLE, DWORD) = nullptr;
};

// Process-scoped walk. Returns kUnavailable when NtGetNextThread cannot serve
// this process, which latches for the process lifetime so a partial walk can
// never silently reduce thread coverage on a later transaction.
WalkResult WalkCurrentProcessThreads(DWORD excludeThreadId, ACCESS_MASK desiredAccess, const ThreadVisitor& visit);

// The system-wide snapshot, filtered to this process. Correct but expensive;
// kept as the fallback and as the equivalence oracle the tests compare against.
WalkResult WalkCurrentProcessThreadsViaSystemSnapshot(DWORD excludeThreadId, ACCESS_MASK desiredAccess,
                                                      const ThreadVisitor& visit);

// True while the process-scoped walk is still trusted.
bool ProcessScopedWalkAvailable();

// Test seam: clears the latch so a test can exercise both routes in one
// process. Not called by the runtime.
void ResetProcessScopedWalkAvailabilityForTesting();

}  // namespace ce::process_threads
