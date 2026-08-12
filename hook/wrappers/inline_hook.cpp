/**
 * Minimal Inline Hook Implementation
 *
 * Installs and removes entry-point detours. The instruction decoder lives in
 * inline_hook_lde.cpp, the trampoline pool and relocation helpers in
 * inline_hook_trampoline.cpp, and the deep/bypass variants in
 * inline_hook_deep.cpp.
 */

#include "inline_hook.h"
#include "inline_hook_internal.h"
#include "inline_hook_lde.h"
#include "inline_hook_policy.h"
#include "hook_patch_transaction.h"

#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "../common/hook_common.h"
#include "../common/overlay_compat.h"
#include "../../common/log_meter.h"

namespace InlineHook {

static void* ResolveExternalEntryJump(const uint8_t* code, bool is64bit) {
    if (!ce::inline_hook_policy::IsPrependChainableEntryJump(code[0], code[1], is64bit))
        return nullptr;
    uintptr_t target = 0;
    if (code[0] == 0xE9) {
        int32_t displacement = 0;
        memcpy(&displacement, code + 1, sizeof(displacement));
        target = reinterpret_cast<uintptr_t>(code + 5) + displacement;
    } else if (is64bit && code[0] == 0xFF && code[1] == 0x25) {
        int32_t displacement = 0;
        memcpy(&displacement, code + 2, sizeof(displacement));
        const uintptr_t pointerAddress = reinterpret_cast<uintptr_t>(code + 6) + displacement;
        MEMORY_BASIC_INFORMATION pointerMemory = {};
        if (VirtualQuery(reinterpret_cast<void*>(pointerAddress), &pointerMemory, sizeof(pointerMemory)) == 0 ||
            pointerMemory.State != MEM_COMMIT || (pointerMemory.Protect & (PAGE_NOACCESS | PAGE_GUARD))) {
            return nullptr;
        }
        memcpy(&target, reinterpret_cast<const void*>(pointerAddress), sizeof(target));
    }

    MEMORY_BASIC_INFORMATION targetMemory = {};
    if (!target || VirtualQuery(reinterpret_cast<void*>(target), &targetMemory, sizeof(targetMemory)) == 0 ||
        targetMemory.State != MEM_COMMIT ||
        !(targetMemory.Protect &
          (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
        return nullptr;
    }
    return reinterpret_cast<void*>(target);
}

static void WriteJumpWithoutLogging(uint8_t* destination, void* target) {
#ifdef _WIN64
    memcpy(destination + 6, static_cast<const void*>(&target), sizeof(target));
    MemoryBarrier();
    const uint8_t header[6] = {0xFF, 0x25, 0x00, 0x00, 0x00, 0x00};
    memcpy(destination, header, sizeof(header));
#else
    const int32_t displacement =
        static_cast<int32_t>(reinterpret_cast<uintptr_t>(target) - reinterpret_cast<uintptr_t>(destination + 5));
    memcpy(destination + 1, &displacement, sizeof(displacement));
    MemoryBarrier();
    destination[0] = 0xE9;
#endif
}

#ifdef _WIN64
static bool WriteNearJumpWithoutLogging(uint8_t* destination, void* target) {
    const int64_t displacement = static_cast<int64_t>(reinterpret_cast<uintptr_t>(target)) -
                                 static_cast<int64_t>(reinterpret_cast<uintptr_t>(destination + 5));
    if (displacement < INT32_MIN || displacement > INT32_MAX)
        return false;
    const int32_t displacement32 = static_cast<int32_t>(displacement);
    memcpy(destination + 1, &displacement32, sizeof(displacement32));
    MemoryBarrier();
    destination[0] = 0xE9;
    return true;
}
#endif

static bool WriteOwnedEntryPatch(void* target, void* detour, int patchSize, const uint8_t* expectedBytes,
                                 uint8_t* installedBytes) {
    ce::hook_patch::ThreadQuiescence quiescence(target, static_cast<size_t>(patchSize));
    if (!quiescence.IsReady())
        return false;
    if (memcmp(target, expectedBytes, patchSize) != 0)
        return false;
    DWORD oldProtect = 0;
    if (!VirtualProtect(target, patchSize, PAGE_EXECUTE_READWRITE, &oldProtect))
        return false;
#ifdef _WIN64
    if (patchSize == ce::inline_hook_policy::kExternalPrependPatchSize) {
        if (!WriteNearJumpWithoutLogging(static_cast<uint8_t*>(target), detour)) {
            DWORD ignoredProtect = 0;
            VirtualProtect(target, patchSize, oldProtect, &ignoredProtect);
            return false;
        }
    } else
#endif
    {
        WriteJumpWithoutLogging(static_cast<uint8_t*>(target), detour);
    }
    for (int i = PATCH_SIZE; i < patchSize; ++i)
        static_cast<uint8_t*>(target)[i] = 0x90;
    DWORD ignoredProtect = 0;
    VirtualProtect(target, patchSize, oldProtect, &ignoredProtect);
    FlushInstructionCache(GetCurrentProcess(), target, patchSize);
    memcpy(installedBytes, target, patchSize);
    return true;
}

static bool RestoreOwnedEntryPatch(const HookEntry& hook) {
    ce::hook_patch::ThreadQuiescence quiescence(hook.target, static_cast<size_t>(hook.patchSize));
    if (!quiescence.IsReady())
        return false;
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
}

// ============================================================================
// Public API
// ============================================================================

static bool InstallImpl(void* target, void* detour, void** outTrampoline, TrampolinePublisher publisher,
                        void* publisherContext) {
    // Use existing optional hook logger; avoid absolute-path file writes from injected code.
    auto LogDirect = [](const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        char buf[1024];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
        vsnprintf(buf, sizeof(buf), fmt, args);
#pragma GCC diagnostic pop
        va_end(args);
        HookLog("%s", buf);
    };

    auto TraceDirect = [](const char* fmt, ...) {
        if (!HookTraceLoggingEnabled()) {
            return;
        }
        va_list args;
        va_start(args, fmt);
        char buf[1024];
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wformat-nonliteral"
        vsnprintf(buf, sizeof(buf), fmt, args);
#pragma GCC diagnostic pop
        va_end(args);
        HookLog("%s", buf);
    };

    // Metered diagnostic: trace-level per-instruction byte dumps are valuable
    // for the first few hook installs but pure noise afterwards - one 90-second
    // trace session dumped ~2.5k byte lines across ~174 installs. Keep full
    // detail for the first 4 installs and then every 100th install as a
    // heartbeat; the compact per-hook lines (Hooking, Original bytes,
    // trampoline result) stay unconditional at trace level.
    static std::atomic<int> s_traceDetailHookCount{0};
    const bool fullHookTrace = HookTraceLoggingEnabled() &&
                               ce::log_meter::ShouldLogCadence(static_cast<uint32_t>(
                                                                   s_traceDetailHookCount.fetch_add(1, std::memory_order_relaxed) + 1),
                                                               4, 100);

    LogDirect("=== Install called: target=%p, detour=%p", target, detour);

    if (!target || !detour || !outTrampoline) {
        LogDirect("FAILED: null parameter");
        return false;
    }
    *outTrampoline = nullptr;

    std::lock_guard<std::mutex> lock(g_hookMutex);

    // Check if already hooked
    for (auto& h : g_hooks) {
        if (h.target == target && h.installed) {
            LogDirect("FAILED: Target %p already hooked by us", target);
            HookLog("InlineHook: Target %p already hooked", target);
            return false;
        }
    }

#ifdef _WIN64
    bool is64bit = true;
#else
    bool is64bit = false;
#endif

    TraceDirect("is64bit=%d, checking if externally hooked...", is64bit ? 1 : 0);

    // If another component owns a conventional entry jump, prepend CE at the
    // real export entry and make our trampoline forward to that exact jump
    // target. Never decode or patch inside the foreign detour body.
    const uint8_t* code = (const uint8_t*)target;

    // Dump first bytes of target
    // SECURITY FIX: Use safe string concatenation
    char firstBytes[64] = {0};
    size_t remaining = sizeof(firstBytes) - 1;
    char* dest = firstBytes;
    for (int i = 0; i < 8 && remaining > 3; i++) {
        int written = snprintf(dest, remaining, "%02X ", code[i]);
        if (written > 0 && (size_t)written < remaining) {
            dest += written;
            remaining -= written;
        }
    }
    LogDirect("First bytes of target: %s", firstBytes);

    if (IsAlreadyHooked(code, is64bit)) {
        void* chainedEntry = ResolveExternalEntryJump(code, is64bit);
        if (!chainedEntry) {
            LogDirect("FAILED: Existing entry patch at %p is not a chainable E9/FF25 jump", target);
            HookLog("InlineHook: Existing entry patch at %p is not safely chainable", target);
            return false;
        }

        uint8_t* trampoline = GetTrampolineSlot(target);
        if (!trampoline) {
            return false;
        }
        WriteJump(trampoline, chainedEntry);
#ifdef _WIN64
        // A Detours/RTSS E9 trampoline can resume at target+5. Keep every byte
        // after that existing jump intact: put CE's absolute jump in the same
        // near RX page and claim only five bytes at the export entry.
        uint8_t* prependTarget = trampoline + TRAMPOLINE_ALIGNMENT;
        WriteJump(prependTarget, detour);
        const int64_t relayDisplacement =
            static_cast<int64_t>(reinterpret_cast<uintptr_t>(prependTarget)) -
            static_cast<int64_t>(reinterpret_cast<uintptr_t>(target) +
                                 ce::inline_hook_policy::kExternalPrependPatchSize);
        if (relayDisplacement < INT32_MIN || relayDisplacement > INT32_MAX) {
            AbandonCurrentTrampoline();
            HookLogImportant("InlineHook: Could not allocate a near relay for external entry at %p", target);
            return false;
        }
        const size_t trampolineBytes = TRAMPOLINE_ALIGNMENT + PATCH_SIZE;
#else
        void* prependTarget = detour;
        const size_t trampolineBytes = PATCH_SIZE;
#endif
        if (!FinalizeCurrentTrampoline(trampoline, trampolineBytes)) {
            return false;
        }

        HookEntry entry = {};
        entry.target = target;
        entry.detour = detour;
        entry.trampoline = trampoline;
        entry.patchSize = ce::inline_hook_policy::kExternalPrependPatchSize;
        memcpy(entry.origBytes, code, entry.patchSize);
        try {
            g_hooks.push_back(entry);
        } catch (...) {
            HookLogImportant("InlineHook: Could not allocate ownership record for target %p", target);
            ReleaseSealedTrampoline(trampoline);
            return false;
        }
        *outTrampoline = trampoline;
        if (publisher) {
            publisher(trampoline, publisherContext);
        }

        if (!WriteOwnedEntryPatch(target, prependTarget, entry.patchSize, entry.origBytes,
                                  g_hooks.back().installedBytes)) {
            if (publisher) {
                publisher(nullptr, publisherContext);
            }
            *outTrampoline = nullptr;
            // Another established CE route can observe the published
            // trampoline before this entry-point claim finishes. Roll back the
            // pointer, but retain executable storage for any caller that
            // already acquired it.
            g_hooks.pop_back();
            if (publisher)
                HookLogImportant("InlineHook: Retaining rolled-back published trampoline %p", trampoline);
            else
                ReleaseSealedTrampoline(trampoline);
            return false;
        }
        g_hooks.back().installed = true;

        char ownerPath[MAX_PATH] = {};
        ce::overlay_compat::TryGetModulePathFromCodeAddress(chainedEntry, ownerPath, sizeof(ownerPath));
        HookLogImportant(
            "InlineHook: Prepended CE at %p while preserving external entry %p (owner=%s recognizedOverlay=%d)",
            target, chainedEntry, ownerPath[0] ? ownerPath : "unknown",
            ce::overlay_compat::IsThirdPartyOverlayModulePath(ownerPath) ? 1 : 0);
        return true;
    }

    LogDirect("Not externally hooked, decoding instructions...");

    // Determine how many bytes to copy (must be >= PATCH_SIZE on instruction
    // boundary)
    int copySize = 0;
    while (copySize < PATCH_SIZE) {
        int len = GetInstructionLength(code + copySize, is64bit);
        if (len == 0) {
            LogDirect("FAILED: Failed to decode instruction at %p+%d (byte=0x%02X)", target, copySize, code[copySize]);
            HookLog(
                "InlineHook: Failed to decode instruction at %p+%d "
                "(byte=0x%02X)",
                target, copySize, code[copySize]);
            return false;
        }
        copySize += len;
    }

    LogDirect("Instructions decoded, copySize=%d bytes", copySize);
    HookLog("InlineHook: Hooking %p, patch=%d bytes, detour=%p, is64bit=%d", target, copySize, detour, is64bit ? 1 : 0);

    // Dump original bytes for diagnosis
    // SECURITY FIX: Use safe string concatenation
    char bytesStr[256] = {0};
    size_t bytesRemaining = sizeof(bytesStr) - 1;
    char* bytesDest = bytesStr;
    for (int i = 0; i < copySize && i < 16 && bytesRemaining > 3; i++) {
        int written = snprintf(bytesDest, bytesRemaining, "%02X ", code[i]);
        if (written > 0 && (size_t)written < bytesRemaining) {
            bytesDest += written;
            bytesRemaining -= written;
        }
    }
    TraceDirect("Original bytes: %s", bytesStr);

    if (fullHookTrace) {
        HookLog("InlineHook: Original bytes at %p:", target);
        for (int i = 0; i < copySize && i < 16; i++) {
            HookLog("  [%02d] 0x%02X", i, code[i]);
        }
    }

    // Allocate trampoline
    LogDirect("Allocating trampoline...");
    uint8_t* trampoline = GetTrampolineSlot(target);
    if (!trampoline) {
        LogDirect("FAILED: Failed to allocate trampoline");
        HookLog("InlineHook: Failed to allocate trampoline");
        return false;
    }
    LogDirect("Trampoline allocated at %p", trampoline);
    HookLog("InlineHook: Trampoline allocated at %p", trampoline);

    // Copy original instructions to trampoline, fixing up RIP-relative refs
    int trampolineOffset = 0;
    int srcOffset = 0;
    // For CALL rel32→absolute: write FF 15 [placeholder] now, patch displacement after loop.
    uintptr_t pendingAbsCallTarget = 0;
    bool hasPendingAbsCall = false;
    int pendingCallInstrOffset = -1;  // trampoline offset where the FF 15 CALL was written
    while (srcOffset < copySize) {
        int instrLen = GetInstructionLength(code + srcOffset, is64bit);

        // Log instruction being copied
        if (fullHookTrace) {
            HookLog("InlineHook: Copying instruction at offset %d, len=%d:", srcOffset, instrLen);
            for (int i = 0; i < instrLen && i < 8; i++) {
                HookLog("  [%02d] 0x%02X", i, code[srcOffset + i]);
            }
        }

        const auto shortBranchResult = TryRelocateExternalShortControlTransfer(
            code + srcOffset, reinterpret_cast<uintptr_t>(code + srcOffset), instrLen,
            reinterpret_cast<uintptr_t>(code), copySize, trampoline, &trampolineOffset, is64bit, "InlineHook");
        if (shortBranchResult == ShortControlRelocationResult::kFailed) {
            AbandonCurrentTrampoline();
            return false;
        }
        if (shortBranchResult == ShortControlRelocationResult::kHandled) {
            srcOffset += instrLen;
            continue;
        }

        memcpy(trampoline + trampolineOffset, code + srcOffset, instrLen);

        // Fix up RIP-relative addressing
        int dispOff = GetRipRelativeDispOffset(code + srcOffset, instrLen, is64bit);
        if (dispOff >= 0) {
            // Read original displacement
            int32_t origDisp;
            memcpy(&origDisp, code + srcOffset + dispOff, 4);

            // Calculate absolute target address
            // RIP-relative: target = instruction_end + displacement
            uintptr_t absTarget = (uintptr_t)(code + srcOffset + instrLen) + origDisp;

            // Calculate new displacement from trampoline position
            uintptr_t newInstrEnd = (uintptr_t)(trampoline + trampolineOffset + instrLen);
            int64_t newDisp = (int64_t)absTarget - (int64_t)newInstrEnd;

            if (fullHookTrace) {
                HookLog(
                    "InlineHook: PC-relative fixup at srcOff=%d, dispOff=%d, "
                    "origDisp=0x%08X, absTarget=%p, newInstrEnd=%p, newDisp=0x%08llX",
                    srcOffset, dispOff, (unsigned)origDisp, (void*)absTarget, (void*)newInstrEnd, (long long)newDisp);
            }

            if (newDisp > INT32_MAX || newDisp < INT32_MIN) {
                // Check if this is a JMP rel32 (0xE9) or CALL rel32 (0xE8) that we can convert to absolute
                uint8_t opcode = code[srcOffset];
                if (opcode == 0xE9 || opcode == 0xE8) {
                    HookLog("InlineHook: Converting %s rel32 to absolute at offset %d (target=%p)",
                            opcode == 0xE9 ? "JMP" : "CALL", srcOffset, (void*)absTarget);

                    if (opcode == 0xE9) {
                        // JMP: FF 25 00 00 00 00 [8-byte address] (14 bytes).
                        // Never returns, so pointer-after-instruction layout is fine.
                        trampoline[trampolineOffset] = 0xFF;
                        trampoline[trampolineOffset + 1] = 0x25;
                        trampoline[trampolineOffset + 2] = 0x00;
                        trampoline[trampolineOffset + 3] = 0x00;
                        trampoline[trampolineOffset + 4] = 0x00;
                        trampoline[trampolineOffset + 5] = 0x00;
                        memcpy(trampoline + trampolineOffset + 6, &absTarget, 8);
                        trampolineOffset += 14;
                    } else {
                        // CALL: write FF 15 with a placeholder displacement; we patch the
                        // real disp AFTER the loop+WriteJump when we know the ptr location.
                        // Return address = trampolineOffset+6 (post-fetch RIP).
                        // Ptr is written after WriteJump; disp is patched at that point.
                        pendingCallInstrOffset = trampolineOffset;
                        trampoline[trampolineOffset] = 0xFF;
                        trampoline[trampolineOffset + 1] = 0x15;
                        trampoline[trampolineOffset + 2] = 0;  // placeholder
                        trampoline[trampolineOffset + 3] = 0;
                        trampoline[trampolineOffset + 4] = 0;
                        trampoline[trampolineOffset + 5] = 0;
                        trampolineOffset += 6;  // only the CALL instruction; pointer written later
                        pendingAbsCallTarget = absTarget;
                        hasPendingAbsCall = true;
                    }
                    srcOffset += instrLen;
                    continue;  // Skip the normal fixup path
                }

                HookLog("InlineHook: RIP-relative fixup out of range at %p+%d (opcode=0x%02X)", target, srcOffset,
                        opcode);
                AbandonCurrentTrampoline();
                return false;
            }

            int32_t newDisp32 = (int32_t)newDisp;
            memcpy(trampoline + trampolineOffset + dispOff, &newDisp32, 4);
        } else {
            if (fullHookTrace) {
                HookLog(
                    "InlineHook: No PC-relative fixup needed for instruction at "
                    "offset %d",
                    srcOffset);
            }
        }

        trampolineOffset += instrLen;
        srcOffset += instrLen;
    }

    // Add jump back to original function after the patched area
    void* jumpTarget = (void*)(code + copySize);
    HookLog("InlineHook: Writing jump back from trampoline+%d to %p (original+%d)", trampolineOffset, jumpTarget,
            copySize);
    WriteJump(trampoline + trampolineOffset, jumpTarget);
    trampolineOffset += PATCH_SIZE;

    // If a CALL rel32→absolute was converted, its target pointer goes here
    // (after the WriteJump), so the CALL return address correctly falls through
    // to the WriteJump continuation above.
    if (hasPendingAbsCall) {
        // Ptr lands here (after WriteJump). Patch the displacement back into the
        // FF 15 instruction: disp = ptrOffset - returnAddrOffset
        //   returnAddr = trampoline + pendingCallInstrOffset + 6  (post-fetch RIP)
        //   ptrOffset   = trampoline + trampolineOffset
        int32_t disp = trampolineOffset - (pendingCallInstrOffset + 6);
        memcpy(trampoline + pendingCallInstrOffset + 2, &disp, 4);
        memcpy(trampoline + trampolineOffset, &pendingAbsCallTarget, 8);
        trampolineOffset += 8;
    }

    // Dump trampoline bytes for diagnosis
    if (fullHookTrace) {
        HookLog("InlineHook: Trampoline bytes (%d bytes total):", trampolineOffset);
        for (int i = 0; i < trampolineOffset && i < 32; i++) {
            HookLog("  [%02d] 0x%02X", i, trampoline[i]);
        }
    }

    // Seal the private page RX and, under CFG, make this 16-byte-aligned entry
    // the page's only valid indirect-call target before patching live code.
    if (!FinalizeCurrentTrampoline(trampoline, static_cast<size_t>(trampolineOffset))) {
        LogDirect("FAILED: Could not seal/register trampoline entrypoint");
        HookLog("InlineHook: Trampoline RX/CFG finalization failed");
        return false;
    }

    // Save original bytes
    HookEntry entry = {};
    entry.target = target;
    entry.detour = detour;
    entry.trampoline = trampoline;
    entry.patchSize = copySize;
    entry.installed = false;
    memcpy(entry.origBytes, code, copySize);

    try {
        g_hooks.push_back(entry);
    } catch (...) {
        LogDirect("FAILED: Could not allocate ownership record for target %p", target);
        ReleaseSealedTrampoline(trampoline);
        return false;
    }

    *outTrampoline = trampoline;
    if (publisher) {
        // Publish the only safe bypass before any thread can observe the live
        // detour. Publication is harmless while the original entry remains
        // unpatched and closes the installer-return race for fatal hooks.
        publisher(trampoline, publisherContext);
    }

    LogDirect("Patching target function with peer threads quiesced...");
    if (!WriteOwnedEntryPatch(target, detour, copySize, entry.origBytes, g_hooks.back().installedBytes)) {
        LogDirect("FAILED: Could not safely patch target %p", target);
        if (publisher) {
            publisher(nullptr, publisherContext);
        }
        *outTrampoline = nullptr;
        // Publication precedes the live patch by design. A concurrent vtable
        // detour may already be executing this safe bypass, so its RX page is
        // retained even though new callers see the restored fallback.
        g_hooks.pop_back();
        if (publisher)
            HookLogImportant("InlineHook: Retaining rolled-back published trampoline %p", trampoline);
        else
            ReleaseSealedTrampoline(trampoline);
        return false;
    }

    g_hooks.back().installed = true;

    LogDirect("SUCCESS: Hook installed at %p -> %p (trampoline=%p)", target, detour, trampoline);
    HookLog("InlineHook: Installed hook at %p -> %p (trampoline=%p)", target, detour, trampoline);
    return true;
}

bool Install(void* target, void* detour, void** outTrampoline) {
    return InstallImpl(target, detour, outTrampoline, nullptr, nullptr);
}

bool InstallPublished(void* target, void* detour, void** outTrampoline, TrampolinePublisher publisher,
                      void* publisherContext) {
    if (!publisher) {
        return false;
    }
    return InstallImpl(target, detour, outTrampoline, publisher, publisherContext);
}

bool IsInstalledEntryPatchIntact(void* target, void** currentJumpTargetOut) {
    if (currentJumpTargetOut) {
        *currentJumpTargetOut = nullptr;
    }
    if (!target) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_hookMutex);
    for (const auto& h : g_hooks) {
        if (h.target != target || !h.installed) {
            continue;
        }
        if (memcmp(h.target, h.installedBytes, h.patchSize) == 0) {
            return true;
        }
#ifdef _WIN64
        constexpr bool is64bit = true;
#else
        constexpr bool is64bit = false;
#endif
        if (currentJumpTargetOut) {
            *currentJumpTargetOut = ResolveExternalEntryJump(static_cast<const uint8_t*>(target), is64bit);
        }
        return false;
    }
    return false;
}

static bool OwnsInstalledEntryBytes(const HookEntry& hook) {
    MEMORY_BASIC_INFORMATION memory = {};
    return hook.target && hook.patchSize > 0 &&
           VirtualQuery(hook.target, &memory, sizeof(memory)) == sizeof(memory) && memory.State == MEM_COMMIT &&
           !(memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) &&
           ce::inline_hook_policy::ShouldRestoreOwnedPatch(
               memcmp(hook.target, hook.installedBytes, hook.patchSize) == 0);
}

bool Remove(void* target) {
    if (!target)
        return false;

    std::lock_guard<std::mutex> lock(g_hookMutex);

    for (auto& h : g_hooks) {
        if (h.target == target && h.installed) {
            if (!OwnsInstalledEntryBytes(h)) {
                HookLogImportant(
                    "InlineHook: Preserving foreign replacement at %p and retaining CE chain ownership",
                    target);
                return true;
            }
            if (RestoreOwnedEntryPatch(h)) {
                h.installed = false;
                HookLog("InlineHook: Removed hook at %p", target);
                return true;
            }
            if (!OwnsInstalledEntryBytes(h)) {
                HookLogImportant(
                    "InlineHook: Preserving concurrent foreign replacement at %p and retaining CE chain ownership",
                    target);
                return true;
            }
            HookLog(
                "InlineHook: Failed to quiesce peer threads while removing hook at %p; leaving it installed", target);
            return false;
        }
    }

    HookLog("InlineHook: No hook found at %p", target);
    return false;
}

void RemoveAll() {
    std::lock_guard<std::mutex> lock(g_hookMutex);

    HookLog("InlineHook::RemoveAll() called - removing %zu hooks", g_hooks.size());

    for (auto& h : g_hooks) {
        if (h.installed) {
            if (!OwnsInstalledEntryBytes(h)) {
                HookLogImportant(
                    "InlineHook: RemoveAll preserved foreign replacement at %p and retained CE chain ownership",
                    h.target);
                continue;
            }
            if (RestoreOwnedEntryPatch(h))
                h.installed = false;
            else if (!OwnsInstalledEntryBytes(h)) {
                HookLogImportant(
                    "InlineHook: RemoveAll preserved concurrent foreign replacement at %p and retained chain state",
                    h.target);
            } else
                HookLog("InlineHook: RemoveAll could not safely quiesce %p; leaving CE hook installed", h.target);
        }
    }
    g_hooks.erase(std::remove_if(g_hooks.begin(), g_hooks.end(),
                                 [](const HookEntry& hook) { return !hook.installed; }),
                  g_hooks.end());

    RemoveAllDeepHooksLocked();

    HookLog("InlineHook::RemoveAll() - retaining %zu trampoline pools for foreign saved-chain safety",
            g_trampolinePools.size());
}

}  // namespace InlineHook
