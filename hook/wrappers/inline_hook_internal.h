/**
 * Inline Hook — shared engine state (internal)
 *
 * inline_hook.cpp, inline_hook_trampoline.cpp and inline_hook_deep.cpp are
 * three parts of one hook engine: they share the hook tables, the near-target
 * trampoline pools and the instruction relocation helpers. The state lives in
 * inline_hook_trampoline.cpp and is reached only through this header, which is
 * not part of the public inline_hook.h contract.
 */

#pragma once

#include <windows.h>
#include <cstdint>
#include <mutex>
#include <vector>

namespace InlineHook {

struct HookEntry {
    void* target;
    void* detour;
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
void WriteJump(uint8_t* dest, void* target);

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

}  // namespace InlineHook
