/**
 * Inline Hook — shared engine state (internal)
 *
 * inline_hook.cpp, inline_hook_entry_patch.cpp, inline_hook_trampoline.cpp,
 * inline_hook_deep.cpp and inline_hook_pristine_image.cpp are parts of one hook
 * engine: they share the hook tables, the near-target trampoline pools and the
 * instruction relocation helpers. The state lives in inline_hook_trampoline.cpp
 * and is reached only through this header, which is not part of the public
 * inline_hook.h contract.
 */

#pragma once

#include <windows.h>
#include <cstdint>
#include <mutex>
#include <vector>

#include "hook_patch_transaction.h"

namespace InlineHook {

struct PublishedHookSpec;

struct HookEntry {
    void* target;
    void* detour;
    void* patchDestination;
    void* trampoline;
    uint8_t origBytes[32];
    uint8_t installedBytes[32];
    int patchSize;
    bool installed;
};

struct DeepHookEntry {
    void* target;           // Original function address
    void* hookAddr;         // Address where the JMP was written (target + resumeOffset)
    int patchSize;          // Size of displaced instructions at hookAddr
    uint8_t origBytes[64];  // Original bytes at hookAddr (for removal)
    uint8_t installedBytes[64];
    uint8_t* trampoline;    // VirtualAlloc'd executable trampoline memory
    bool installed;
};

inline constexpr size_t TRAMPOLINE_POOL_SIZE = 4096;
inline constexpr size_t TRAMPOLINE_ENTRY_SIZE = 64;  // Max per hook
inline constexpr size_t TRAMPOLINE_ALIGNMENT = 16;

#ifdef _WIN64
inline constexpr int PATCH_SIZE = 14;  // FF 25 00 00 00 00 + 8-byte address
#else
inline constexpr int PATCH_SIZE = 5;  // E9 + 4-byte relative offset
#endif

// Defined in inline_hook_trampoline.cpp. g_hookMutex guards all of them.
extern std::vector<HookEntry> g_hooks;
extern std::vector<DeepHookEntry> g_deepHooks;
extern std::mutex g_hookMutex;
extern uint8_t* g_trampolinePool;
extern std::vector<uint8_t*> g_trampolinePools;
extern size_t g_trampolineOffset;

// Defined in inline_hook_pristine_image.cpp: read a function's unpatched bytes
// from its module's image file with the image-base relocations applied, so deep
// hooks and bypass trampolines can verify live code against what shipped.
bool ReadOrigBytesFromDisk(void* funcAddr, uint8_t* outBuf, int count, size_t* relocationsApplied);

// Batch-install helpers. The caller owns g_hookMutex throughout preparation
// and commit; failed published trampolines are retained under the same rule as
// a failed individual InstallPublished call.
bool PreparePublishedHookLocked(PublishedHookSpec* hook, size_t* hookIndex);

// Why a WriteOwnedEntryPatchQuiesced attempt refused to write. Recorded into
// the caller's struct instead of logged at the site: the write runs while every
// peer thread is suspended, and the logger takes a lock a suspended peer may
// hold - logging there can deadlock the process frozen mid-patch. Callers pass
// the record to ReportEntryPatchFailure once the quiescence transaction has
// released every peer.
enum class EntryPatchIssue : uint8_t {
    kNone,
    kBytesMismatch,
    kVirtualProtectFailed,
    kJumpUnreachable,
};

struct EntryPatchFailure {
    EntryPatchIssue issue = EntryPatchIssue::kNone;
    void* target = nullptr;
    DWORD error = 0;
};

// Rate-limited per issue. Never call while peer threads are suspended.
void ReportEntryPatchFailure(const EntryPatchFailure& failure);

// Caller must keep a ready ThreadQuiescence transaction alive for the entire
// call and prove this exact target range safe against its captured contexts.
// Records a failure reason into `outFailure` instead of logging (see above).
bool WriteOwnedEntryPatchQuiesced(void* target, void* patchDestination, int patchSize,
                                  const uint8_t* expectedBytes, uint8_t* installedBytes,
                                  EntryPatchFailure* outFailure);
bool WriteOwnedEntryPatch(void* target, void* patchDestination, int patchSize,
                          const uint8_t* expectedBytes, uint8_t* installedBytes);
// Defined in inline_hook_entry_patch.cpp with the two above: applying and
// reverting CE's own entry bytes is one unit, and it is the only part of the
// engine that writes live code.
//
// Restores hook.origBytes when CE still owns the installed bytes. False leaves
// the patch in place, which Remove/RemoveAll report and then keep the entry for -
// a hook CE cannot revert must stay known, not be forgotten while still patched.
bool RestoreOwnedEntryPatch(const HookEntry& hook);
// True when the live bytes are still the ones CE wrote.
bool InstalledEntryBytesMatch(const HookEntry& hook);
// InstalledEntryBytesMatch run through the restore policy: whether CE may revert.
bool OwnsInstalledEntryBytes(const HookEntry& hook);
bool CanCommitPreparedEntryPatchLocked(size_t hookIndex);
// Records a failure reason into `outFailure` instead of logging - the caller
// commits inside a ThreadQuiescence window (see EntryPatchFailure).
bool CommitPreparedEntryPatchQuiescedLocked(size_t hookIndex, EntryPatchFailure* outFailure);
bool CommitPreparedEntryPatchLocked(size_t hookIndex);

// Commit one TRAMPOLINE_POOL_SIZE page, writable, with an all-invalid CFG
// bitmap when Control Flow Guard is active.
uint8_t* AllocateWritableTrampolinePage(void* preferredAddress);

// Seal a whole private allocation execute/read and register its single aligned
// entrypoint with CFG. Restores the page to read/write on failure.
bool FinalizeExecutableTrampoline(void* allocationBase, size_t allocationSize, void* entrypoint, size_t usedBytes);

// Reserve the next slot in the pool nearest to 'nearAddr', allocating a new
// pool when required. The slot stays writable until it is finalized.
uint8_t* GetTrampolineSlot(void* nearAddr);

// Seal the slot handed out by the last GetTrampolineSlot() call and advance the
// pool cursor past it.
bool FinalizeCurrentTrampoline(uint8_t* trampoline, size_t usedBytes);

// Return the slot handed out by the last GetTrampolineSlot() call to the pool.
void AbandonCurrentTrampoline();

// Release a slot that was already sealed executable.
void ReleaseSealedTrampoline(void* trampoline);

// Called only while g_hookMutex is held. Restores deep hooks CE still owns and
// retains any chain state that a foreign follower may still call.
void RemoveAllDeepHooksLocked();

// Emit an absolute (x64) or relative (x86) jump from 'dest' to 'target'.
// False when no emitted displacement lands on 'target' (see
// ce::hook_jump_policy); the caller must fail the install instead of leaving a
// mis-aimed jump behind.
bool WriteJump(uint8_t* dest, void* target);

enum class ShortControlRelocationResult {
    kNotHandled,
    kHandled,
    kFailed,
};

// Rewrite a short branch whose target escapes the copied prologue block into an
// equivalent long form inside the trampoline. Intra-block hops are left alone
// so the byte-for-byte copy stays valid.
ShortControlRelocationResult TryRelocateExternalShortControlTransfer(
    const uint8_t* instrBytes, uintptr_t instrAddr, int instrLen, uintptr_t copiedBlockBase, size_t copiedBlockSize,
    uint8_t* trampoline, int* trampolineOffset, bool is64bit, const char* ownerTag);


// Reports the outcome of a deep-hook live patch. Defined in inline_hook_deep_remove.cpp
// so the installer unit stays inside the file-size ceiling.
void LogDeepHookPatchOutcome(const void* resumeCode, bool patchInstalled, bool acceptedUnstableSnapshot,
                             ce::hook_patch::QuiesceFailure quiesceFailure, bool ownershipChanged, DWORD patchError);

}  // namespace InlineHook
