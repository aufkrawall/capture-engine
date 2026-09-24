/**
 * Inline Hook - CE's own entry patch, applied and reverted under quiescence
 *
 * The bytes at a hooked function's entry are the one piece of the engine that is
 * written while the process is running, so every write here is gated on a
 * ThreadQuiescence transaction that proves no peer thread is executing the range
 * about to change. Split out of inline_hook.cpp, which owns the install/remove
 * bookkeeping around these writes and stays inside the file-size ceiling that way.
 */

#include "inline_hook.h"
#include "inline_hook_internal.h"
#include "inline_hook_lde.h"
#include "inline_hook_policy.h"
#include "hook_patch_transaction.h"

#include <windows.h>

#include <atomic>
#include <cstdint>
#include <cstring>

#include "../common/hook_common.h"
#include "../common/hook_jump_policy.h"

namespace InlineHook {

static bool WriteJumpWithoutLogging(uint8_t* destination, void* target) {
#ifdef _WIN64
    memcpy(destination + 6, static_cast<const void*>(&target), sizeof(target));
    MemoryBarrier();
    const uint8_t header[6] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    memcpy(destination, header, sizeof(header));
    return true;
#else
    // Native x86 E9 semantics wrap the EIP update modulo the 32-bit address
    // space, so a cross-module entry patch (game code -> hook DLL) is legal at
    // any distance inside it; the policy still computes the displacement in
    // 64-bit signed arithmetic and round-trips it, so a jump that cannot land
    // fails the install instead of being silently cast.
    int32_t displacement = 0;
    if (!ce::hook_jump_policy::TryRel32Displacement(destination, target,
                                                    ce::hook_jump_policy::Rel32Semantics::kWrapAddress32,
                                                    &displacement))
        return false;
    memcpy(destination + 1, &displacement, sizeof(displacement));
    MemoryBarrier();
    destination[0] = 0xE9;
    return true;
#endif
}

#ifdef _WIN64
static bool WriteNearJumpWithoutLogging(uint8_t* destination, void* target) {
    int32_t displacement32 = 0;
    if (!ce::hook_jump_policy::TryRel32Displacement(destination, target,
                                                    ce::hook_jump_policy::Rel32Semantics::kSignExtended,
                                                    &displacement32))
        return false;
    memcpy(destination + 1, &displacement32, sizeof(displacement32));
    MemoryBarrier();
    destination[0] = 0xE9;
    return true;
}
#endif

bool WriteOwnedEntryPatchQuiesced(void* target, void* patchDestination, int patchSize,
                                  const uint8_t* expectedBytes, uint8_t* installedBytes,
                                  EntryPatchFailure* outFailure) {
    if (outFailure)
        *outFailure = EntryPatchFailure{};
    if (memcmp(target, expectedBytes, patchSize) != 0) {
        if (outFailure) {
            outFailure->issue = EntryPatchIssue::kBytesMismatch;
            outFailure->target = target;
        }
        return false;
    }
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        if (outFailure) {
            outFailure->issue = EntryPatchIssue::kVirtualProtectFailed;
            outFailure->target = target;
            outFailure->error = GetLastError();
        }
        return false;
    }
#ifdef _WIN64
    const bool jumpWritten = (patchSize == ce::inline_hook_policy::kExternalPrependPatchSize)
                                 ? WriteNearJumpWithoutLogging(static_cast<uint8_t*>(target), patchDestination)
                                 : WriteJumpWithoutLogging(static_cast<uint8_t*>(target), patchDestination);
#else
    const bool jumpWritten = WriteJumpWithoutLogging(static_cast<uint8_t*>(target), patchDestination);
#endif
    if (!jumpWritten) {
        DWORD ignoredProtect = 0;
        VirtualProtect(target, patchSize, oldProtect, &ignoredProtect);
        if (outFailure) {
            outFailure->issue = EntryPatchIssue::kJumpUnreachable;
            outFailure->target = target;
        }
        return false;
    }
    for (int i = PATCH_SIZE; i < patchSize; ++i)
        static_cast<uint8_t*>(target)[i] = 0x90;
    DWORD ignoredProtect = 0;
    VirtualProtect(target, patchSize, oldProtect, &ignoredProtect);
    FlushInstructionCache(GetCurrentProcess(), target, patchSize);
    memcpy(installedBytes, target, patchSize);
    return true;
}

void ReportEntryPatchFailure(const EntryPatchFailure& failure) {
    static std::atomic<uint32_t> s_mismatchLogs{0};
    static std::atomic<uint32_t> s_vpFailLogs{0};
    static std::atomic<uint32_t> s_jumpFailLogs{0};
    uint32_t count = 0;
    switch (failure.issue) {
        case EntryPatchIssue::kBytesMismatch:
            count = s_mismatchLogs.fetch_add(1, std::memory_order_relaxed);
            if (count < 8 || (count % 64) == 0) {
                HookLogImportant("InlineHook: WriteOwnedEntryPatch bytes changed concurrently at %p (count=%u)",
                                 failure.target, count + 1);
            }
            break;
        case EntryPatchIssue::kVirtualProtectFailed:
            count = s_vpFailLogs.fetch_add(1, std::memory_order_relaxed);
            if (count < 8 || (count % 64) == 0) {
                HookLogImportant("InlineHook: WriteOwnedEntryPatch VirtualProtect failed at %p (error=%lu count=%u)",
                                 failure.target, static_cast<unsigned long>(failure.error), count + 1);
            }
            break;
        case EntryPatchIssue::kJumpUnreachable:
            count = s_jumpFailLogs.fetch_add(1, std::memory_order_relaxed);
            if (count < 8 || (count % 64) == 0) {
                HookLogImportant("InlineHook: WriteOwnedEntryPatch jump target unreachable from %p (count=%u)",
                                 failure.target, count + 1);
            }
            break;
        case EntryPatchIssue::kNone:
            break;
    }
}

// Runs `action` under thread quiescence, retrying once with kAcceptSuspendedSet
// when the strict walk only failed because peers kept appearing.
//
// `what` names the operation for the log. The relaxed retry is not a silent
// downgrade - it drops the "no new threads appeared" condition while keeping the
// IsRangeSafe() check that is what actually makes the write safe - but it is
// worth being able to see in a session log which patches took it, so it is
// reported once the transaction has released every peer. Logging while threads
// are suspended can deadlock (the logger takes a lock a suspended peer may hold),
// which is why nothing here logs before the ThreadQuiescence destructor runs.
template <typename F>
static bool ExecuteWithQuiescenceFallback(const void* target, size_t patchSize, const char* what,
                                          ce::hook_patch::QuiesceFailure* outFailure, F&& action) {
    if (outFailure)
        *outFailure = ce::hook_patch::QuiesceFailure::kNone;
    {
        ce::hook_patch::ThreadQuiescence quiescence(target, patchSize);
        if (quiescence.IsReady())
            return action();
        if (outFailure)
            *outFailure = quiescence.FailureReason();
        if (quiescence.FailureReason() != ce::hook_patch::QuiesceFailure::kUnstableSnapshot)
            return false;
    }
    bool relaxedReady = false;
    bool relaxedResult = false;
    ce::hook_patch::QuiesceFailure relaxedFailure = ce::hook_patch::QuiesceFailure::kNone;
    {
        ce::hook_patch::ThreadQuiescence fallback(target, patchSize,
                                                  ce::hook_patch::UnstableSnapshotPolicy::kAcceptSuspendedSet);
        relaxedReady = fallback.IsReady();
        if (relaxedReady) {
            relaxedResult = action();
        } else {
            relaxedFailure = fallback.FailureReason();
        }
    }
    if (!relaxedReady) {
        if (outFailure)
            *outFailure = relaxedFailure;
        return false;
    }
    if (outFailure)
        *outFailure = ce::hook_patch::QuiesceFailure::kNone;
    static std::atomic<uint32_t> s_relaxedLogs{0};
    const uint32_t count = s_relaxedLogs.fetch_add(1, std::memory_order_relaxed);
    if (count < 8 || (count % 64) == 0) {
        HookLogImportant("InlineHook: %s at %p used the relaxed thread-snapshot policy (result=%d count=%u)",
                         what ? what : "patch", target, relaxedResult ? 1 : 0, count + 1);
    }
    return relaxedResult;
}

bool WriteOwnedEntryPatch(void* target, void* patchDestination, int patchSize,
                          const uint8_t* expectedBytes, uint8_t* installedBytes) {
    ce::hook_patch::QuiesceFailure quiesceFailure = ce::hook_patch::QuiesceFailure::kNone;
    EntryPatchFailure patchFailure;
    const bool success = ExecuteWithQuiescenceFallback(
        target, static_cast<size_t>(patchSize), "install", &quiesceFailure, [&]() {
            return WriteOwnedEntryPatchQuiesced(target, patchDestination, patchSize, expectedBytes, installedBytes,
                                                &patchFailure);
        });
    // Reported here, not inside the transaction: a suspended peer may hold the
    // logger's lock (see EntryPatchFailure).
    if (!success)
        ReportEntryPatchFailure(patchFailure);
    if (!success && quiesceFailure != ce::hook_patch::QuiesceFailure::kNone) {
        static std::atomic<uint32_t> s_quiesceFailLogs{0};
        const uint32_t count = s_quiesceFailLogs.fetch_add(1, std::memory_order_relaxed);
        if (count < 8 || (count % 64) == 0) {
            HookLogImportant("InlineHook: WriteOwnedEntryPatch quiescence failed at %p (reason=%d count=%u)", target,
                             static_cast<int>(quiesceFailure), count + 1);
        }
    }
    return success;
}

// Removal keeps the relaxed fallback deliberately, and for a stronger reason than
// installation has. A refused install costs one hook; a refused removal leaves
// CE's jump patched into the game (Remove/RemoveAll both log "leaving it
// installed" and keep the entry), and at unload that jump points into memory CE
// no longer occupies. Under NVIDIA Smooth Motion the strict condition is
// unreachable during teardown for the same reason it is during D3D init, so
// refusing there would be choosing the worse failure.
bool RestoreOwnedEntryPatch(const HookEntry& hook) {
    return ExecuteWithQuiescenceFallback(hook.target, static_cast<size_t>(hook.patchSize), "remove", nullptr, [&]() {
        if (memcmp(hook.target, hook.installedBytes, hook.patchSize) != 0)
            return false;
        DWORD oldProtect = 0;
        if (!VirtualProtect(hook.target, hook.patchSize, PAGE_EXECUTE_READWRITE, &oldProtect))
            return false;
        memcpy(hook.target, hook.origBytes, hook.patchSize);
        DWORD ignoredProtect = 0;
        VirtualProtect(hook.target, hook.patchSize, oldProtect, &ignoredProtect);
        FlushInstructionCache(GetCurrentProcess(), hook.target, hook.patchSize);
        return true;
    });
}

bool InstalledEntryBytesMatch(const HookEntry& hook) {
    if (!hook.target || hook.patchSize <= 0 || hook.patchSize > static_cast<int>(sizeof(hook.installedBytes))) {
        return false;
    }
    uint8_t liveBytes[sizeof(hook.installedBytes)] = {};
    SIZE_T bytesRead = 0;
    return ReadProcessMemory(GetCurrentProcess(), hook.target, liveBytes, static_cast<SIZE_T>(hook.patchSize),
                             &bytesRead) &&
           bytesRead == static_cast<SIZE_T>(hook.patchSize) &&
           memcmp(liveBytes, hook.installedBytes, static_cast<size_t>(hook.patchSize)) == 0;
}

// ============================================================================
// Public API
// ============================================================================


bool OwnsInstalledEntryBytes(const HookEntry& hook) {
    return ce::inline_hook_policy::ShouldRestoreOwnedPatch(InstalledEntryBytesMatch(hook));
}

}  // namespace InlineHook
