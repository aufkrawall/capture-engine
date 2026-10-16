/**
 * Inline Hook — the x86 deep hook
 *
 * The x64 deep hook (inline_hook_deep.cpp) lets CE intercept a function below a
 * foreign entry patch. Without it an x86 process had only the entry prepend: CE
 * then owned dxgi!Present's first bytes, and the Steam overlay, which hooks DXGI
 * only once the game's first swapchain exists and skips an entry that already
 * jumps elsewhere, never drew (BioShock Infinite, session 20261017_000225).
 *
 * The byte rules (prologue shapes, the undo + jump patch) live in
 * deep_hook_x86_policy.h; this unit relocates, publishes and patches like the
 * x64 installer: trampoline = pristine prologue + displaced instructions + jump
 * back, written under thread quiescence over exactly the displaced range.
 */

#include "inline_hook.h"
#include "inline_hook_internal.h"
#include "inline_hook_lde.h"
#include "hook_patch_transaction.h"
#include "deep_hook_x86_policy.h"

#include <windows.h>
#include <cstring>
#include <mutex>

#include "hook/runtime/hook_common.h"

namespace InlineHook {

#ifndef _WIN64

namespace {

constexpr int kDiskBytes = 64;

// The widest recognized x86 entry patches: E9 rel32 (5), FF 25 abs32 and push imm32/ret (6).
int VisibleExternalPatchSize(const uint8_t* code) {
    if (code[0] == 0xE9)
        return 5;
    if (code[0] == 0xFF && code[1] == 0x25)
        return 6;
    if (code[0] == 0x68 && code[5] == 0xC3)
        return 6;
    return 0;
}

}  // namespace

void* InstallDeepHookImplX86(void* target, void* wrapperFn, TrampolinePublisher publisher, void* publisherContext,
                             int minimumExternalPatchSize, ce::hook_patch::UnstableSnapshotPolicy unstablePolicy) {
    if (!target || !wrapperFn)
        return nullptr;
    std::lock_guard<std::mutex> lock(g_hookMutex);
    for (const auto& existing : g_deepHooks) {
        if (existing.target == target && existing.installed) {
            HookLog("DeepHook(x86): Target %p already deep-hooked", target);
            return nullptr;
        }
    }

    const uint8_t* code = static_cast<const uint8_t*>(target);
    int existingJmpSize = VisibleExternalPatchSize(code);
    if (existingJmpSize == 0 && minimumExternalPatchSize <= 0) {
        HookLog("DeepHook(x86): No external hook at byte 0 of %p (byte=0x%02X)", target, code[0]);
        return nullptr;
    }
    if (minimumExternalPatchSize > existingJmpSize)
        existingJmpSize = minimumExternalPatchSize;

    uint8_t disk[kDiskBytes];
    size_t relocationsApplied = 0;
    if (!ReadOrigBytesFromDisk(target, disk, kDiskBytes, &relocationsApplied)) {
        HookLog("DeepHook(x86): Failed to read original bytes from disk for %p", target);
        return nullptr;
    }
    VerifiedResumeOffset verified = {};
    if (!TryFindVerifiedExternalHookResumeOffset("DeepHook(x86)", code, disk, kDiskBytes, existingJmpSize, false,
                                                 &verified)) {
        return nullptr;
    }
    const int resumeOffset = verified.resumeOffset;
    ce::deep_hook_x86::PrologUndo undo;
    if (!ce::deep_hook_x86::TryAnalyzeProlog(disk, resumeOffset, &undo)) {
        HookLogImportant("DeepHook(x86): Prolog [0,%d) of %p is not a frame-pointer prologue (%02X %02X %02X %02X %02X) "
                         "- cannot undo, keeping the caller's fallback",
                         resumeOffset, target, disk[0], disk[1], disk[2], disk[3], disk[4]);
        return nullptr;
    }

    const uint8_t* resumeCode = code + resumeOffset;
    const int neededPatchSize = ce::deep_hook_x86::PatchSize(undo);
    int displaceSize = 0;
    while (displaceSize < neededPatchSize) {
        const int len = GetInstructionLength(resumeCode + displaceSize, false);
        if (len == 0) {
            HookLog("DeepHook(x86): Failed to decode at resume+%d (byte=0x%02X)", displaceSize,
                    resumeCode[displaceSize]);
            return nullptr;
        }
        displaceSize += len;
    }
    // Every displaced byte must still be the image's own: CE never copies or overwrites
    // bytes another hook rewrote past the entry.
    if (resumeOffset + displaceSize > kDiskBytes ||
        std::memcmp(resumeCode, disk + resumeOffset, static_cast<size_t>(displaceSize)) != 0) {
        HookLogImportant("DeepHook(x86): Displaced range %p+%d..%d differs from the image or exceeds %d bytes - refusing",
                         target, resumeOffset, resumeOffset + displaceSize, kDiskBytes);
        return nullptr;
    }

    uint8_t* trampoline = AllocateWritableTrampolinePage(nullptr);
    if (!trampoline) {
        HookLog("DeepHook(x86): Failed to allocate trampoline memory");
        return nullptr;
    }
    std::memset(trampoline, 0xCC, TRAMPOLINE_POOL_SIZE);

    // Trampoline: the pristine prologue, the displaced instructions (relative branches
    // retargeted), then a jump to where the body continues.
    std::memcpy(trampoline, disk, static_cast<size_t>(resumeOffset));
    int tOff = resumeOffset;
    for (int srcOff = 0; srcOff < displaceSize;) {
        const int instrLen = GetInstructionLength(resumeCode + srcOff, false);
        const auto shortBranch = TryRelocateExternalShortControlTransfer(
            resumeCode + srcOff, reinterpret_cast<uintptr_t>(resumeCode + srcOff), instrLen,
            reinterpret_cast<uintptr_t>(resumeCode), static_cast<size_t>(displaceSize), trampoline, &tOff, false,
            "DeepHook(x86)");
        if (shortBranch == ShortControlRelocationResult::kFailed) {
            VirtualFree(trampoline, 0, MEM_RELEASE);
            return nullptr;
        }
        if (shortBranch == ShortControlRelocationResult::kNotHandled) {
            std::memcpy(trampoline + tOff, resumeCode + srcOff, static_cast<size_t>(instrLen));
            const int dispOff = GetRipRelativeDispOffset(resumeCode + srcOff, instrLen, false);
            if (dispOff >= 0) {
                int32_t originalDisp = 0;
                std::memcpy(&originalDisp, resumeCode + srcOff + dispOff, sizeof(originalDisp));
                const uint32_t absoluteTarget =
                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(resumeCode + srcOff + instrLen)) +
                    static_cast<uint32_t>(originalDisp);
                const uint32_t rel = ce::deep_hook_x86::Rel32(
                    static_cast<uint32_t>(reinterpret_cast<uintptr_t>(trampoline + tOff + instrLen)), absoluteTarget);
                std::memcpy(trampoline + tOff + dispOff, &rel, sizeof(rel));
            }
            tOff += instrLen;
        }
        srcOff += instrLen;
    }
    const uint32_t continueAddress = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(resumeCode + displaceSize));
    trampoline[tOff] = 0xE9;
    const uint32_t continueRel =
        ce::deep_hook_x86::Rel32(static_cast<uint32_t>(reinterpret_cast<uintptr_t>(trampoline + tOff + 5)),
                                 continueAddress);
    std::memcpy(trampoline + tOff + 1, &continueRel, sizeof(continueRel));
    tOff += 5;
    if (!FinalizeExecutableTrampoline(trampoline, TRAMPOLINE_POOL_SIZE, trampoline, static_cast<size_t>(tOff))) {
        HookLog("DeepHook(x86): Trampoline RX/CFG finalization failed");
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return nullptr;
    }

    uint8_t patch[kDiskBytes];
    if (!ce::deep_hook_x86::BuildPatch(undo, static_cast<uint32_t>(reinterpret_cast<uintptr_t>(resumeCode)),
                                       static_cast<uint32_t>(reinterpret_cast<uintptr_t>(wrapperFn)), displaceSize,
                                       patch, kDiskBytes)) {
        HookLog("DeepHook(x86): Patch does not fit the %d displaced bytes", displaceSize);
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return nullptr;
    }

    DeepHookEntry entry = {};
    entry.target = target;
    entry.hookAddr = const_cast<uint8_t*>(resumeCode);
    entry.patchSize = displaceSize;
    std::memcpy(entry.origBytes, resumeCode, static_cast<size_t>(displaceSize));
    entry.trampoline = trampoline;
    entry.installed = false;
    try {
        g_deepHooks.push_back(entry);
    } catch (...) {
        HookLogImportant("DeepHook(x86): Could not allocate ownership record for target %p", target);
        VirtualFree(trampoline, 0, MEM_RELEASE);
        return nullptr;
    }
    if (publisher)
        publisher(trampoline, publisherContext);

    bool patchInstalled = false;
    DWORD patchError = ERROR_SUCCESS;
    auto quiesceFailure = ce::hook_patch::QuiesceFailure::kNone;
    bool ownershipChanged = false;
    bool acceptedUnstableSnapshot = false;
    {
        ce::hook_patch::ThreadQuiescence quiescence(resumeCode, static_cast<size_t>(displaceSize), unstablePolicy);
        quiesceFailure = quiescence.FailureReason();
        acceptedUnstableSnapshot = quiescence.AcceptedUnstableSnapshot();
        if (quiescence.IsReady()) {
            if (std::memcmp(resumeCode, entry.origBytes, static_cast<size_t>(displaceSize)) != 0) {
                ownershipChanged = true;
            } else {
                DWORD oldProtect = 0;
                if (VirtualProtect(const_cast<uint8_t*>(resumeCode), static_cast<size_t>(displaceSize),
                                   PAGE_EXECUTE_READWRITE, &oldProtect)) {
                    std::memcpy(const_cast<uint8_t*>(resumeCode), patch, static_cast<size_t>(displaceSize));
                    DWORD ignored = 0;
                    VirtualProtect(const_cast<uint8_t*>(resumeCode), static_cast<size_t>(displaceSize), oldProtect,
                                   &ignored);
                    FlushInstructionCache(GetCurrentProcess(), resumeCode, static_cast<size_t>(displaceSize));
                    std::memcpy(g_deepHooks.back().installedBytes, resumeCode, static_cast<size_t>(displaceSize));
                    patchInstalled = true;
                } else {
                    patchError = GetLastError();
                }
            }
        }
    }
    LogDeepHookPatchOutcome(resumeCode, patchInstalled, acceptedUnstableSnapshot, quiesceFailure, ownershipChanged,
                            patchError);
    if (!patchInstalled) {
        if (publisher)
            publisher(nullptr, publisherContext);
        g_deepHooks.pop_back();
        // A published trampoline may already be held by an in-flight caller.
        if (publisher)
            HookLogImportant("DeepHook(x86): Retaining rolled-back published trampoline %p", trampoline);
        else
            VirtualFree(trampoline, 0, MEM_RELEASE);
        return nullptr;
    }
    g_deepHooks.back().installed = true;
    HookLogImportant("DeepHook(x86): Installed at %p+%d (undo=%s displaced=%d trampoline=%p continues=%p, "
                     "foreign span=%d relocations=%u)",
                     target, resumeOffset, undo.framePointerOnly ? "pop ebp" : "mov esp,ebp; pop ebp", displaceSize,
                     trampoline, reinterpret_cast<void*>(static_cast<uintptr_t>(continueAddress)), existingJmpSize,
                     static_cast<unsigned>(relocationsApplied));
    return trampoline;
}

#endif  // !_WIN64

}  // namespace InlineHook
