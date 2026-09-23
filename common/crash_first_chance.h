#pragma once

// First-chance fault bookkeeping for the vectored crash handler.
//
// A first-chance hardware fault is not a crash: managed and JIT runtimes,
// emulators and anti-tamper code raise and handle them as control flow (see
// ce::crash_dump_policy::ClassifyFirstChanceException). The handler therefore
// only records the fault's record and context here - into fixed static slots,
// with no allocation, lock or I/O - and the dump is taken when the process
// actually dies of it. That happens either through the unhandled filter or
// through a termination that follows the fault
// (ce::crash_dump_policy::IsTerminationFollowingUnresolvedFault). The latter
// is where the recorded context is used, so the dump still names the faulting
// instruction.

#include <windows.h>

#include <cstdint>

namespace ce::crash_first_chance {

// Records `pointers` as the calling thread's most recent unresolved fault.
// Never allocates, locks or blocks; if every slot is busy the record is dropped
// and counted.
void RecordFault(const EXCEPTION_POINTERS* pointers);

// Forgets the calling thread's recorded fault (a handler resumed execution).
void ClearFaultForCurrentThread();

// Copies the calling thread's recorded fault. False when there is none.
bool CopyFaultForCurrentThread(EXCEPTION_RECORD* record, CONTEXT* context);

// True when the calling thread is executing inside exception dispatch right
// now: an exception filter, a vectored handler or an unwinding frame between
// the raise and its resolution. Resolved from the return addresses on the
// thread's own stack (KiUserExceptionDispatcher / RtlRaiseException frames).
bool IsCurrentThreadInsideExceptionDispatch();

// Registers the vectored continue handler that clears a thread's record when
// any handler resolves the exception by continuing execution, and caches the
// dispatcher address ranges. Called once from InstallCrashHandler.
void Install();

struct Statistics {
    uint64_t recorded = 0;
    uint64_t resolvedByContinue = 0;
    uint64_t dropped = 0;
};
Statistics GetStatistics();

#ifdef CE_UNIT_TESTS
void ResetForTesting();
bool IsAddressInDispatcherForTesting(const void* address);
#endif

}  // namespace ce::crash_first_chance
