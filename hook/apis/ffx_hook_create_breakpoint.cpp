#include "ffx_hook_internal.h"
#include "../../common/log_meter.h"

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

namespace {

// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
std::mutex g_CreateBreakpointMutex;
PVOID g_CreateBreakpointVehHandle = nullptr;
std::atomic<void*> g_CreateBreakpointTarget{nullptr};
std::atomic<HMODULE> g_CreateBreakpointModule{nullptr};
std::atomic<bool> g_CreateBreakpointArmed{false};
std::atomic<bool> g_CreateBreakpointSuspended{false};
std::atomic<bool> g_CreateBreakpointDeferredRearm{false};
std::atomic<int> g_CreateForwardDepth{0};
uint8_t g_CreateBreakpointOriginalByte = 0;

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

// Caller holds g_CreateBreakpointMutex and has pinned the target's module.
bool ArmPinnedCreateBreakpointLocked(void* target, const char* moduleName, const char* reason) {
    if (g_CreateBreakpointSuspended.load(std::memory_order_acquire) ||
        g_CreateBreakpointTarget.load(std::memory_order_acquire) != target) {
        return false;
    }
    const uint8_t currentByte = *static_cast<const volatile uint8_t*>(target);
    if (g_CreateBreakpointArmed.load(std::memory_order_acquire)) {
        if (currentByte == 0xCC) {
            return true;
        }
        // A fresh mapping of the runtime at the same base: the old arm went away with the old image.
        g_CreateBreakpointArmed.store(false, std::memory_order_release);
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

    g_CreateBreakpointOriginalByte = currentByte;
    // Publish before the int3 exists: a thread that traps on the new byte must find it accounted for.
    g_CreateBreakpointArmed.store(true, std::memory_order_release);
    if (!WriteFfxExportEntryByte(target, 0xCC)) {
        g_CreateBreakpointArmed.store(false, std::memory_order_release);
        HookLogImportant("FFX Hook: Failed to arm ffxCreateContext entry breakpoint at %p (module=%s err=%lu)", target,
                         moduleName ? moduleName : "FFX", GetLastError());
        return false;
    }

    const bool steadyStateRearm = reason && std::strcmp(reason, "post-call rearm") == 0;
    static std::atomic<uint32_t> s_rearmLogCount{0};
    if (!steadyStateRearm ||
        ce::log_meter::ShouldLogCadence(s_rearmLogCount.fetch_add(1, std::memory_order_relaxed) + 1, 5, 100)) {
        HookLogImportant("FFX Hook: Armed ffxCreateContext entry breakpoint at %p (module=%s reason=%s)", target,
                         moduleName ? moduleName : "FFX", reason ? reason : "unknown");
    }
    return true;
}

// Same proof without taking a reference, for callers holding g_CreateBreakpointMutex: releasing a pin there
// could run a module unload (and the loader lock) under CE's mutex.
bool IsLiveCreateContextExport(HMODULE expectedModule, void* target) {
    HMODULE owner = nullptr;
    return expectedModule && IsLiveFfxExportEntry(target, "ffxCreateContext", &owner) && owner == expectedModule;
}

// Caller holds g_CreateBreakpointMutex. Returns false only when a live armed byte could not be removed.
bool RestoreCreateBreakpointLocked(const char* reason) {
    void* target = g_CreateBreakpointTarget.load(std::memory_order_acquire);
    if (!target || !g_CreateBreakpointArmed.load(std::memory_order_acquire)) {
        return true;
    }
    if (!IsLiveCreateContextExport(g_CreateBreakpointModule.load(std::memory_order_acquire), target)) {
        // The image is gone (or replaced); its byte went with it.
        g_CreateBreakpointArmed.store(false, std::memory_order_release);
        HookLogImportant("FFX Hook: Dropped ffxCreateContext entry-breakpoint state for unloaded target %p (%s)",
                         target, reason ? reason : "unknown");
        return true;
    }
    if (*static_cast<const volatile uint8_t*>(target) == 0xCC &&
        !WriteFfxExportEntryByte(target, g_CreateBreakpointOriginalByte)) {
        HookLogImportant("FFX Hook: Failed to restore ffxCreateContext entry byte at %p (%s err=%lu)", target,
                         reason ? reason : "unknown", GetLastError());
        return false;
    }
    g_CreateBreakpointArmed.store(false, std::memory_order_release);
    HookLogImportant("FFX Hook: Restored ffxCreateContext entry byte at %p (%s)", target, reason ? reason : "unknown");
    return true;
}

bool RearmCurrentCreateBreakpoint(const char* reason) {
    void* target = g_CreateBreakpointTarget.load(std::memory_order_acquire);
    HMODULE module = g_CreateBreakpointModule.load(std::memory_order_acquire);
    HMODULE pinned = nullptr;
    if (!PinLiveCreateContextExport(module, target, &pinned)) {
        return false;
    }
    bool armed = false;
    {
        std::lock_guard<std::mutex> lock(g_CreateBreakpointMutex);
        armed = ArmPinnedCreateBreakpointLocked(target, "protected official FFX runtime", reason);
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
    void* target = g_CreateBreakpointTarget.load(std::memory_order_acquire);
    CONTEXT* ctx = ep->ContextRecord;
    const bool hitsTarget = FFXHook::detail::IsEntryBreakpointHit(ep->ExceptionRecord->ExceptionAddress,
                                                                  static_cast<uintptr_t>(ctx->Rip), target);
    const bool armed = g_CreateBreakpointArmed.load(std::memory_order_acquire);
    const bool targetByteIsBreakpoint =
        hitsTarget && *static_cast<const volatile uint8_t*>(target) == static_cast<uint8_t>(0xCC);
    const auto action = FFXHook::detail::ClassifyEntryBreakpoint(hitsTarget, armed, targetByteIsBreakpoint);
    if (action == FFXHook::detail::EntryBreakpointAction::kNotOurs) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    if (action == FFXHook::detail::EntryBreakpointAction::kHandleCall) {
        if (!WriteFfxExportEntryByte(target, g_CreateBreakpointOriginalByte)) {
            // Resuming on an int3 that cannot be removed would trap again at once. Stop arming and let the
            // next handler see it instead of spinning this thread.
            g_CreateBreakpointSuspended.store(true, std::memory_order_release);
            return EXCEPTION_CONTINUE_SEARCH;
        }
        g_CreateBreakpointArmed.store(false, std::memory_order_release);
        g_CreateBreakpointDeferredRearm.store(true, std::memory_order_release);
    }
    // kHandleCall, or kResumeAtTarget (another thread already restored the byte): either way the entry state is
    // intact, so resume in the detour exactly as if the client had called it.
    ffx_hook_t_FfxCreateContextOriginalOverride = reinterpret_cast<PfnFfxCreateContext>(target);
    ctx->Rip = reinterpret_cast<DWORD64>(&Hooked_ffxCreateContext);
    return EXCEPTION_CONTINUE_EXECUTION;
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
        const bool retargetBlocked = g_CreateBreakpointTarget.load(std::memory_order_acquire) != targetAddress &&
                                     !RestoreCreateBreakpointLocked("ffxCreateContext target changed");
        if (g_CreateBreakpointVehHandle && !retargetBlocked &&
            !g_CreateBreakpointSuspended.load(std::memory_order_acquire)) {
            g_CreateBreakpointModule.store(module, std::memory_order_release);
            g_CreateBreakpointTarget.store(targetAddress, std::memory_order_release);
            if (g_CreateForwardDepth.load(std::memory_order_acquire) > 0) {
                // A forward is running the original right now; its outermost return re-arms.
                g_CreateBreakpointDeferredRearm.store(true, std::memory_order_release);
                armed = true;
            } else {
                armed = ArmPinnedCreateBreakpointLocked(targetAddress, ffx_hook_moduleName, ffx_hook_reason);
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

    g_CreateForwardDepth.fetch_add(1, std::memory_order_acq_rel);
    {
        std::lock_guard<std::mutex> lock(g_CreateBreakpointMutex);
        void* target = reinterpret_cast<void*>(originalCreate);
        if (g_CreateBreakpointArmed.load(std::memory_order_acquire) &&
            g_CreateBreakpointTarget.load(std::memory_order_acquire) == target &&
            IsCommittedReadableCodeAddress(target) && *static_cast<const volatile uint8_t*>(target) == 0xCC) {
            if (WriteFfxExportEntryByte(target, g_CreateBreakpointOriginalByte)) {
                g_CreateBreakpointArmed.store(false, std::memory_order_release);
                g_CreateBreakpointDeferredRearm.store(true, std::memory_order_release);
            } else {
                HookLogImportant("FFX Hook: Failed to pause ffxCreateContext entry breakpoint before forwarding "
                                 "(target=%p err=%lu)",
                                 target, GetLastError());
            }
        }
    }

    const ffxReturnCode_t result = originalCreate(ffx_hook_context, ffx_hook_desc, memCb);

    const int remainingDepth = g_CreateForwardDepth.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remainingDepth == 0 && g_CreateBreakpointDeferredRearm.exchange(false, std::memory_order_acq_rel)) {
        RearmCurrentCreateBreakpoint("post-call rearm");
    }
    return result;
}

void SuspendFfxCreateContextBreakpoint(const char* ffx_hook_reason) {
    std::lock_guard<std::mutex> lock(g_CreateBreakpointMutex);
    // Set first so a concurrent forward cannot re-arm after the byte is restored.
    g_CreateBreakpointSuspended.store(true, std::memory_order_release);
    g_CreateBreakpointDeferredRearm.store(false, std::memory_order_release);
    RestoreCreateBreakpointLocked(ffx_hook_reason);
}

void ResumeFfxCreateContextBreakpoint(const char* ffx_hook_reason) {
    {
        std::lock_guard<std::mutex> lock(g_CreateBreakpointMutex);
        if (!g_CreateBreakpointVehHandle || !g_CreateBreakpointTarget.load(std::memory_order_acquire)) {
            return;
        }
        g_CreateBreakpointSuspended.store(false, std::memory_order_release);
    }
    RearmCurrentCreateBreakpoint(ffx_hook_reason);
}

// Loader-lock context: atomics only. The target is kept so a reload can still be recognized as a retarget; with
// the armed flag clear, nothing restores into the old address and the VEH treats an int3 there as foreign.
void InvalidateFfxCreateContextBreakpointForUnloadedImage(const void* imageBase, size_t imageSize) {
    void* target = g_CreateBreakpointTarget.load(std::memory_order_acquire);
    if (!FFXHook::detail::IsAddressInImage(target, imageBase, imageSize)) {
        return;
    }
    const bool wasArmed = g_CreateBreakpointArmed.exchange(false, std::memory_order_acq_rel);
    g_CreateBreakpointDeferredRearm.store(false, std::memory_order_release);
    HookLog("FFX Hook: ffxCreateContext entry-breakpoint target %p left with its unloading image (wasArmed=%d)", target,
            wasArmed ? 1 : 0);
}

void ShutdownFfxCreateContextBreakpoint() {
    SuspendFfxCreateContextBreakpoint("FFX hook shutdown");
    std::lock_guard<std::mutex> lock(g_CreateBreakpointMutex);
    // The byte is restored first, so only a thread already inside dispatch could still need the handler.
    if (g_CreateBreakpointVehHandle) {
        RemoveVectoredExceptionHandler(g_CreateBreakpointVehHandle);
        g_CreateBreakpointVehHandle = nullptr;
    }
    g_CreateBreakpointTarget.store(nullptr, std::memory_order_release);
    g_CreateBreakpointModule.store(nullptr, std::memory_order_release);
    g_CreateBreakpointArmed.store(false, std::memory_order_release);
    // Like the ffxConfigure breakpoint, a later FFXHook::Init starts from scratch.
    g_CreateBreakpointSuspended.store(false, std::memory_order_release);
}
