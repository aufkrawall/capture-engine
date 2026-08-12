/**
 * Inline Hook — deep hooks and bypass trampolines
 *
 * Patches a function body past an external entry-point JMP so that callers
 * using a foreign saved trampoline are still routed through our wrapper, and
 * builds read-only bypass trampolines that execute the real body without
 * modifying the target.
 *
 * Split out of inline_hook.cpp; see inline_hook_internal.h for the shared
 * surface.
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

namespace InlineHook {

// ============================================================================
// Deep Hook Implementation
// ============================================================================
//
// A "deep hook" patches the function body PAST an external hook's JMP patch.
// This catches callers that use saved trampolines (e.g. Streamline's internal
// DXGI calls) which bypass the JMP at byte 0.
//
// The hook undoes the initial push prolog and redirects to a caller-provided
// wrapper function with the same calling convention as the original. A full
// trampoline is built containing the complete original prolog, allowing the
// wrapper to call through to the real function and capture the return value.

// Read original (unpatched) function bytes from the DLL file on disk.
static bool ReadOrigBytesFromDisk(void* funcAddr, uint8_t* outBuf, int count) {
    HMODULE hMod = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            (LPCSTR)funcAddr, &hMod) ||
        !hMod) {
        return false;
    }

    char modPath[MAX_PATH];
    if (!GetModuleFileNameA(hMod, modPath, MAX_PATH))
        return false;

    uintptr_t rva = (uintptr_t)funcAddr - (uintptr_t)hMod;

    HANDLE hFile =
        CreateFileA(modPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
    if (hFile == INVALID_HANDLE_VALUE)
        return false;

    bool success = false;
    DWORD br;

    IMAGE_DOS_HEADER dosH;
    if (!ReadFile(hFile, &dosH, sizeof(dosH), &br, nullptr) || dosH.e_magic != IMAGE_DOS_SIGNATURE) {
        CloseHandle(hFile);
        return false;
    }

    if (SetFilePointer(hFile, dosH.e_lfanew, nullptr, FILE_BEGIN) == INVALID_SET_FILE_POINTER) {
        CloseHandle(hFile);
        return false;
    }

    DWORD sig;
    if (!ReadFile(hFile, &sig, 4, &br, nullptr) || sig != IMAGE_NT_SIGNATURE) {
        CloseHandle(hFile);
        return false;
    }

    IMAGE_FILE_HEADER fh;
    if (!ReadFile(hFile, &fh, sizeof(fh), &br, nullptr)) {
        CloseHandle(hFile);
        return false;
    }

    // Skip optional header to reach section headers
    if (SetFilePointer(hFile, fh.SizeOfOptionalHeader, nullptr, FILE_CURRENT) == INVALID_SET_FILE_POINTER) {
        CloseHandle(hFile);
        return false;
    }

    for (WORD i = 0; i < fh.NumberOfSections; i++) {
        IMAGE_SECTION_HEADER sh;
        if (!ReadFile(hFile, &sh, sizeof(sh), &br, nullptr))
            break;

        if (rva >= sh.VirtualAddress && rva < sh.VirtualAddress + sh.Misc.VirtualSize) {
            DWORD fileOff = sh.PointerToRawData + (DWORD)(rva - sh.VirtualAddress);
            // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
            if (SetFilePointer(hFile, fileOff, nullptr, FILE_BEGIN) != INVALID_SET_FILE_POINTER) {
                if (ReadFile(hFile, outBuf, (DWORD)count, &br, nullptr) && (int)br == count)
                    success = true;
            }
            break;
        }
    }

    CloseHandle(hFile);
    return success;
}

struct VerifiedResumeOffset {
    int resumeOffset = 0;
    int firstCandidateOffset = 0;
};

static bool TryFindVerifiedExternalHookResumeOffset(const char* context, const uint8_t* liveCode,
                                                    const uint8_t* origDiskBytes, int availableBytes,
                                                    int existingJmpSize, bool is64bit, VerifiedResumeOffset* result) {
    constexpr int kCompareBytes = 8;
    constexpr int kMaxResumeScanBytes = 48;
    int resumeOffset = 0;
    int firstCandidateOffset = 0;

    while (resumeOffset < kMaxResumeScanBytes) {
        int len = GetInstructionLength(origDiskBytes + resumeOffset, is64bit);
        if (len == 0 || resumeOffset + len > availableBytes) {
            HookLogImportant(
                "%s: Failed to decode disk bytes while finding verified resume offset "
                "(offset=%d len=%d jump=%d)",
                context, resumeOffset, len, existingJmpSize);
            return false;
        }

        resumeOffset += len;
        if (resumeOffset < existingJmpSize) {
            continue;
        }

        if (firstCandidateOffset == 0) {
            firstCandidateOffset = resumeOffset;
        }
        if (resumeOffset + kCompareBytes > availableBytes) {
            HookLogImportant(
                "%s: Refusing unsafe resume offset +%d; not enough disk bytes to verify "
                "%d-byte live/disk match (jump=%d)",
                context, resumeOffset, kCompareBytes, existingJmpSize);
            return false;
        }

        const bool liveBytesMatchDisk =
            memcmp(liveCode + resumeOffset, origDiskBytes + resumeOffset, kCompareBytes) == 0;
        if (ce::inline_hook_policy::IsVerifiedExternalHookResumeOffset(resumeOffset, existingJmpSize,
                                                                       liveBytesMatchDisk)) {
            if (ce::inline_hook_policy::ShouldExtendExternalHookResumeOffset(firstCandidateOffset, resumeOffset,
                                                                             liveBytesMatchDisk)) {
                HookLogImportant(
                    "%s: Extended resume offset past patched fill bytes "
                    "(firstCandidate=%d selected=%d jump=%d compare=%d)",
                    context, firstCandidateOffset, resumeOffset, existingJmpSize, kCompareBytes);
            } else {
                HookLog("%s: Verified resume offset = %d (past %d-byte external JMP, compare=%d)", context,
                        resumeOffset, existingJmpSize, kCompareBytes);
            }
            if (result) {
                result->resumeOffset = resumeOffset;
                result->firstCandidateOffset = firstCandidateOffset;
            }
            return true;
        }

        HookLogImportant(
            "%s: Resume candidate +%d is still inside a patched live span "
            "(jump=%d live0=0x%02X disk0=0x%02X)",
            context, resumeOffset, existingJmpSize, liveCode[resumeOffset], origDiskBytes[resumeOffset]);
    }

    HookLogImportant(
        "%s: Refusing unsafe external-hook trampoline; no verified live/disk resume "
        "match found after jump (jump=%d firstCandidate=%d scan=%d)",
        context, existingJmpSize, firstCandidateOffset, kMaxResumeScanBytes);
    return false;
}

static void* InstallDeepHookImpl(void* target, void* wrapperFn, TrampolinePublisher publisher,
                                 void* publisherContext, int minimumExternalPatchSize) {
#ifndef _WIN64
    (void)publisher;
    (void)publisherContext;
    (void)minimumExternalPatchSize;
    HookLog("DeepHook: Only supported on x64");
    return nullptr;
#else
    if (!target || !wrapperFn)
        return nullptr;

    std::lock_guard<std::mutex> lock(g_hookMutex);

    // Check not already deep-hooked
    for (auto& d : g_deepHooks) {
        if (d.target == target && d.installed) {
            HookLog("DeepHook: Target %p already deep-hooked", target);
            return nullptr;
        }
    }

    const uint8_t* code = (const uint8_t*)target;

    // Step 1: Determine how many entry bytes the external hook owns.
    //
    // Byte 0 is only a sample. RTSS-style engines restore the original bytes, call through and
    // re-patch on every call, so a target that IS hooked reads clean much of the time. When the
    // caller has already observed the patch it passes its size, and the body patch goes past
    // that span regardless of what the sample shows now.
    int existingJmpSize = 0;
    if (code[0] == 0xE9) {
        existingJmpSize = 5;
    } else if (code[0] == 0xFF && code[1] == 0x25) {
        existingJmpSize = 14;
    } else if (minimumExternalPatchSize > 0) {
        existingJmpSize = minimumExternalPatchSize;
        HookLog(
            "DeepHook: No external jump visible at byte 0 of %p right now (byte=0x%02X); using the caller's observed "
            "%d-byte external patch span",
            target, code[0], existingJmpSize);
    } else {
        HookLog("DeepHook: No external hook at byte 0 of %p (byte=0x%02X)", target, code[0]);
        return nullptr;
    }

    if (minimumExternalPatchSize > existingJmpSize) {
        // A larger span was observed earlier than the one visible now — honour the larger one,
        // because CE must never land inside bytes a foreign tool rewrites.
        HookLog("DeepHook: Widening external patch span at %p from %d to the caller-observed %d bytes", target,
                existingJmpSize, minimumExternalPatchSize);
        existingJmpSize = minimumExternalPatchSize;
    }

    HookLog("DeepHook: External %d-byte JMP detected at %p", existingJmpSize, target);

    // Step 2: Read original (unpatched) bytes from DLL on disk
    uint8_t origDiskBytes[64];
    if (!ReadOrigBytesFromDisk(target, origDiskBytes, 64)) {
        HookLog("DeepHook: Failed to read original bytes from disk for %p", target);
        return nullptr;
    }

    // Log original bytes from disk
    HookLog("DeepHook: Original disk bytes at %p:", target);
    for (int i = 0; i < 32; i += 8) {
        HookLog("  [%02d] %02X %02X %02X %02X %02X %02X %02X %02X", i, origDiskBytes[i], origDiskBytes[i + 1],
                origDiskBytes[i + 2], origDiskBytes[i + 3], origDiskBytes[i + 4], origDiskBytes[i + 5],
                origDiskBytes[i + 6], origDiskBytes[i + 7]);
    }

    // Step 3: Determine a verified resume offset after the full live patch span.
    VerifiedResumeOffset verifiedResume = {};
    if (!TryFindVerifiedExternalHookResumeOffset("DeepHook", code, origDiskBytes, 64, existingJmpSize, true,
                                                 &verifiedResume)) {
        return nullptr;
    }
    int resumeOffset = verifiedResume.resumeOffset;
    HookLog("DeepHook: Resume offset = %d (past %d-byte external JMP, firstCandidate=%d)", resumeOffset,
            existingJmpSize, verifiedResume.firstCandidateOffset);

    // Step 4: Determine how much stack the prolog in [0, resumeOffset) consumed, so the patch
    // can undo it and hand the wrapper the original call state. Those instructions have always
    // already run by then — either in the function itself or in the external hook's trampoline.
    int stackUndo = 0;
    if (!ce::inline_hook_policy::TryComputeDeepHookPrologStackDelta(origDiskBytes, resumeOffset, &stackUndo)) {
        HookLog("DeepHook: Prolog at %p is not a recognized stack shape - cannot undo (resumeOffset=%d)", target,
                resumeOffset);
        return nullptr;
    }
    HookLog("DeepHook: Prolog consumes %d bytes of stack to undo (resumeOffset=%d)", stackUndo, resumeOffset);

    // Step 5: Determine how many bytes to displace at resume offset (need >= patch size)
    const uint8_t* resumeCode = code + resumeOffset;
    int displaceSize = 0;
    // The in-place patch is: [add rsp,N (4 or 7 bytes, omitted when nothing to undo)]
    // + jmp [rip+0] addr (14 bytes)
    int undoEncodingSize = (stackUndo == 0) ? 0 : ((stackUndo <= 127) ? 4 : 7);
    int neededPatchSize = undoEncodingSize + PATCH_SIZE;
    while (displaceSize < neededPatchSize) {
        int len = GetInstructionLength(resumeCode + displaceSize, true);
        if (len == 0) {
            HookLog("DeepHook: Failed to decode at resume+%d (byte=0x%02X)", displaceSize, resumeCode[displaceSize]);
            return nullptr;
        }
        displaceSize += len;
    }
    if (displaceSize > 64) {
        HookLog("DeepHook: Refusing oversized %d-byte displaced block", displaceSize);
        return nullptr;
    }

    HookLog("DeepHook: Will displace %d bytes at offset %d (patch needs %d)", displaceSize, resumeOffset,
            neededPatchSize);

    // Step 6: Allocate private writable memory nearby for the full trampoline.
    // The trampoline contains: original prolog [0, resumeOffset+displaceSize) + JMP to continue
    uint8_t* trampoline = nullptr;
    {
        uintptr_t tgt = (uintptr_t)target;
        for (uintptr_t delta = 0x10000; delta < 0x7FFF0000ULL; delta += 0x10000) {
            if (tgt > delta) {
                uintptr_t tryAddr = (tgt - delta + 0xFFFF) & ~(uintptr_t)0xFFFF;
                trampoline = AllocateWritableTrampolinePage(reinterpret_cast<void*>(tryAddr));
                if (trampoline)
                    break;
            }
            uintptr_t tryAddr = ((tgt + delta) + 0xFFFF) & ~(uintptr_t)0xFFFF;
            trampoline = AllocateWritableTrampolinePage(reinterpret_cast<void*>(tryAddr));
            if (trampoline)
                break;
        }
    }
    if (!trampoline) {
        trampoline = AllocateWritableTrampolinePage(nullptr);
    }
    if (!trampoline) {
        HookLog("DeepHook: Failed to allocate trampoline memory");
        return nullptr;
    }
    memset(trampoline, 0xCC, TRAMPOLINE_POOL_SIZE);  // Fill with INT3 for safety

    HookLog("DeepHook: Trampoline at %p (distance=%lld)", trampoline,
            (long long)((intptr_t)trampoline - (intptr_t)target));

    // Step 7: Build the full trampoline
    // Layout: [original prolog bytes 0..resumeOffset] [displaced bytes with RIP fixups] [JMP continue]
    int tOff = 0;

    // Copy original push instructions [0, resumeOffset) from disk
    memcpy(trampoline, origDiskBytes, resumeOffset);
    tOff = resumeOffset;

    // Copy displaced bytes [resumeOffset, resumeOffset+displaceSize) with RIP-relative fixups
    int srcOff = 0;
    bool fixupFailed = false;
    uintptr_t deepPendingAbsCallTarget = 0;
    bool deepHasPendingAbsCall = false;
    int deepPendingCallInstrOff = -1;
    while (srcOff < displaceSize) {
        int instrLen = GetInstructionLength(resumeCode + srcOff, true);

        const auto shortBranchResult = TryRelocateExternalShortControlTransfer(
            resumeCode + srcOff, reinterpret_cast<uintptr_t>(resumeCode + srcOff), instrLen,
            reinterpret_cast<uintptr_t>(resumeCode), displaceSize, trampoline, &tOff, true, "DeepHook");
        if (shortBranchResult == ShortControlRelocationResult::kFailed) {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return nullptr;
        }
        if (shortBranchResult == ShortControlRelocationResult::kHandled) {
            srcOff += instrLen;
            continue;
        }

        memcpy(trampoline + tOff, resumeCode + srcOff, instrLen);

        int dispOff = GetRipRelativeDispOffset(resumeCode + srcOff, instrLen, true);
        if (dispOff >= 0) {
            int32_t origDisp;
            memcpy(&origDisp, resumeCode + srcOff + dispOff, 4);
            uintptr_t absTarget = (uintptr_t)(resumeCode + srcOff + instrLen) + origDisp;
            uintptr_t newInstrEnd = (uintptr_t)(trampoline + tOff + instrLen);
            int64_t newDisp = (int64_t)absTarget - (int64_t)newInstrEnd;

            if (newDisp > INT32_MAX || newDisp < INT32_MIN) {
                uint8_t op = resumeCode[srcOff];
                if (op == 0xE9 || op == 0xE8) {
                    if (op == 0xE9) {
                        // JMP: FF 25 00 00 00 00 [8-byte address]
                        trampoline[tOff] = 0xFF;
                        trampoline[tOff + 1] = 0x25;
                        trampoline[tOff + 2] = 0;
                        trampoline[tOff + 3] = 0;
                        trampoline[tOff + 4] = 0;
                        trampoline[tOff + 5] = 0;
                        memcpy(trampoline + tOff + 6, &absTarget, 8);
                        tOff += 14;
                    } else {
                        // CALL: write FF 15 with placeholder; patch disp after the
                        // JMP-continue block when we know the ptr location.
                        deepPendingCallInstrOff = tOff;
                        trampoline[tOff] = 0xFF;
                        trampoline[tOff + 1] = 0x15;
                        trampoline[tOff + 2] = 0;  // placeholder
                        trampoline[tOff + 3] = 0;
                        trampoline[tOff + 4] = 0;
                        trampoline[tOff + 5] = 0;
                        tOff += 6;
                        deepPendingAbsCallTarget = absTarget;
                        deepHasPendingAbsCall = true;
                    }
                    srcOff += instrLen;
                    continue;
                }
                HookLog("DeepHook: RIP fixup out of range at resume+%d", srcOff);
                fixupFailed = true;
                break;
            }
            int32_t newDisp32 = (int32_t)newDisp;
            memcpy(trampoline + tOff + dispOff, &newDisp32, 4);
            HookLog("DeepHook: Fixed RIP-relative at +%d: origDisp=0x%08X -> newDisp=0x%08X", srcOff,
                    (unsigned)origDisp, (unsigned)newDisp32);
        }
        tOff += instrLen;
        srcOff += instrLen;
    }

    if (fixupFailed) {
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return nullptr;
    }

    // JMP to (target + resumeOffset + displaceSize) — function body continues here
    uintptr_t continueAddr = (uintptr_t)target + resumeOffset + displaceSize;
    trampoline[tOff++] = 0xFF;
    trampoline[tOff++] = 0x25;
    trampoline[tOff++] = 0x00;
    trampoline[tOff++] = 0x00;
    trampoline[tOff++] = 0x00;
    trampoline[tOff++] = 0x00;
    memcpy(&trampoline[tOff], &continueAddr, 8);
    tOff += 8;

    // If a CALL abs conversion was deferred, write its target pointer here and
    // patch the displacement back into the FF 15 instruction now that we know
    // the exact ptr location.
    if (deepHasPendingAbsCall) {
        int32_t disp = tOff - (deepPendingCallInstrOff + 6);
        memcpy(trampoline + deepPendingCallInstrOff + 2, &disp, 4);
        memcpy(&trampoline[tOff], &deepPendingAbsCallTarget, 8);
        tOff += 8;
    }

    if (!FinalizeExecutableTrampoline(trampoline, TRAMPOLINE_POOL_SIZE, trampoline, static_cast<size_t>(tOff))) {
        HookLog("DeepHook: Trampoline RX/CFG finalization failed");
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return nullptr;
    }

    HookLog("DeepHook: Trampoline built, %d bytes (prolog=%d, displaced=%d, jmp=%d)", tOff, resumeOffset, displaceSize,
            PATCH_SIZE);

    // Step 8: Build the in-place patch at resumeOffset
    // Format: [add rsp, <stackUndo>] ; jmp [rip+0] <wrapperFn>
    uint8_t patchBuf[64];
    int pOff = 0;

    if (stackUndo == 0) {
        // Nothing to undo (e.g. a shadow-space save prolog): the wrapper is entered on the
        // caller's own stack frame, so emitting `add rsp, 0` would only waste patch bytes.
    } else if (stackUndo <= 127) {
        patchBuf[pOff++] = 0x48;                // REX.W
        patchBuf[pOff++] = 0x83;                // ADD r/m64, imm8
        patchBuf[pOff++] = 0xC4;                // ModRM: RSP
        patchBuf[pOff++] = (uint8_t)stackUndo;  // imm8
    } else {
        patchBuf[pOff++] = 0x48;  // REX.W
        patchBuf[pOff++] = 0x81;  // ADD r/m64, imm32
        patchBuf[pOff++] = 0xC4;  // ModRM: RSP
        uint32_t undoVal = (uint32_t)stackUndo;
        memcpy(&patchBuf[pOff], &undoVal, 4);
        pOff += 4;
    }

    // jmp [rip+0] <wrapperFn>
    patchBuf[pOff++] = 0xFF;
    patchBuf[pOff++] = 0x25;
    patchBuf[pOff++] = 0x00;
    patchBuf[pOff++] = 0x00;
    patchBuf[pOff++] = 0x00;
    patchBuf[pOff++] = 0x00;
    uintptr_t wrapAddr = (uintptr_t)wrapperFn;
    memcpy(&patchBuf[pOff], &wrapAddr, 8);
    pOff += 8;

    // Fill remaining displaced bytes with NOP
    while (pOff < displaceSize)
        patchBuf[pOff++] = 0x90;

    // Step 9: Patch the live code at resumeOffset
    DeepHookEntry entry = {};
    entry.target = target;
    entry.hookAddr = (void*)resumeCode;
    entry.patchSize = displaceSize;
    memcpy(entry.origBytes, resumeCode, displaceSize);
    entry.trampoline = trampoline;
    entry.installed = false;

    try {
        g_deepHooks.push_back(entry);
    } catch (...) {
        HookLogImportant("DeepHook: Could not allocate ownership record for target %p", target);
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return nullptr;
    }

    if (publisher)
        publisher(trampoline, publisherContext);

    bool patchInstalled = false;
    DWORD patchError = ERROR_SUCCESS;
    {
        ce::hook_patch::ThreadQuiescence quiescence(resumeCode, static_cast<size_t>(displaceSize));
        if (quiescence.IsReady() && memcmp(resumeCode, entry.origBytes, displaceSize) == 0) {
            DWORD oldProtect = 0;
            if (VirtualProtect((void*)resumeCode, displaceSize, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                // Peer threads are suspended outside the displaced range, so publish the
                // complete patch without an executable INT3/partial-jump window.
                memcpy((void*)resumeCode, patchBuf, displaceSize);

                DWORD ignoredProtect = 0;
                VirtualProtect((void*)resumeCode, displaceSize, oldProtect, &ignoredProtect);
                FlushInstructionCache(GetCurrentProcess(), (void*)resumeCode, displaceSize);
                memcpy(g_deepHooks.back().installedBytes, resumeCode, displaceSize);
                patchInstalled = true;
            } else {
                patchError = GetLastError();
            }
        }
    }
    if (!patchInstalled) {
        if (publisher)
            publisher(nullptr, publisherContext);
        HookLogImportant(
            "DeepHook: Refusing live patch at %p because peer threads could not be quiesced, ownership changed, "
            "or VirtualProtect failed (error=%lu)",
            resumeCode, static_cast<unsigned long>(patchError));
        // The publisher made this trampoline callable before the live patch
        // attempt. Retain its RX allocation for an in-flight caller that
        // acquired the pointer before rollback.
        g_deepHooks.pop_back();
        if (publisher)
            HookLogImportant("DeepHook: Retaining rolled-back published trampoline %p", trampoline);
        else
            VirtualFree(trampoline, 0, MEM_RELEASE);
        return nullptr;
    }

    g_deepHooks.back().installed = true;

    HookLog("DeepHook: SUCCESS at %p+%d (trampoline=%p, displaced=%d, continues at %p)", target, resumeOffset,
            trampoline, displaceSize, (void*)continueAddr);
    return trampoline;
#endif
}

void* InstallDeepHook(void* target, void* wrapperFn, int minimumExternalPatchSize) {
    return InstallDeepHookImpl(target, wrapperFn, nullptr, nullptr, minimumExternalPatchSize);
}

void* InstallDeepHookPublished(void* target, void* wrapperFn, TrampolinePublisher publisher, void* publisherContext,
                               int minimumExternalPatchSize) {
    if (!publisher)
        return nullptr;
    return InstallDeepHookImpl(target, wrapperFn, publisher, publisherContext, minimumExternalPatchSize);
}

// ============================================================================
// Bypass Trampoline — execute real function body past an external E9/FF25 hook
// ============================================================================

void* CreateBypassTrampoline(void* target) {
    if (!target)
        return nullptr;

    std::lock_guard<std::mutex> lock(g_hookMutex);

    const uint8_t* code = (const uint8_t*)target;
#ifdef _WIN64
    constexpr bool kIs64Bit = true;
#else
    constexpr bool kIs64Bit = false;
#endif

    // Step 1: Detect external hook at byte 0
    int existingJmpSize = 0;
    if (code[0] == 0xE9) {
        existingJmpSize = 5;
    } else if (code[0] == 0xFF && code[1] == 0x25) {
        existingJmpSize = 14;
    } else {
        HookLog("BypassTrampoline: No external hook at byte 0 of %p (byte=0x%02X)", target, code[0]);
        return nullptr;
    }

    HookLog("BypassTrampoline: External %d-byte JMP detected at %p", existingJmpSize, target);

    // Step 2: Read original (unpatched) bytes from DLL on disk
    uint8_t origDiskBytes[64];
    if (!ReadOrigBytesFromDisk(target, origDiskBytes, 64)) {
        HookLog("BypassTrampoline: Failed to read original bytes from disk for %p", target);
        return nullptr;
    }

    HookLog("BypassTrampoline: Original disk bytes at %p:", target);
    for (int i = 0; i < 32; i += 8) {
        HookLog("  [%02d] %02X %02X %02X %02X %02X %02X %02X %02X", i, origDiskBytes[i], origDiskBytes[i + 1],
                origDiskBytes[i + 2], origDiskBytes[i + 3], origDiskBytes[i + 4], origDiskBytes[i + 5],
                origDiskBytes[i + 6], origDiskBytes[i + 7]);
    }

    // Step 3: Find a safe instruction boundary after the full live patch span.
    VerifiedResumeOffset verifiedResume = {};
    if (!TryFindVerifiedExternalHookResumeOffset("BypassTrampoline", code, origDiskBytes, 64, existingJmpSize, kIs64Bit,
                                                 &verifiedResume)) {
        return nullptr;
    }
    int resumeOffset = verifiedResume.resumeOffset;
    HookLog("BypassTrampoline: Resume offset = %d (past %d-byte external JMP, firstCandidate=%d)", resumeOffset,
            existingJmpSize, verifiedResume.firstCandidateOffset);

    // Step 4: Allocate trampoline slot near the target
    uint8_t* trampoline = GetTrampolineSlot(target);
    if (!trampoline) {
        HookLog("BypassTrampoline: Failed to allocate trampoline slot near %p", target);
        return nullptr;
    }
    void* trampolinePoolBase = nullptr;
    DWORD trampolinePoolProtect = 0;
    GetTrampolinePoolInfo(trampoline, &trampolinePoolBase, &trampolinePoolProtect);
    HookLog("BypassTrampoline: Building aligned entry=%p pool=%p protect=0x%08lX", trampoline, trampolinePoolBase,
            static_cast<unsigned long>(trampolinePoolProtect));

    // Step 5: Copy original instructions to trampoline with RIP-relative fixups.
    // The source bytes are from disk (original code), but RIP-relative addresses
    // must be computed as if the instructions live at their original location.
    int trampolineOffset = 0;
    int srcOffset = 0;
    uintptr_t pendingAbsCallTarget = 0;
    bool hasPendingAbsCall = false;
    int pendingCallInstrOffset = -1;

    while (srcOffset < resumeOffset) {
        int instrLen = GetInstructionLength(origDiskBytes + srcOffset, kIs64Bit);
        if (instrLen == 0) {
            HookLog("BypassTrampoline: Instruction decode failed at offset %d", srcOffset);
            AbandonCurrentTrampoline();
            return nullptr;
        }

        const auto shortBranchResult = TryRelocateExternalShortControlTransfer(
            origDiskBytes + srcOffset, reinterpret_cast<uintptr_t>(reinterpret_cast<uint8_t*>(target) + srcOffset),
            instrLen, reinterpret_cast<uintptr_t>(target), resumeOffset, trampoline, &trampolineOffset, kIs64Bit,
            "BypassTrampoline");
        if (shortBranchResult == ShortControlRelocationResult::kFailed) {
            AbandonCurrentTrampoline();
            return nullptr;
        }
        if (shortBranchResult == ShortControlRelocationResult::kHandled) {
            srcOffset += instrLen;
            continue;
        }

        memcpy(trampoline + trampolineOffset, origDiskBytes + srcOffset, instrLen);

        // Fix up RIP-relative addressing.
        // The original instruction was at (target + srcOffset), so compute the
        // absolute target from the original location, then adjust the displacement
        // for the trampoline location.
        int dispOff = GetRipRelativeDispOffset(origDiskBytes + srcOffset, instrLen, kIs64Bit);
        if (dispOff >= 0) {
            int32_t origDisp;
            memcpy(&origDisp, origDiskBytes + srcOffset + dispOff, 4);

            // Absolute target = original instruction end + displacement
            uintptr_t absTarget = (uintptr_t)((uint8_t*)target + srcOffset + instrLen) + origDisp;

            // New displacement from trampoline position
            uintptr_t newInstrEnd = (uintptr_t)(trampoline + trampolineOffset + instrLen);
            int64_t newDisp = (int64_t)absTarget - (int64_t)newInstrEnd;

            HookLog("BypassTrampoline: RIP fixup at srcOff=%d, absTarget=%p, newDisp=0x%llX", srcOffset,
                    (void*)absTarget, (long long)newDisp);

            if (newDisp > INT32_MAX || newDisp < INT32_MIN) {
#ifdef _WIN64
                uint8_t opcode = origDiskBytes[srcOffset];
                if (opcode == 0xE9) {
                    trampoline[trampolineOffset] = 0xFF;
                    trampoline[trampolineOffset + 1] = 0x25;
                    trampoline[trampolineOffset + 2] = 0x00;
                    trampoline[trampolineOffset + 3] = 0x00;
                    trampoline[trampolineOffset + 4] = 0x00;
                    trampoline[trampolineOffset + 5] = 0x00;
                    memcpy(trampoline + trampolineOffset + 6, &absTarget, 8);
                    trampolineOffset += 14;
                    srcOffset += instrLen;
                    continue;
                } else if (opcode == 0xE8) {
                    pendingCallInstrOffset = trampolineOffset;
                    trampoline[trampolineOffset] = 0xFF;
                    trampoline[trampolineOffset + 1] = 0x15;
                    trampoline[trampolineOffset + 2] = 0;
                    trampoline[trampolineOffset + 3] = 0;
                    trampoline[trampolineOffset + 4] = 0;
                    trampoline[trampolineOffset + 5] = 0;
                    trampolineOffset += 6;
                    pendingAbsCallTarget = absTarget;
                    hasPendingAbsCall = true;
                    srcOffset += instrLen;
                    continue;
                }
#endif

                HookLog("BypassTrampoline: RIP fixup out of range at offset %d (opcode=0x%02X)", srcOffset,
                        origDiskBytes[srcOffset]);
                AbandonCurrentTrampoline();
                return nullptr;
            }

            int32_t newDisp32 = (int32_t)newDisp;
            memcpy(trampoline + trampolineOffset + dispOff, &newDisp32, 4);
        }

        trampolineOffset += instrLen;
        srcOffset += instrLen;
    }

    // Step 6: Write JMP back to the real code PAST the external hook
    void* jumpTarget = (void*)((uint8_t*)target + resumeOffset);
    HookLog("BypassTrampoline: Writing JMP back from trampoline+%d to %p (target+%d)", trampolineOffset, jumpTarget,
            resumeOffset);
    WriteJump(trampoline + trampolineOffset, jumpTarget);
    trampolineOffset += PATCH_SIZE;

    // Patch pending absolute CALL if needed
    if (hasPendingAbsCall) {
        int32_t disp = trampolineOffset - (pendingCallInstrOffset + 6);
        memcpy(trampoline + pendingCallInstrOffset + 2, &disp, 4);
        memcpy(trampoline + trampolineOffset, &pendingAbsCallTarget, 8);
        trampolineOffset += 8;
        HookLog("BypassTrampoline: Patched absolute CALL target at trampoline+%d", trampolineOffset - 8);
    }

    if (!FinalizeCurrentTrampoline(trampoline, static_cast<size_t>(trampolineOffset))) {
        HookLog("BypassTrampoline: Trampoline RX/CFG finalization failed");
        return nullptr;
    }

    GetTrampolinePoolInfo(trampoline, &trampolinePoolBase, &trampolinePoolProtect);
    HookLog(
        "BypassTrampoline: Created RX/CFG trampoline at %p (%d bytes, pool=%p protect=0x%08lX) "
        "- bypasses %d-byte external hook at %p (resume=%d firstCandidate=%d)",
        trampoline, trampolineOffset, trampolinePoolBase, static_cast<unsigned long>(trampolinePoolProtect),
        existingJmpSize, target, resumeOffset, verifiedResume.firstCandidateOffset);

    return trampoline;
}
}  // namespace InlineHook
