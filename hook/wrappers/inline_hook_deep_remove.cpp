#include "inline_hook.h"
#include "inline_hook_internal.h"
#include "hook_patch_transaction.h"

#include <windows.h>
#include <cstring>
#include <atomic>
#include <mutex>
#include "../common/hook_common.h"

namespace InlineHook {

namespace {
// Why the last deep-hook install refused. Kept next to the deep-hook ownership
// records rather than in the installer, which is at its size ceiling.
std::atomic<ce::hook_patch::QuiesceFailure> g_lastDeepHookQuiesceFailure{ce::hook_patch::QuiesceFailure::kNone};
}  // namespace

void SetLastDeepHookQuiesceFailure(ce::hook_patch::QuiesceFailure failure) {
    g_lastDeepHookQuiesceFailure.store(failure, std::memory_order_release);
}

ce::hook_patch::QuiesceFailure GetLastDeepHookQuiesceFailure() {
    return g_lastDeepHookQuiesceFailure.load(std::memory_order_acquire);
}

// Why a deep-hook patch landed or was refused. All of it used to be one sentence
// that folded three unrelated conditions together, so a transient
// thread-creation race could not be told apart from a permanent refusal
// (Witcher 3 + Smooth Motion, sessions 20260919_182155 / _183858).
void LogDeepHookPatchOutcome(const void* resumeCode, bool patchInstalled, bool acceptedUnstableSnapshot,
                             ce::hook_patch::QuiesceFailure quiesceFailure, bool ownershipChanged, DWORD patchError) {
    if (patchInstalled) {
        if (acceptedUnstableSnapshot) {
            HookLogImportant(
                "DeepHook: Patched %p on the suspended thread set while peers were still being created — the range "
                "was proven free of every suspended thread; only the unreachable no-new-threads guarantee was "
                "given up",
                resumeCode);
        }
        return;
    }
    HookLogImportant(
        "DeepHook: Refusing live patch at %p — quiesce=%s ownershipChanged=%d VirtualProtectError=%lu retryable=%d",
        resumeCode, ce::hook_patch::GetQuiesceFailureName(quiesceFailure), ownershipChanged ? 1 : 0,
        static_cast<unsigned long>(patchError), ce::hook_patch::IsRetryableQuiesceFailure(quiesceFailure) ? 1 : 0);
    SetLastDeepHookQuiesceFailure(quiesceFailure);
}

namespace {

enum class DeepHookRestoreResult {
    Restored,
    ForeignPreserved,
    QuiescenceFailed,
    ProtectFailed,
};

DeepHookRestoreResult RestoreOwnedDeepHookPatch(DeepHookEntry& entry) {
    MEMORY_BASIC_INFORMATION memory = {};
    const bool readable = VirtualQuery(entry.hookAddr, &memory, sizeof(memory)) == sizeof(memory) &&
                          memory.State == MEM_COMMIT && !(memory.Protect & (PAGE_NOACCESS | PAGE_GUARD));
    if (!readable || memcmp(entry.hookAddr, entry.installedBytes, entry.patchSize) != 0)
        return DeepHookRestoreResult::ForeignPreserved;

    {
        ce::hook_patch::ThreadQuiescence quiescence(entry.hookAddr, static_cast<size_t>(entry.patchSize));
        if (!quiescence.IsReady())
            return DeepHookRestoreResult::QuiescenceFailed;
        if (memcmp(entry.hookAddr, entry.installedBytes, entry.patchSize) != 0)
            return DeepHookRestoreResult::ForeignPreserved;

        DWORD oldProtect = 0;
        if (!VirtualProtect(entry.hookAddr, entry.patchSize, PAGE_EXECUTE_READWRITE, &oldProtect))
            return DeepHookRestoreResult::ProtectFailed;
        memcpy(entry.hookAddr, entry.origBytes, entry.patchSize);
        DWORD ignoredProtect = 0;
        VirtualProtect(entry.hookAddr, entry.patchSize, oldProtect, &ignoredProtect);
        FlushInstructionCache(GetCurrentProcess(), entry.hookAddr, entry.patchSize);
    }

    entry.installed = false;
    return DeepHookRestoreResult::Restored;
}

}  // namespace

bool RemoveDeepHook(void* target) {
    if (!target)
        return false;

    std::lock_guard<std::mutex> lock(g_hookMutex);

    for (auto& entry : g_deepHooks) {
        if (entry.target != target || !entry.installed)
            continue;

        const DeepHookRestoreResult result = RestoreOwnedDeepHookPatch(entry);
        if (result == DeepHookRestoreResult::Restored) {
            HookLog("DeepHook: Removed at %p; retaining trampoline for saved-chain safety", target);
            return true;
        }
        if (result == DeepHookRestoreResult::ForeignPreserved) {
            HookLogImportant("DeepHook: Preserving foreign replacement at %p; its chain may still retain CE",
                             entry.hookAddr);
        } else if (result == DeepHookRestoreResult::QuiescenceFailed) {
            HookLogImportant("DeepHook: Could not safely quiesce/remove %p; leaving CE chain installed",
                             entry.hookAddr);
        } else {
            HookLogImportant("DeepHook: Could not make %p writable; leaving CE chain installed", entry.hookAddr);
        }
        return false;
    }

    HookLog("DeepHook: No hook found for %p", target);
    return false;
}

void RemoveAllDeepHooksLocked() {
    for (auto& entry : g_deepHooks) {
        if (!entry.installed)
            continue;

        const DeepHookRestoreResult result = RestoreOwnedDeepHookPatch(entry);
        if (result == DeepHookRestoreResult::Restored)
            continue;
        if (result == DeepHookRestoreResult::ForeignPreserved) {
            HookLogImportant(
                "DeepHook: RemoveAll preserving foreign replacement at %p; CE chain state remains resident",
                entry.hookAddr);
        } else if (result == DeepHookRestoreResult::QuiescenceFailed) {
            HookLogImportant("DeepHook: RemoveAll could not safely quiesce %p; leaving CE hook installed",
                             entry.hookAddr);
        } else {
            HookLogImportant("DeepHook: RemoveAll could not make %p writable; leaving CE hook installed",
                             entry.hookAddr);
        }
    }
}

}  // namespace InlineHook
