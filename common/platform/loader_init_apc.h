#pragma once

/*
 * Ordering a remote load before a target's own code.
 *
 * A remote thread created while a process is still inside ntdll's loader
 * initialization does not run first: Windows holds it until initialization
 * completes and then releases it together with the initial thread, which goes
 * straight on to the image entry point. Whichever wins that race decides what
 * the injected DLL can still intercept. witcher3.exe calls its statically
 * imported sl.interposer.dll!slInit within milliseconds of initialization, so
 * the Streamline takeover's DllMain gate sometimes saw the call (sessions
 * 20261013_005224 p7728, 20261013_111644 p26376 entered the gate in the same
 * millisecond DllMain started) and sometimes did not (20261014_190537 p35192:
 * slInitGateEntered=0, resident runtime kept).
 *
 * ntdll's LdrpInitialize delivers the initial thread's pending user APCs
 * (NtTestAlert) after process initialization and before the entry point. An
 * APC queued while that thread is provably short of that point therefore runs
 * before any code of the image, on the target's own initial thread. Proof is
 * read from the target's PEB while its initial thread is suspended:
 * - PEB.Ldr == null: initialization has not started (created suspended);
 * - CrossProcessFlags.ProcessInitializing set: initialization is in progress.
 *   ntdll clears it before the NtTestAlert delivery (measured: 0x3 inside a
 *   static-import DllMain, 0x1 inside the init-end APC, which runs before main).
 * Anything else means the target may already run its own code; the caller
 * keeps its ordinary route and no APC is queued (a stale APC would fire at an
 * arbitrary later alertable wait).
 *
 * Only the target process is touched: its initial thread is suspended for the
 * two memory reads and the queue call, then resumed. Native x64 targets only;
 * the PEB offsets below are the x64 layout.
 */

#include <windows.h>

#include <cstdint>

namespace ce::loader_init_apc {

enum class Phase {
    NotStarted,    // PEB.Ldr not yet set: the initial thread has not begun initialization.
    Initializing,  // PEB.ProcessInitializing set: inside LdrpInitializeProcess.
    Complete,      // Initialization finished: the image's own code may be running.
    Unsupported,   // Not a native x64 target, or this is the calling process.
    Unreadable,    // PEB, initial thread or its suspension could not be obtained.
};

const char* PhaseName(Phase phase);

// PEB fields as read while the initial thread is suspended.
Phase ClassifyPhase(uint64_t pebLdr, uint32_t crossProcessFlags);

// Reads PEB.Ldr and CrossProcessFlags of a native x64 `process` (needs
// PROCESS_QUERY_INFORMATION | PROCESS_VM_READ). Racy unless the initial thread
// is suspended or otherwise held; QueueBeforeEntryPoint does that itself.
Phase ReadPhase(HANDLE process, DWORD* error = nullptr);

// Whether ntdll delivers an APC queued in `phase` before the image entry point.
bool RunsBeforeEntryPoint(Phase phase);

struct Result {
    bool queued = false;
    Phase phase = Phase::Unreadable;
    DWORD initialThreadId = 0;
    DWORD error = ERROR_SUCCESS;
};

// `process` needs PROCESS_QUERY_INFORMATION | PROCESS_VM_READ. Queues
// `routine(argument)` on the initial thread only when RunsBeforeEntryPoint;
// the argument's memory must stay valid until the APC has run.
Result QueueBeforeEntryPoint(HANDLE process, DWORD pid, PAPCFUNC routine, ULONG_PTR argument);

}  // namespace ce::loader_init_apc
