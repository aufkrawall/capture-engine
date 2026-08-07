/**
 * Inline Hook — trampoline pool and instruction relocation
 *
 * Owns the hook tables, the near-target trampoline pools and their W^X /
 * Control Flow Guard lifecycle, plus the jump emission and short-branch
 * relocation helpers shared by the shallow and deep hook installers.
 *
 * Split out of inline_hook.cpp; see inline_hook_internal.h for the shared
 * surface.
 */

#include "inline_hook.h"
#include "inline_hook_internal.h"

#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "../common/hook_common.h"

namespace InlineHook {

// ============================================================================
// Hook State
// ============================================================================
//
// The tables and the pool cursor are declared in inline_hook_internal.h and
// defined here, so the installers in inline_hook.cpp and inline_hook_deep.cpp
// share one engine.

std::vector<HookEntry> g_hooks;
std::vector<DeepHookEntry> g_deepHooks;
std::mutex g_hookMutex;
uint8_t* g_trampolinePool = nullptr;
std::vector<uint8_t*> g_trampolinePools;
size_t g_trampolineOffset = 0;

static bool IsControlFlowGuardEnabled();

uint8_t* AllocateWritableTrampolinePage(void* preferredAddress) {
    const bool cfgEnabled = IsControlFlowGuardEnabled();
    const DWORD initialProtection = cfgEnabled ? PAGE_EXECUTE_READ | PAGE_TARGETS_INVALID : PAGE_READWRITE;
    void* allocation =
        VirtualAlloc(preferredAddress, TRAMPOLINE_POOL_SIZE, MEM_COMMIT | MEM_RESERVE, initialProtection);
    if (!allocation || !cfgEnabled)
        return static_cast<uint8_t*>(allocation);

    // PAGE_TARGETS_INVALID is accepted only by VirtualAlloc. Start with an RX
    // page whose entire CFG bitmap is invalid, then remove execute permission
    // while constructing the trampoline. Finalization restores RX with
    // PAGE_TARGETS_NO_UPDATE so the invalid bitmap survives until the one
    // aligned entrypoint is registered.
    DWORD oldProtection = 0;
    if (!VirtualProtect(allocation, TRAMPOLINE_POOL_SIZE, PAGE_READWRITE, &oldProtection)) {
        VirtualFree(allocation, 0, MEM_RELEASE);
        return nullptr;
    }
    return static_cast<uint8_t*>(allocation);
}

#ifdef _WIN64
namespace {

constexpr uintptr_t kTrampolineSearchWindow = 0x7FFF0000ULL;
constexpr uintptr_t kAllocationGranularity = 0x10000ULL;

// Address inside [regionStart, regionEnd) that is closest to the target and can
// still hold a whole pool, or 0 when the region cannot host one.
uintptr_t ClosestPoolAddressInRegion(uintptr_t regionStart, uintptr_t regionEnd, uintptr_t low, uintptr_t high,
                                     uintptr_t target) {
    if (regionEnd < TRAMPOLINE_POOL_SIZE)
        return 0;
    const uintptr_t firstFit =
        (std::max(regionStart, low) + kAllocationGranularity - 1) & ~(kAllocationGranularity - 1);
    uintptr_t lastFit = (regionEnd - TRAMPOLINE_POOL_SIZE) & ~(kAllocationGranularity - 1);
    if (lastFit > high)
        lastFit = high & ~(kAllocationGranularity - 1);
    if (firstFit > lastFit || firstFit + TRAMPOLINE_POOL_SIZE > regionEnd)
        return 0;

    uintptr_t candidate = target & ~(kAllocationGranularity - 1);
    if (candidate < firstFit)
        candidate = firstFit;
    else if (candidate > lastFit)
        candidate = lastFit;
    return candidate;
}

}  // namespace
#endif

// Allocate memory near the target (within ±2GB for x64).
// Each trampoline gets a private read/write page while it is constructed. The
// page is sealed execute/read before any target can reference it, which keeps
// the allocation W^X and avoids changing protection under active callers.
static uint8_t* AllocateTrampolinePool(void* nearAddr) {
#ifdef _WIN64
    // Try to allocate within ±2GB of target for RIP-relative fixups.
    uintptr_t target = (uintptr_t)nearAddr;
    uintptr_t low = target > kTrampolineSearchWindow ? target - kTrampolineSearchWindow : 0x10000ULL;
    uintptr_t high = target + kTrampolineSearchWindow;

    // Prefer the free block closest to the target. Taking the first free block
    // above `low` instead lands roughly 2GB below the target, which pushes the
    // rewritten displacement of any RIP-relative instruction in the copied
    // prologue - those normally reference data just past the function, i.e. above
    // the target - outside the +/-2GB range, so the install fails. That is what
    // kept opengl32's swap exports on the IAT-only path.
    uintptr_t bestAddr = 0;
    uintptr_t bestDistance = 0;
    MEMORY_BASIC_INFORMATION mbi;
    for (uintptr_t addr = low; addr < high;) {
        if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == 0)
            break;

        const uintptr_t regionStart = (uintptr_t)mbi.BaseAddress;
        const uintptr_t regionEnd = regionStart + mbi.RegionSize;
        if (mbi.State == MEM_FREE && mbi.RegionSize >= TRAMPOLINE_POOL_SIZE) {
            const uintptr_t candidate = ClosestPoolAddressInRegion(regionStart, regionEnd, low, high, target);
            if (candidate != 0) {
                const uintptr_t distance = candidate > target ? candidate - target : target - candidate;
                if (bestAddr == 0 || distance < bestDistance) {
                    bestAddr = candidate;
                    bestDistance = distance;
                }
            }
        }
        addr = regionEnd;
    }

    if (bestAddr != 0) {
        if (void* p = AllocateWritableTrampolinePage(reinterpret_cast<void*>(bestAddr)))
            return (uint8_t*)p;
    }

    // The preferred address can be taken by another thread between the scan and
    // the reservation; fall back to first fit inside the same window.
    for (uintptr_t addr = low; addr < high;) {
        if (VirtualQuery((void*)addr, &mbi, sizeof(mbi)) == 0)
            break;

        if (mbi.State == MEM_FREE && mbi.RegionSize >= TRAMPOLINE_POOL_SIZE) {
            uintptr_t aligned = (addr + 0xFFFF) & ~(uintptr_t)0xFFFF;
            if (aligned + TRAMPOLINE_POOL_SIZE <= addr + mbi.RegionSize) {
                void* p = AllocateWritableTrampolinePage(reinterpret_cast<void*>(aligned));
                if (p)
                    return (uint8_t*)p;
            }
        }
        addr = (uintptr_t)mbi.BaseAddress + mbi.RegionSize;
    }
#endif
    // Fallback: allocate anywhere
    return AllocateWritableTrampolinePage(nullptr);
}

bool IsInTrampolinePool(void* address) {
    if (!address)
        return false;
    for (const auto& pool : g_trampolinePools) {
        uintptr_t poolStart = reinterpret_cast<uintptr_t>(pool);
        uintptr_t poolEnd = poolStart + TRAMPOLINE_POOL_SIZE;
        uintptr_t addr = reinterpret_cast<uintptr_t>(address);
        if (addr >= poolStart && addr < poolEnd)
            return true;
    }
    return false;
}

bool GetTrampolinePoolInfo(void* address, void** poolBaseOut, DWORD* protectOut) {
    if (poolBaseOut) {
        *poolBaseOut = nullptr;
    }
    if (protectOut) {
        *protectOut = 0;
    }
    if (!address) {
        return false;
    }

    uintptr_t addr = reinterpret_cast<uintptr_t>(address);
    for (const auto& pool : g_trampolinePools) {
        uintptr_t poolStart = reinterpret_cast<uintptr_t>(pool);
        uintptr_t poolEnd = poolStart + TRAMPOLINE_POOL_SIZE;
        if (addr >= poolStart && addr < poolEnd) {
            if (poolBaseOut) {
                *poolBaseOut = pool;
            }
            if (protectOut) {
                MEMORY_BASIC_INFORMATION mbi = {};
                if (VirtualQuery(address, &mbi, sizeof(mbi)) != 0) {
                    *protectOut = mbi.Protect;
                }
            }
            return true;
        }
    }
    return false;
}

static bool IsControlFlowGuardEnabled() {
    PROCESS_MITIGATION_CONTROL_FLOW_GUARD_POLICY policy{};
    return GetProcessMitigationPolicy(GetCurrentProcess(), ProcessControlFlowGuardPolicy, &policy, sizeof(policy)) &&
           policy.EnableControlFlowGuard;
}

using SetProcessValidCallTargetsFn = BOOL(WINAPI*)(HANDLE, PVOID, SIZE_T, ULONG, PCFG_CALL_TARGET_INFO);

#ifdef _WIN64
__declspec(guard(nocf))
#endif
static BOOL CallCfgRegistrationBootstrap(SetProcessValidCallTargetsFn function, HANDLE process, PVOID base, SIZE_T size,
                                         ULONG count, PCFG_CALL_TARGET_INFO targets) {
    // MinGW's Kernel32 import library does not expose this Windows API. This
    // one trusted export call must bootstrap registration without first
    // requiring its dynamically resolved address to pass the host CFG bitmap.
    return function(process, base, size, count, targets);
}

static bool RegisterOnlyTrampolineEntrypoint(void* allocationBase, size_t allocationSize, void* entrypoint) {
    if (!IsControlFlowGuardEnabled())
        return true;
    const uintptr_t base = reinterpret_cast<uintptr_t>(allocationBase);
    const uintptr_t entry = reinterpret_cast<uintptr_t>(entrypoint);
    if (!allocationBase || entry < base || entry >= base + allocationSize ||
        ((entry - base) % TRAMPOLINE_ALIGNMENT) != 0) {
        return false;
    }
    CFG_CALL_TARGET_INFO target{};
    target.Offset = entry - base;
    target.Flags = CFG_CALL_TARGET_VALID;
    static const auto setProcessValidCallTargets = []() -> SetProcessValidCallTargetsFn {
        HMODULE module = GetModuleHandleW(L"kernelbase.dll");
        if (!module)
            module = GetModuleHandleW(L"kernel32.dll");
        return module ? reinterpret_cast<SetProcessValidCallTargetsFn>(
                            GetProcAddress(module, "SetProcessValidCallTargets"))
                      : nullptr;
    }();
    if (!setProcessValidCallTargets) {
        HookLogImportant("InlineHook: SetProcessValidCallTargets is unavailable for CFG-enabled target");
        return false;
    }
    if (!CallCfgRegistrationBootstrap(setProcessValidCallTargets, GetCurrentProcess(), allocationBase, allocationSize,
                                      1, &target)) {
        HookLogImportant("InlineHook: SetProcessValidCallTargets failed for entry=%p page=%p error=%lu", entrypoint,
                         allocationBase, GetLastError());
        return false;
    }
    return true;
}

bool FinalizeExecutableTrampoline(void* allocationBase, size_t allocationSize, void* entrypoint,
                                         size_t usedBytes) {
    if (!allocationBase || !entrypoint || usedBytes == 0 || usedBytes > allocationSize)
        return false;
    FlushInstructionCache(GetCurrentProcess(), entrypoint, usedBytes);
    DWORD oldProtect = 0;
    const DWORD executeProtection = PAGE_EXECUTE_READ | (IsControlFlowGuardEnabled() ? PAGE_TARGETS_NO_UPDATE : 0);
    if (!VirtualProtect(allocationBase, allocationSize, executeProtection, &oldProtect)) {
        HookLogImportant("InlineHook: Failed to seal trampoline RX entry=%p error=%lu", entrypoint, GetLastError());
        return false;
    }
    if (!RegisterOnlyTrampolineEntrypoint(allocationBase, allocationSize, entrypoint)) {
        DWORD ignored = 0;
        VirtualProtect(allocationBase, allocationSize, PAGE_READWRITE, &ignored);
        return false;
    }
    return true;
}

uint8_t* GetTrampolineSlot(void* nearAddr) {
    if (!g_trampolinePool) {
        uint8_t* newPool = AllocateTrampolinePool(nearAddr);
        if (!newPool)
            return nullptr;
        g_trampolinePool = newPool;
        g_trampolineOffset = 0;
        g_trampolinePools.push_back(g_trampolinePool);
        HookLog("GetTrampolineSlot: New pool allocated at %p (total pools=%zu)", g_trampolinePool,
                g_trampolinePools.size());
    }

    g_trampolineOffset = (g_trampolineOffset + TRAMPOLINE_ALIGNMENT - 1) & ~(TRAMPOLINE_ALIGNMENT - 1);
    uint8_t* slot = g_trampolinePool + g_trampolineOffset;
    HookLog("GetTrampolineSlot: Allocating slot at offset %zu (addr=%p) for target %p", g_trampolineOffset, slot,
            nearAddr);
    g_trampolineOffset += TRAMPOLINE_ENTRY_SIZE;
    return slot;
}

bool FinalizeCurrentTrampoline(uint8_t* trampoline, size_t usedBytes) {
    uint8_t* pool = g_trampolinePool;
    const bool validEntry = pool && trampoline && trampoline >= pool && trampoline < pool + TRAMPOLINE_POOL_SIZE;
    const bool finalized =
        validEntry && FinalizeExecutableTrampoline(pool, TRAMPOLINE_POOL_SIZE, trampoline, usedBytes);
    // A sealed or failed page is never reopened for another trampoline. This
    // avoids transiently revoking execute permission from an active entrypoint.
    g_trampolinePool = nullptr;
    g_trampolineOffset = 0;
    if (!finalized && pool) {
        const auto entry = std::find(g_trampolinePools.begin(), g_trampolinePools.end(), pool);
        if (entry != g_trampolinePools.end())
            g_trampolinePools.erase(entry);
        VirtualFree(pool, 0, MEM_RELEASE);
    }
    return finalized;
}

void AbandonCurrentTrampoline() {
    uint8_t* pool = g_trampolinePool;
    g_trampolinePool = nullptr;
    g_trampolineOffset = 0;
    if (!pool)
        return;
    const auto entry = std::find(g_trampolinePools.begin(), g_trampolinePools.end(), pool);
    if (entry != g_trampolinePools.end())
        g_trampolinePools.erase(entry);
    VirtualFree(pool, 0, MEM_RELEASE);
}

void ReleaseSealedTrampoline(void* trampoline) {
    if (!trampoline)
        return;
    const auto entry = std::find(g_trampolinePools.begin(), g_trampolinePools.end(), trampoline);
    if (entry == g_trampolinePools.end())
        return;
    g_trampolinePools.erase(entry);
    VirtualFree(trampoline, 0, MEM_RELEASE);
}

// Write an absolute jump at 'dest' to 'target'
void WriteJump(uint8_t* dest, void* target) {
#ifdef _WIN64
    // CRITICAL ORDER: Write the 8-byte absolute target address FIRST, then the
    // 6-byte JMP [RIP+0] header. If a concurrent thread sees a partial JMP,
    // dest+6 already contains the correct absolute target, and the disp32=0
    // means [RIP+0] correctly reads from dest+6.
    memcpy(dest + 6, reinterpret_cast<const void*>(&target), 8);
    MemoryBarrier();
    // Write full 6-byte JMP header atomically (32-bit aligned, single memcpy)
    const uint8_t jmpHeader[6] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    memcpy(dest, jmpHeader, 6);
    HookLog("WriteJump: x64 JMP [RIP+0] -> %p at %p", target, dest);
#else
    // E9 [4-byte relative offset]
    // CRITICAL ORDER: Write the displacement FIRST (as a 4-byte aligned write),
    // then the opcode. A concurrent thread sees either original code (safe)
    // or a complete JMP with a valid displacement.
    // The displacement is relative to the instruction AFTER the JMP
    // JMP rel32 means: RIP = (address of next instruction) + rel32
    // So: target = (dest + 5) + rel32
    // Therefore: rel32 = target - (dest + 5)
    int32_t rel = (int32_t)((uintptr_t)target - (uintptr_t)(dest + 5));
    memcpy(dest + 1, &rel, 4);
    MemoryBarrier();
    dest[0] = 0xE9;
    HookLog("WriteJump: x86 JMP rel32 -> %p at %p (rel=0x%08X, dest+5=%p)", target, dest, (unsigned)rel,
            (void*)(dest + 5));
    // Verify: dest+5 + rel should equal target
    uintptr_t verify = (uintptr_t)(dest + 5) + rel;
    HookLog(
        "WriteJump: Verification: dest+5(0x%p) + rel(0x%08X) = 0x%p "
        "(expected %p)",
        (void*)(dest + 5), (unsigned)rel, (void*)verify, target);
#endif
}

static bool IsShortConditionalJumpOpcode(uint8_t opcode) {
    return opcode >= 0x70 && opcode <= 0x7F;
}

static bool IsShortUnconditionalJumpOpcode(uint8_t opcode) {
    return opcode == 0xEB;
}

static bool IsShortLoopControlOpcode(uint8_t opcode) {
    return opcode >= 0xE0 && opcode <= 0xE3;
}

ShortControlRelocationResult TryRelocateExternalShortControlTransfer(
    const uint8_t* instrBytes, uintptr_t instrAddr, int instrLen, uintptr_t copiedBlockBase, size_t copiedBlockSize,
    uint8_t* trampoline, int* trampolineOffset, bool is64bit, const char* ownerTag) {
    if (!instrBytes || instrLen < 2 || !trampoline || !trampolineOffset) {
        return ShortControlRelocationResult::kNotHandled;
    }

    const uint8_t opcode = instrBytes[0];
    if (!IsShortConditionalJumpOpcode(opcode) && !IsShortUnconditionalJumpOpcode(opcode) &&
        !IsShortLoopControlOpcode(opcode)) {
        return ShortControlRelocationResult::kNotHandled;
    }

    const int8_t origDisp = static_cast<int8_t>(instrBytes[1]);
    const uintptr_t absTarget = instrAddr + instrLen + origDisp;
    const uintptr_t copiedBlockEnd = copiedBlockBase + copiedBlockSize;

    // The GTA FSR->DLSS crash came from a short branch that escaped the copied
    // prologue and then landed inside the trampoline's appended jump stub.
    // Keep intra-block short hops on the byte-for-byte path and rewrite only
    // branches that leave the copied block.
    if (absTarget >= copiedBlockBase && absTarget < copiedBlockEnd) {
        return ShortControlRelocationResult::kNotHandled;
    }

    if (IsShortLoopControlOpcode(opcode)) {
        HookLog("%s: Cannot relocate external short loop/control opcode 0x%02X at %p (target=%p)",
                ownerTag ? ownerTag : "InlineHook", opcode, reinterpret_cast<void*>(instrAddr),
                reinterpret_cast<void*>(absTarget));
        return ShortControlRelocationResult::kFailed;
    }

    if (IsShortUnconditionalJumpOpcode(opcode)) {
        const int64_t newDisp = static_cast<int64_t>(absTarget) -
                                static_cast<int64_t>(reinterpret_cast<uintptr_t>(trampoline + *trampolineOffset + 5));
        if (newDisp >= INT32_MIN && newDisp <= INT32_MAX) {
            trampoline[*trampolineOffset] = 0xE9;
            const int32_t newDisp32 = static_cast<int32_t>(newDisp);
            memcpy(trampoline + *trampolineOffset + 1, &newDisp32, 4);
            *trampolineOffset += 5;
        }
#ifdef _WIN64
        else if (is64bit) {
            WriteJump(trampoline + *trampolineOffset, reinterpret_cast<void*>(absTarget));
            *trampolineOffset += PATCH_SIZE;
        }
#endif
        else {
            HookLog("%s: Cannot relocate external short JMP at %p (target=%p, x64=%d)",
                    ownerTag ? ownerTag : "InlineHook", reinterpret_cast<void*>(instrAddr),
                    reinterpret_cast<void*>(absTarget), is64bit ? 1 : 0);
            return ShortControlRelocationResult::kFailed;
        }

        HookLog("%s: Rewrote external short JMP at %p to target %p", ownerTag ? ownerTag : "InlineHook",
                reinterpret_cast<void*>(instrAddr), reinterpret_cast<void*>(absTarget));
        return ShortControlRelocationResult::kHandled;
    }

    const uint8_t condCode = static_cast<uint8_t>(opcode & 0x0F);
    const int64_t newDisp = static_cast<int64_t>(absTarget) -
                            static_cast<int64_t>(reinterpret_cast<uintptr_t>(trampoline + *trampolineOffset + 6));
    if (newDisp >= INT32_MIN && newDisp <= INT32_MAX) {
        trampoline[*trampolineOffset] = 0x0F;
        trampoline[*trampolineOffset + 1] = static_cast<uint8_t>(0x80 | condCode);
        const int32_t newDisp32 = static_cast<int32_t>(newDisp);
        memcpy(trampoline + *trampolineOffset + 2, &newDisp32, 4);
        *trampolineOffset += 6;
    }
#ifdef _WIN64
    else if (is64bit) {
        trampoline[*trampolineOffset] = static_cast<uint8_t>(0x70 | (condCode ^ 1u));
        trampoline[*trampolineOffset + 1] = static_cast<uint8_t>(PATCH_SIZE);
        WriteJump(trampoline + *trampolineOffset + 2, reinterpret_cast<void*>(absTarget));
        *trampolineOffset += 2 + PATCH_SIZE;
    }
#endif
    else {
        HookLog("%s: Cannot relocate external short Jcc opcode 0x%02X at %p (target=%p, x64=%d)",
                ownerTag ? ownerTag : "InlineHook", opcode, reinterpret_cast<void*>(instrAddr),
                reinterpret_cast<void*>(absTarget), is64bit ? 1 : 0);
        return ShortControlRelocationResult::kFailed;
    }

    HookLog("%s: Rewrote external short Jcc opcode 0x%02X at %p to target %p", ownerTag ? ownerTag : "InlineHook",
            opcode, reinterpret_cast<void*>(instrAddr), reinterpret_cast<void*>(absTarget));
    return ShortControlRelocationResult::kHandled;
}

}  // namespace InlineHook
