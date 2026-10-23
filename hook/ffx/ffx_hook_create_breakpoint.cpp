#include "ffx_hook_internal.h"
#include "ffx_export_lifetime.h"
#include "common/logging/log_meter.h"

// ffxCreateContext entry breakpoint for protected official AMD runtimes.
//
// GTA V Enhanced reloads amd_fidelityfx_dx12.dll for every native-FSR session, resolves the exports without its
// import table (CE's GetProcAddress route never sees them) and calls ffxCreateContext before the next cached-slot
// rescan can reroute the pointer it stored (session 20260925_165708: create in flight at 16:59:05.356, slots
// routed at .372, on every reload). CE then saw destroy and configure but never create, so every destroy was
// "Non-FG" and the all-FG-contexts-destroyed teardown never ran.
//
// The entry byte is armed from the module-load notification, on the loading thread, before the load returns to
// the client. A trapped call restores the byte and resumes at Hooked_ffxCreateContext with the untouched entry
// state (return address on the stack, arguments in registers), so the detour runs as an ordinary call and not
// inside exception dispatch. The detour forwards through CallFfxCreateContextOriginalGuarded, which keeps the
// byte out of the way while the original runs and re-arms once the outermost forward returns. Creates happen a
// few times per FSR session, so this never becomes the per-frame 0xCC contention that retired the ffxConfigure
// breakpoint; routed slots and GetProcAddress callers reach the same detour and are paused the same way.
//
// Every live runtime image keeps its own slot. Two images can share a name: a tool's own FidelityFX copy beside the
// game's (OptiScaler FGOutput=FSRFG, 20261023_221541) resolves its exports outside every CE route, so only its own
// entry byte sees its create. A trap resumes in the detour with that slot's export as the exact original.

namespace {

constexpr size_t kSlotCount = 4;

struct Slot {
    std::atomic<void*> target{nullptr};
    std::atomic<HMODULE> module{nullptr};
    std::atomic<bool> armed{false};
    std::atomic<bool> deferredRearm{false};
    std::atomic<int> forwardDepth{0};
    uint8_t originalByte = 0;  // written under the mutex before `armed` publishes it
};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
std::mutex g_CreateBreakpointMutex;
PVOID g_CreateBreakpointVehHandle = nullptr;
std::atomic<bool> g_CreateBreakpointSuspended{false};
Slot g_Slots[kSlotCount];

// Pins the module that owns `target` and proves `target` is still that module's ffxCreateContext export.
// Anything weaker could write 0xCC into an unrelated image mapped at a reloaded runtime's old address.
bool PinLiveCreateContextExport(HMODULE expectedModule, void* target, HMODULE* pinnedOut) {
    *pinnedOut = nullptr;
    HMODULE owner = nullptr;
    if (!target || !expectedModule ||
        !GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(target), &owner) ||
        !owner) {
        return false;
    }
    if (owner != expectedModule || reinterpret_cast<void*>(GetProcAddress(owner, "ffxCreateContext")) != target ||
        !IsCommittedReadableCodeAddress(target)) {
        FreeLibrary(owner);
        return false;
    }
    *pinnedOut = owner;
    return true;
}

Slot* FindSlotForTarget(const void* target) {
    if (!target) {
        return nullptr;
    }
    for (Slot& slot : g_Slots) {
        if (slot.target.load(std::memory_order_acquire) == target) {
            return &slot;
        }
    }
    return nullptr;
}

// Caller holds g_CreateBreakpointMutex and has pinned the slot's module.
bool ArmPinnedSlotLocked(Slot& slot, const char* moduleName, const char* reason) {
    void* target = slot.target.load(std::memory_order_acquire);
    if (!target || g_CreateBreakpointSuspended.load(std::memory_order_acquire)) {
        return false;
    }
    const uint8_t currentByte = *static_cast<const volatile uint8_t*>(target);
    if (slot.armed.load(std::memory_order_acquire)) {
        if (currentByte == 0xCC) {
            return true;
        }
        // A fresh mapping of the runtime at the same base: the old arm went away with the old image.
        slot.armed.store(false, std::memory_order_release);
    }
    if (currentByte == 0xCC) {
        // Someone else's int3 (a debugger or another hook). Stacking onto it would lose its original byte.
        static std::atomic<uint32_t> s_foreignByteLogCount{0};
        const uint32_t logCount = s_foreignByteLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ce::log_meter::ShouldLogCadence(logCount, 5, 100)) {
            HookLogImportant(
                "FFX Hook: ffxCreateContext at %p already starts with a breakpoint CE did not write; leaving it "
                "unarmed (module=%s log=%u)",
                target, moduleName ? moduleName : "FFX", logCount);
        }
        return false;
    }

    slot.originalByte = currentByte;
    // Publish before the int3 exists: a thread that traps on the new byte must find it accounted for.
    slot.armed.store(true, std::memory_order_release);
    if (!WriteFfxExportEntryByte(target, 0xCC)) {
        slot.armed.store(false, std::memory_order_release);
        HookLogImportant("FFX Hook: Failed to arm ffxCreateContext entry breakpoint at %p (module=%s err=%lu)", target,
                         moduleName ? moduleName : "FFX", GetLastError());
        return false;
    }

    const bool steadyStateRearm = reason && std::strcmp(reason, "post-call rearm") == 0;
    static std::atomic<uint32_t> s_rearmLogCount{0};
    if (!steadyStateRearm ||
        ce::log_meter::ShouldLogCadence(s_rearmLogCount.fetch_add(1, std::memory_order_relaxed) + 1, 5, 100)) {
        HookLogImportant("FFX Hook: Armed ffxCreateContext entry breakpoint at %p (module=%s reason=%s slot=%zu)",
                         target, moduleName ? moduleName : "FFX", reason ? reason : "unknown",
                         static_cast<size_t>(&slot - g_Slots));
    }
    return true;
}

// Caller holds g_CreateBreakpointMutex and acquired the image pin outside it.
// A failed pin means the old image is gone; never inspect its entry byte.
bool RestoreSlotLocked(Slot& slot, void* pinnedTarget, HMODULE pinnedModule, const char* reason) {
    void* target = slot.target.load(std::memory_order_acquire);
    if (!target || !slot.armed.load(std::memory_order_acquire)) {
        return true;
    }
    if (target != pinnedTarget) {
        return false;  // a concurrent retarget owns the byte now
    }
    if (!pinnedModule) {
        // The image is gone (or replaced); its byte went with it.
        slot.armed.store(false, std::memory_order_release);
        HookLogImportant("FFX Hook: Dropped ffxCreateContext entry-breakpoint state for unloaded target %p (%s)",
                         target, reason ? reason : "unknown");
        return true;
    }
    if (*static_cast<const volatile uint8_t*>(target) == 0xCC && !WriteFfxExportEntryByte(target, slot.originalByte)) {
        HookLogImportant("FFX Hook: Failed to restore ffxCreateContext entry byte at %p (%s err=%lu)", target,
                         reason ? reason : "unknown", GetLastError());
        return false;
    }
    slot.armed.store(false, std::memory_order_release);
    HookLogImportant("FFX Hook: Restored ffxCreateContext entry byte at %p (%s)", target, reason ? reason : "unknown");
    return true;
}

bool RearmSlot(Slot& slot, const char* reason) {
    void* target = slot.target.load(std::memory_order_acquire);
    HMODULE module = slot.module.load(std::memory_order_acquire);
    HMODULE pinned = nullptr;
    if (!PinLiveCreateContextExport(module, target, &pinned)) {
        return false;
    }
    bool armed = false;
    {
        std::lock_guard<std::mutex> lock(g_CreateBreakpointMutex);
        if (slot.target.load(std::memory_order_acquire) == target) {
            armed = ArmPinnedSlotLocked(slot, "protected official FFX runtime", reason);
        }
    }
    FreeLibrary(pinned);
    return armed;
}

#ifdef _WIN64
LONG WINAPI FfxCreateContextBreakpointVEH(EXCEPTION_POINTERS* ep) {
    if (!ep || !ep->ContextRecord || !ep->ExceptionRecord ||
        ep->ExceptionRecord->ExceptionCode != STATUS_BREAKPOINT) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    CONTEXT* ctx = ep->ContextRecord;
    for (Slot& slot : g_Slots) {
        void* target = slot.target.load(std::memory_order_acquire);
        const bool hitsTarget = target && FFXHook::detail::IsEntryBreakpointHit(ep->ExceptionRecord->ExceptionAddress,
                                                                                static_cast<uintptr_t>(ctx->Rip),
                                                                                target);
        if (!hitsTarget) {
            continue;
        }
        const bool armed = slot.armed.load(std::memory_order_acquire);
        const bool targetByteIsBreakpoint = *static_cast<const volatile uint8_t*>(target) == static_cast<uint8_t>(0xCC);
        const auto action = FFXHook::detail::ClassifyEntryBreakpoint(hitsTarget, armed, targetByteIsBreakpoint);
        if (action == FFXHook::detail::EntryBreakpointAction::kNotOurs) {
            return EXCEPTION_CONTINUE_SEARCH;
        }
        if (action == FFXHook::detail::EntryBreakpointAction::kHandleCall) {
            if (!WriteFfxExportEntryByte(target, slot.originalByte)) {
                // Resuming on an int3 that cannot be removed would trap again at once. Stop arming and let the
                // next handler see it instead of spinning this thread.
                g_CreateBreakpointSuspended.store(true, std::memory_order_release);
                return EXCEPTION_CONTINUE_SEARCH;
            }
            slot.armed.store(false, std::memory_order_release);
            slot.deferredRearm.store(true, std::memory_order_release);
        }
        // kHandleCall, or kResumeAtTarget (another thread already restored the byte): either way the entry state
        // is intact, so resume in the detour exactly as if the client had called it.
        ffx_hook_t_FfxCreateContextOriginalOverride = reinterpret_cast<PfnFfxCreateContext>(target);
        ctx->Rip = reinterpret_cast<DWORD64>(&Hooked_ffxCreateContext);
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

}  // namespace

bool ArmFfxCreateContextBreakpoint(HMODULE module, PfnFfxCreateContext target, const char* ffx_hook_moduleName,
                                   const char* ffx_hook_reason) {
#ifndef _WIN64
    (void)module;
    (void)target;
    (void)ffx_hook_moduleName;
    (void)ffx_hook_reason;
    return false;
#else
    void* targetAddress = reinterpret_cast<void*>(target);
    HMODULE pinned = nullptr;
    if (!PinLiveCreateContextExport(module, targetAddress, &pinned)) {
        static std::atomic<uint32_t> s_refusedLogCount{0};
        const uint32_t logCount = s_refusedLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ce::log_meter::ShouldLogCadence(logCount, 5, 100)) {
            HookLogImportant(
                "FFX Hook: Refusing to arm ffxCreateContext entry breakpoint: %p is not a live export of %s (log=%u)",
                targetAddress, ffx_hook_moduleName ? ffx_hook_moduleName : "FFX", logCount);
        }
        return false;
    }
    bool armed = false;
    {
        std::lock_guard<std::mutex> lock(g_CreateBreakpointMutex);
        if (!g_CreateBreakpointVehHandle) {
            g_CreateBreakpointVehHandle = AddVectoredExceptionHandler(1, FfxCreateContextBreakpointVEH);
        }
        // The image's slot: its own, else the first free one. A pinned live export cannot share a slot with
        // another image, and an unloaded image's slot was freed by the unload notification.
        Slot* slot = nullptr;
        for (Slot& candidate : g_Slots) {
            if (candidate.module.load(std::memory_order_acquire) == module) {
                slot = &candidate;
                break;
            }
        }
        if (slot && slot->target.load(std::memory_order_acquire) != targetAddress) {
            // The same handle mapping a reloaded image: the old arm went away with the old image.
            slot->armed.store(false, std::memory_order_release);
            slot->deferredRearm.store(false, std::memory_order_release);
            slot->target.store(targetAddress, std::memory_order_release);
        }
        if (!slot) {
            for (Slot& candidate : g_Slots) {
                if (!candidate.target.load(std::memory_order_acquire)) {
                    slot = &candidate;
                    slot->armed.store(false, std::memory_order_release);
                    slot->deferredRearm.store(false, std::memory_order_release);
                    slot->module.store(module, std::memory_order_release);
                    slot->target.store(targetAddress, std::memory_order_release);
                    break;
                }
            }
        }
        if (!slot) {
            static std::atomic<uint32_t> s_fullLogCount{0};
            if (ce::log_meter::ShouldLogCadence(s_fullLogCount.fetch_add(1, std::memory_order_relaxed) + 1, 5, 100)) {
                HookLogImportant("FFX Hook: no free ffxCreateContext breakpoint slot for %p (module=%s, %zu live images)",
                                 targetAddress, ffx_hook_moduleName ? ffx_hook_moduleName : "FFX", kSlotCount);
            }
        } else if (g_CreateBreakpointVehHandle && !g_CreateBreakpointSuspended.load(std::memory_order_acquire)) {
            if (slot->forwardDepth.load(std::memory_order_acquire) > 0) {
                // A forward is running the original right now; its outermost return re-arms.
                slot->deferredRearm.store(true, std::memory_order_release);
                armed = true;
            } else {
                armed = ArmPinnedSlotLocked(*slot, ffx_hook_moduleName, ffx_hook_reason);
            }
        }
    }
    FreeLibrary(pinned);
    return armed;
#endif
}

ffxReturnCode_t CallFfxCreateContextOriginalGuarded(PfnFfxCreateContext originalCreate, ffxContext* ffx_hook_context,
                                                    ffxCreateContextDescHeader* ffx_hook_desc,
                                                    const ffxAllocationCallbacks* memCb) {
    if (!originalCreate) {
        return 1;
    }

    void* target = reinterpret_cast<void*>(originalCreate);
    Slot* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_CreateBreakpointMutex);
        slot = FindSlotForTarget(target);
        if (slot) {
            slot->forwardDepth.fetch_add(1, std::memory_order_acq_rel);
            if (slot->armed.load(std::memory_order_acquire) && IsCommittedReadableCodeAddress(target) &&
                *static_cast<const volatile uint8_t*>(target) == 0xCC) {
                if (WriteFfxExportEntryByte(target, slot->originalByte)) {
                    slot->armed.store(false, std::memory_order_release);
                    slot->deferredRearm.store(true, std::memory_order_release);
                } else {
                    HookLogImportant("FFX Hook: Failed to pause ffxCreateContext entry breakpoint before forwarding "
                                     "(target=%p err=%lu)",
                                     target, GetLastError());
                }
            }
        }
    }

    const ffxReturnCode_t result = originalCreate(ffx_hook_context, ffx_hook_desc, memCb);

    if (slot) {
        const int remainingDepth = slot->forwardDepth.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (remainingDepth == 0 && slot->target.load(std::memory_order_acquire) == target &&
            slot->deferredRearm.exchange(false, std::memory_order_acq_rel)) {
            RearmSlot(*slot, "post-call rearm");
        }
    }
    return result;
}

void SuspendFfxCreateContextBreakpoint(const char* ffx_hook_reason) {
    void* targets[kSlotCount] = {};
    HMODULE modules[kSlotCount] = {};
    {
        std::lock_guard<std::mutex> lock(g_CreateBreakpointMutex);
        // Freeze arming while acquiring the pins outside CE's mutex.
        g_CreateBreakpointSuspended.store(true, std::memory_order_release);
        for (size_t i = 0; i < kSlotCount; ++i) {
            g_Slots[i].deferredRearm.store(false, std::memory_order_release);
            targets[i] = g_Slots[i].target.load(std::memory_order_acquire);
            modules[i] = g_Slots[i].module.load(std::memory_order_acquire);
        }
    }
    for (size_t i = 0; i < kSlotCount; ++i) {
        if (!targets[i]) {
            continue;
        }
        ce::ffx_export_lifetime::ModulePin pin(modules[i], targets[i], "ffxCreateContext");
        std::lock_guard<std::mutex> lock(g_CreateBreakpointMutex);
        RestoreSlotLocked(g_Slots[i], targets[i], pin.Get(), ffx_hook_reason);
    }
}

void ResumeFfxCreateContextBreakpoint(const char* ffx_hook_reason) {
    {
        std::lock_guard<std::mutex> lock(g_CreateBreakpointMutex);
        if (!g_CreateBreakpointVehHandle) {
            return;
        }
        g_CreateBreakpointSuspended.store(false, std::memory_order_release);
    }
    for (Slot& slot : g_Slots) {
        if (slot.target.load(std::memory_order_acquire)) {
            RearmSlot(slot, ffx_hook_reason);
        }
    }
}

// Loader-lock context: atomics only. The departing image's slot is freed with its arm cleared, so nothing restores
// into the old address and the VEH treats an int3 there as foreign.
void InvalidateFfxCreateContextBreakpointForUnloadedImage(const void* imageBase, size_t imageSize) {
    for (Slot& slot : g_Slots) {
        void* target = slot.target.load(std::memory_order_acquire);
        if (!FFXHook::detail::IsAddressInImage(target, imageBase, imageSize)) {
            continue;
        }
        const bool wasArmed = slot.armed.exchange(false, std::memory_order_acq_rel);
        slot.deferredRearm.store(false, std::memory_order_release);
        slot.module.store(nullptr, std::memory_order_release);
        slot.target.store(nullptr, std::memory_order_release);
        HookLog("FFX Hook: ffxCreateContext entry-breakpoint target %p left with its unloading image (wasArmed=%d)",
                target, wasArmed ? 1 : 0);
    }
}

void ShutdownFfxCreateContextBreakpoint() {
    SuspendFfxCreateContextBreakpoint("FFX hook shutdown");
    std::lock_guard<std::mutex> lock(g_CreateBreakpointMutex);
    // The bytes are restored first, so only a thread already inside dispatch could still need the handler.
    if (g_CreateBreakpointVehHandle) {
        RemoveVectoredExceptionHandler(g_CreateBreakpointVehHandle);
        g_CreateBreakpointVehHandle = nullptr;
    }
    for (Slot& slot : g_Slots) {
        slot.target.store(nullptr, std::memory_order_release);
        slot.module.store(nullptr, std::memory_order_release);
        slot.armed.store(false, std::memory_order_release);
        slot.deferredRearm.store(false, std::memory_order_release);
    }
    // Like the ffxConfigure breakpoint, a later FFXHook::Init starts from scratch.
    g_CreateBreakpointSuspended.store(false, std::memory_order_release);
}
