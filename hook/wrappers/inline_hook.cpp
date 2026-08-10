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

#include <windows.h>
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>
#include "../common/hook_common.h"
#include "../../common/log_meter.h"

namespace InlineHook {

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

    // CRITICAL: Check if the target function appears to already be hooked
    // by another component (another DLL, another process, overlay, etc.)
    // CRITICAL FIX: Do NOT attempt to restore the prologue - the guessing is
    // unreliable and causes black screens when the guess is wrong.
    // Instead, skip hooking entirely and let the external overlay handle things.
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
        // External overlay detected - try chain hooking
        // Resolve the chain target from either a JMP rel32 (E9) or indirect JMP (FF 25)
        LogDirect("External hook detected, attempting chain hooking...");

        uintptr_t chainTarget = 0;

        if (code[0] == 0xE9) {
            // JMP rel32 - valid on both x86 and x64
            int32_t rel32 = *(const int32_t*)(code + 1);
            uintptr_t overlayTarget = (uintptr_t)target + 5 + rel32;
            LogDirect("JMP rel32 detected: rel32=0x%08X, overlay target=%p", (unsigned)rel32, (void*)overlayTarget);
            HookLog("InlineHook: Chaining to overlay hook at %p", (void*)overlayTarget);

            // Follow one more level of JMP rel32 if present (multi-level chaining)
            const uint8_t* overlayCode = (const uint8_t*)overlayTarget;
            if (overlayCode[0] == 0xE9) {
                LogDirect("Overlay target also has JMP, following chain...");
                int32_t rel32_2 = *(const int32_t*)(overlayCode + 1);
                uintptr_t finalTarget = overlayTarget + 5 + rel32_2;
                LogDirect("Second JMP target: %p", (void*)finalTarget);
                overlayTarget = finalTarget;
            }
            chainTarget = overlayTarget;
        } else if (is64bit && code[0] == 0xFF && code[1] == 0x25) {
            // JMP [rip+disp32] on x64 - dereference indirect pointer to find real target
            int32_t disp;
            memcpy(&disp, code + 2, 4);
            uintptr_t addrPtr = (uintptr_t)(code + 6) + disp;
            MEMORY_BASIC_INFORMATION mbiFF;
            if (VirtualQuery((void*)addrPtr, &mbiFF, sizeof(mbiFF)) > 0 && mbiFF.State == MEM_COMMIT &&
                (mbiFF.Protect & (PAGE_READONLY | PAGE_READWRITE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE))) {
                chainTarget = *(uintptr_t*)addrPtr;
                LogDirect("JMP [rip+disp32] chain: indirect addr=%p, real target=%p", (void*)addrPtr,
                          (void*)chainTarget);
            } else {
                LogDirect("FAILED: Cannot read FF25 indirect address %p", (void*)addrPtr);
            }
        }

        if (chainTarget != 0) {
            // Verify the chain target is executable
            MEMORY_BASIC_INFORMATION mbi;
            if (VirtualQuery((void*)chainTarget, &mbi, sizeof(mbi)) == sizeof(mbi)) {
                LogDirect("Overlay target memory: Base=%p, Protect=0x%X", mbi.BaseAddress, mbi.Protect);

                // CRITICAL: Check if chain target is inside a known overlay module.
                // Installing a hook inside Steam/discord overlay code causes infinite recursion
                // because the overlay's trampoline calls back into the original function.
                HMODULE hModule = nullptr;
                if (GetModuleHandleExA(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCSTR)chainTarget, &hModule)) {
                    char moduleName[MAX_PATH] = {};
                    GetModuleFileNameA(hModule, moduleName, MAX_PATH);
                    std::string modLower(moduleName);
                    std::transform(modLower.begin(), modLower.end(), modLower.begin(),
                                   [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });
                    if (modLower.find("gameoverlayrenderer") != std::string::npos ||
                        modLower.find("d3doverlay") != std::string::npos ||
                        modLower.find("discord") != std::string::npos || modLower.find("nvidia") != std::string::npos ||
                        modLower.find("amd") != std::string::npos) {
                        LogDirect("Chain target is inside overlay module %s - skipping to avoid recursion", moduleName);
                        HookLog("InlineHook: Skipping chain hook into overlay module %s (would cause recursion)",
                                moduleName);
                        LogDirect("FAILED: Function at %p is already hooked by external overlay", target);
                        HookLog("InlineHook: Function at %p is already hooked by external overlay", target);
                        HookLog("InlineHook: Chain hooking skipped (overlay module protection)");
                        return false;
                    }
                }

                if (mbi.State == MEM_COMMIT && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ |
                                                               PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY))) {
                    LogDirect("Overlay target is executable, attempting hook...");

                    if (!IsAlreadyHooked((const uint8_t*)chainTarget, is64bit)) {
                        LogDirect("Attempting chain install at overlay target %p", (void*)chainTarget);

                        // Decode enough instructions to cover PATCH_SIZE bytes
                        int chainCopySize = 0;
                        const uint8_t* chainCode = (const uint8_t*)chainTarget;
                        while (chainCopySize < PATCH_SIZE) {
                            int len = GetInstructionLength(chainCode + chainCopySize, is64bit);
                            if (len == 0) {
                                LogDirect("FAILED: Cannot decode instruction at overlay target+%d", chainCopySize);
                                break;
                            }
                            chainCopySize += len;
                        }

                        if (chainCopySize >= PATCH_SIZE) {
                            LogDirect("Chain hook: copySize=%d, installing at %p", chainCopySize, (void*)chainTarget);

                            // Allocate trampoline near chain target for RIP-relative safety
                            uint8_t* chainTrampoline = GetTrampolineSlot((void*)chainTarget);
                            if (!chainTrampoline) {
                                LogDirect("FAILED: Could not allocate trampoline for chain hook");
                                return false;
                            }
                            LogDirect("Chain hook trampoline allocated at %p", chainTrampoline);

                            // Copy instructions to trampoline with RIP-relative fixups.
                            // On x64 instructions may reference data via RIP+disp32; the displacement
                            // must be recomputed when the instruction executes from a different address.
                            int trampolineOff = 0;
                            int srcOff = 0;
                            bool fixupFailed = false;
                            uintptr_t chainPendingAbsCallTarget = 0;
                            bool chainHasPendingAbsCall = false;
                            int chainPendingCallInstrOff = -1;
                            while (srcOff < chainCopySize) {
                                int instrLen = GetInstructionLength(chainCode + srcOff, is64bit);

                                const auto shortBranchResult = TryRelocateExternalShortControlTransfer(
                                    chainCode + srcOff, reinterpret_cast<uintptr_t>(chainCode + srcOff), instrLen,
                                    reinterpret_cast<uintptr_t>(chainCode), chainCopySize, chainTrampoline,
                                    &trampolineOff, is64bit, "InlineHook(chain)");
                                if (shortBranchResult == ShortControlRelocationResult::kFailed) {
                                    LogDirect("Chain hook: short control relocation failed at srcOff=%d", srcOff);
                                    fixupFailed = true;
                                    break;
                                }
                                if (shortBranchResult == ShortControlRelocationResult::kHandled) {
                                    srcOff += instrLen;
                                    continue;
                                }

                                memcpy(chainTrampoline + trampolineOff, chainCode + srcOff, instrLen);

                                int dispOff = GetRipRelativeDispOffset(chainCode + srcOff, instrLen, is64bit);
                                if (dispOff >= 0) {
                                    int32_t origDisp;
                                    memcpy(&origDisp, chainCode + srcOff + dispOff, 4);
                                    uintptr_t absTarget = (uintptr_t)(chainCode + srcOff + instrLen) + origDisp;
                                    uintptr_t newInstrEnd = (uintptr_t)(chainTrampoline + trampolineOff + instrLen);
                                    int64_t newDisp = (int64_t)absTarget - (int64_t)newInstrEnd;

                                    if (newDisp > INT32_MAX || newDisp < INT32_MIN) {
                                        uint8_t op = chainCode[srcOff];
                                        if (op == 0xE9 || op == 0xE8) {
                                            if (op == 0xE9) {
                                                // JMP: FF 25 00 00 00 00 [8-byte address]
                                                chainTrampoline[trampolineOff] = 0xFF;
                                                chainTrampoline[trampolineOff + 1] = 0x25;
                                                chainTrampoline[trampolineOff + 2] = 0;
                                                chainTrampoline[trampolineOff + 3] = 0;
                                                chainTrampoline[trampolineOff + 4] = 0;
                                                chainTrampoline[trampolineOff + 5] = 0;
                                                memcpy(chainTrampoline + trampolineOff + 6, &absTarget, 8);
                                                trampolineOff += 14;
                                            } else {
                                                // CALL: write FF 15 with placeholder; patch disp
                                                // after loop+JMP-back when ptr location is known.
                                                chainPendingCallInstrOff = trampolineOff;
                                                chainTrampoline[trampolineOff] = 0xFF;
                                                chainTrampoline[trampolineOff + 1] = 0x15;
                                                chainTrampoline[trampolineOff + 2] = 0;  // placeholder
                                                chainTrampoline[trampolineOff + 3] = 0;
                                                chainTrampoline[trampolineOff + 4] = 0;
                                                chainTrampoline[trampolineOff + 5] = 0;
                                                trampolineOff += 6;
                                                chainPendingAbsCallTarget = absTarget;
                                                chainHasPendingAbsCall = true;
                                            }
                                            srcOff += instrLen;
                                            continue;
                                        }
                                        LogDirect("Chain hook: RIP fixup out of range at srcOff=%d, aborting", srcOff);
                                        fixupFailed = true;
                                        break;
                                    }
                                    int32_t newDisp32 = (int32_t)newDisp;
                                    memcpy(chainTrampoline + trampolineOff + dispOff, &newDisp32, 4);
                                }
                                trampolineOff += instrLen;
                                srcOff += instrLen;
                            }
                            if (fixupFailed || trampolineOff > static_cast<int>(TRAMPOLINE_ENTRY_SIZE) - 5) {
                                LogDirect("Chain hook: trampoline build failed (off=%d)", trampolineOff);
                                AbandonCurrentTrampoline();
                                return false;
                            }

                            // Add JMP back to chain code after the copied bytes.
                            // E9 rel32 is safe: trampoline is within ±2GB of chainTarget.
                            uint8_t* jmpSite = chainTrampoline + trampolineOff;
                            jmpSite[0] = 0xE9;  // JMP rel32
                            int32_t jmpOffset =
                                (int32_t)((uintptr_t)(chainCode + chainCopySize) - (uintptr_t)(jmpSite + 5));
                            memcpy(jmpSite + 1, &jmpOffset, 4);
                            // If a CALL abs conversion was deferred, its ptr goes after the
                            // E9 JMP-back. Patch the displacement in the FF 15 instruction now
                            // that we know both locations.
                            if (chainHasPendingAbsCall) {
                                uint8_t* ptrAddr = jmpSite + 5;  // right after the 5-byte E9 JMP
                                int32_t disp =
                                    (int32_t)((uint8_t*)ptrAddr - (chainTrampoline + chainPendingCallInstrOff + 6));
                                memcpy(chainTrampoline + chainPendingCallInstrOff + 2, &disp, 4);
                                memcpy(ptrAddr, &chainPendingAbsCallTarget, 8);
                            }
                            LogDirect("Chain trampoline: %d src bytes -> %d trampoline bytes, JMP -> %p (rel=0x%08X)",
                                      chainCopySize, trampolineOff, (void*)(chainCode + chainCopySize),
                                      (unsigned)jmpOffset);
                            const size_t chainTrampolineBytes = static_cast<size_t>(trampolineOff) + 5 +
                                                                (chainHasPendingAbsCall ? 8u : 0u);
                            if (!FinalizeCurrentTrampoline(chainTrampoline, chainTrampolineBytes)) {
                                LogDirect("Chain hook: trampoline RX/CFG finalization failed");
                                return false;
                            }

                            // Create the hook entry
                            HookEntry newHook = {};
                            newHook.target = (void*)chainTarget;
                            newHook.trampoline = chainTrampoline;
                            newHook.patchSize = chainCopySize;
                            newHook.installed = false;
                            memcpy(newHook.origBytes, chainCode, chainCopySize);

                            // Save trampoline to caller's storage
                            if (outTrampoline) {
                                *outTrampoline = chainTrampoline;
                            }
                            if (publisher) {
                                publisher(chainTrampoline, publisherContext);
                            }

                            // Patch chain target: use WriteJump for proper x64 support
                            // (14-byte FF25+addr on x64, 5-byte E9 rel32 on x86)
                            DWORD oldProtect;
                            if (VirtualProtect((void*)chainTarget, chainCopySize, PAGE_EXECUTE_READWRITE,
                                               &oldProtect)) {
                                WriteJump((uint8_t*)chainTarget, detour);
                                // NOP fill remaining bytes beyond PATCH_SIZE
                                for (int i = PATCH_SIZE; i < chainCopySize; i++) {
                                    ((uint8_t*)chainTarget)[i] = 0x90;
                                }

                                FlushInstructionCache(GetCurrentProcess(), (void*)chainTarget, chainCopySize);

                                DWORD dummy;
                                VirtualProtect((void*)chainTarget, chainCopySize, oldProtect, &dummy);

                                newHook.installed = true;
                                g_hooks.push_back(newHook);

                                LogDirect("SUCCESS: Chain hook installed at %p -> %p (trampoline=%p)",
                                          (void*)chainTarget, detour, chainTrampoline);
                                HookLog("InlineHook: Chain hook installed at %p (trampoline=%p)", (void*)chainTarget,
                                        chainTrampoline);
                                return true;
                            } else {
                                LogDirect("FAILED: VirtualProtect failed for chain target");
                                if (publisher) {
                                    publisher(nullptr, publisherContext);
                                }
                                *outTrampoline = nullptr;
                                ReleaseSealedTrampoline(chainTrampoline);
                            }
                        }
                    } else {
                        LogDirect("Overlay target also appears hooked, skipping");
                    }
                } else {
                    LogDirect("Overlay target is NOT executable (Protect=0x%X)", mbi.Protect);
                }
            } else {
                LogDirect("VirtualQuery failed for overlay target");
            }
        }

        LogDirect("FAILED: Function at %p is already hooked by external overlay", target);
        HookLog("InlineHook: Function at %p is already hooked by external overlay", target);
        HookLog("InlineHook: Chain hooking failed or not supported");
        return false;
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
    entry.trampoline = trampoline;
    entry.patchSize = copySize;
    entry.installed = true;
    memcpy(entry.origBytes, code, copySize);

    if (publisher) {
        // Publish the only safe bypass before any thread can observe the live
        // detour. Publication is harmless while the original entry remains
        // unpatched and closes the installer-return race for fatal hooks.
        publisher(trampoline, publisherContext);
    }

    LogDirect("Patching target function...");
    // Patch the target function
    DWORD oldProtect;
    if (!VirtualProtect(target, copySize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        LogDirect("FAILED: VirtualProtect failed (error=%lu)", GetLastError());
        HookLog("InlineHook: VirtualProtect failed (error=%lu)", GetLastError());
        if (publisher) {
            publisher(nullptr, publisherContext);
        }
        ReleaseSealedTrampoline(trampoline);
        return false;
    }
    LogDirect("VirtualProtect succeeded, oldProtect=0x%08X", oldProtect);

    // CRITICAL ORDER: For x64, write the 8-byte absolute address FIRST,
    // then the 6-byte JMP [RIP+0] header. This eliminates the race where a
    // concurrent thread executing the target function decodes a partial JMP
    // and jumps through zeros. The trampoline is already fully built and
    // cache-flushed before we touch the target.
    volatile uint8_t* pTarget = (volatile uint8_t*)target;
#ifdef _WIN64
    // x64: Use absolute jump via [RIP+0] - 14 bytes total
    // FF 25 00 00 00 00 [8-byte absolute address]
    uint8_t jmpBuf[14];
    jmpBuf[0] = 0xFF;
    jmpBuf[1] = 0x25;
    jmpBuf[2] = 0x00;
    jmpBuf[3] = 0x00;
    jmpBuf[4] = 0x00;
    jmpBuf[5] = 0x00;
    memcpy(jmpBuf + 6, reinterpret_cast<const void*>(&detour), 8);

    // Write the 8-byte target address first, so a concurrent thread that
    // sees a partial JMP [RIP+0] header reads the correct target from dest+6.
    memcpy((void*)(pTarget + 6), jmpBuf + 6, 8);
    MemoryBarrier();
    // Now write the 6-byte JMP header (FF 25 + disp32=0)
    memcpy((void*)pTarget, jmpBuf, 6);
#else
    // x86: Calculate displacement for the actual target location.
    // On x86 the E9 rel32 is only 5 bytes. Write the 4-byte displacement
    // FIRST so a concurrent thread that sees a partial E9 reads a correct
    // (or near-correct) displacement from a 4-byte natural write.
    pTarget[0] = 0xE9;  // JMP rel32 opcode
    int32_t rel = (int32_t)((uintptr_t)detour - (uintptr_t)((uint8_t*)target + 5));
    // Write displacement atomically (32-bit aligned), then the opcode.
    *(int32_t*)(pTarget + 1) = rel;
    MemoryBarrier();
    pTarget[0] = 0xE9;  // re-write opcode in case MemoryBarrier changed it
    HookLog("InlineHook: Target JMP at %p -> %p (rel=0x%08X)", target, detour, (unsigned)rel);
    // Verify the calculation
    uintptr_t verify = (uintptr_t)((uint8_t*)target + 5) + rel;
    HookLog("InlineHook: Verification: %p + 5 + 0x%08X = %p (expected %p)", target, (unsigned)rel, (void*)verify,
            detour);
#endif

    // Fill any remaining bytes after the jump with NOPs
    for (int i = PATCH_SIZE; i < copySize; i++) {
        pTarget[i] = 0x90;
    }

    VirtualProtect(target, copySize, PAGE_EXECUTE_READ, &oldProtect);
    FlushInstructionCache(GetCurrentProcess(), target, copySize);

    g_hooks.push_back(entry);
    *outTrampoline = trampoline;

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

bool Remove(void* target) {
    if (!target)
        return false;

    std::lock_guard<std::mutex> lock(g_hookMutex);

    for (auto& h : g_hooks) {
        if (h.target == target && h.installed) {
            DWORD oldProtect;
            if (VirtualProtect(h.target, h.patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                memcpy(h.target, h.origBytes, h.patchSize);
                VirtualProtect(h.target, h.patchSize, oldProtect, &oldProtect);
                FlushInstructionCache(GetCurrentProcess(), h.target, h.patchSize);
                h.installed = false;
                HookLog("InlineHook: Removed hook at %p", target);
                return true;
            }
            HookLog(
                "InlineHook: Failed to remove hook at %p (VirtualProtect "
                "error=%lu)",
                target, GetLastError());
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
            DWORD oldProtect;
            if (VirtualProtect(h.target, h.patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                memcpy(h.target, h.origBytes, h.patchSize);
                VirtualProtect(h.target, h.patchSize, oldProtect, &oldProtect);
                FlushInstructionCache(GetCurrentProcess(), h.target, h.patchSize);
            }
            h.installed = false;
        }
    }
    g_hooks.clear();

    // Also remove deep hooks
    for (auto& d : g_deepHooks) {
        if (d.installed) {
            DWORD oldProtect;
            if (VirtualProtect(d.hookAddr, d.patchSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                memcpy(d.hookAddr, d.origBytes, d.patchSize);
                VirtualProtect(d.hookAddr, d.patchSize, oldProtect, &oldProtect);
                FlushInstructionCache(GetCurrentProcess(), d.hookAddr, d.patchSize);
            }
            d.installed = false;
        }
        if (d.trampoline) {
            VirtualFree(d.trampoline, 0, MEM_RELEASE);
            d.trampoline = nullptr;
        }
    }
    g_deepHooks.clear();

    if (!g_trampolinePools.empty()) {
        for (uint8_t* pool : g_trampolinePools) {
            if (pool) {
                HookLog("InlineHook::RemoveAll() - freeing trampoline pool at %p", pool);
                VirtualFree(pool, 0, MEM_RELEASE);
            }
        }
        g_trampolinePools.clear();
    } else if (g_trampolinePool) {
        // Backward compatibility for any pool not tracked in the vector.
        HookLog("InlineHook::RemoveAll() - freeing trampoline pool at %p", g_trampolinePool);
        VirtualFree(g_trampolinePool, 0, MEM_RELEASE);
    }
    g_trampolinePool = nullptr;
    g_trampolineOffset = 0;
}

}  // namespace InlineHook
