#include "dxgi_shared_internal.h"

// Validity of foreign Present handlers CE forwards into.
//
// Split out of dxgi_shared_steam.cpp to keep both units under the source-size ceiling:
// this is the "a saved foreign hook target is never valid forever" boundary, not Steam
// -specific routing.

namespace DXGIShared {
bool IsExecutableCodeAddress(const void* address) {
    if (!address) {
        return false;
    }
    MEMORY_BASIC_INFORMATION memory = {};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory)) {
        return false;
    }
    if (memory.State != MEM_COMMIT || (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    return (memory.Protect &
            (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}
}

namespace DXGIShared {
namespace {
bool IsCommittedExecutableAddress(const void* address) {
    if (!address) {
        return false;
    }
    MEMORY_BASIC_INFORMATION memory = {};
    if (VirtualQuery(address, &memory, sizeof(memory)) != sizeof(memory)) {
        return false;
    }
    if (memory.State != MEM_COMMIT || (memory.Protect & (PAGE_NOACCESS | PAGE_GUARD)) != 0) {
        return false;
    }
    return (memory.Protect &
            (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY)) != 0;
}
}  // namespace

// A foreign Present hook target captured once at install time is NOT valid forever. The
// overlays that share dxgi!Present tear their hooks down and rebuild them (RTSS restores and
// re-patches on every call; Steam re-hooks on new swapchains), which frees or rewrites the
// runtime-allocated thunk CE recorded. Talos session 20260812_024730 crashed exactly there:
// five milliseconds after `foreign re-hook took the Present entry from CE`, CE called its
// saved hook 0x7FF842DC0000, whose `FF 25` payload now read 0x295C8999101 - a heap address -
// and the jump died with a DEP execute violation inside
// TryInvokeGuardedExternalSteamOverlayPresent. Validate the entry AND, for a thunk, the
// address it forwards to, before ever transferring control there.
bool IsCallableForeignPresentHandler(const void* handler) {
    if (!IsCommittedExecutableAddress(handler)) {
        return false;
    }
    const auto* code = static_cast<const uint8_t*>(handler);
    if (!IsReadableMemory(code, 16)) {
        return false;
    }
    if (code[0] == 0xFF && code[1] == 0x25) {
        void* thunkTarget = ResolveFF25JmpTarget(const_cast<void*>(handler));
        return IsCommittedExecutableAddress(thunkTarget);
    }
    if (code[0] == 0xE9) {
        void* jumpTarget = ResolveE9JmpTarget(const_cast<void*>(handler));
        return IsCommittedExecutableAddress(jumpTarget);
    }
    return true;
}
}

namespace DXGIShared {
// Self-heal a stale saved foreign hook: whoever owns the live dxgi!Present entry right now is
// the current head of the foreign chain, so re-derive the target from those bytes. Returns
// the refreshed, validated handler, or nullptr when the entry is CE's own prepend, is not a
// chainable jump, or still forwards somewhere uncallable (then callers fail closed to the
// clean DXGI bypass).
PFN_Present RefreshExternalOverlayPresentHookFromLiveEntry() {
    const PFN_Present liveEntry = dxgi_shared_s_originalVtable8Present;
    if (!liveEntry || liveEntry == DetourPresent) {
        return nullptr;
    }
    const auto* entryCode = reinterpret_cast<const uint8_t*>(liveEntry);
    if (!IsReadableMemory(entryCode, 16)) {
        return nullptr;
    }
    void* entryTarget = nullptr;
    if (entryCode[0] == 0xE9) {
        entryTarget = ResolveE9JmpTarget(const_cast<void*>(reinterpret_cast<const void*>(liveEntry)));
    } else if (entryCode[0] == 0xFF && entryCode[1] == 0x25) {
        entryTarget = ResolveFF25JmpTarget(const_cast<void*>(reinterpret_cast<const void*>(liveEntry)));
    }
    if (!entryTarget) {
        return nullptr;
    }
    // CE's own relay lives in a CE trampoline pool; re-adopting it would make CE call itself.
    if (InlineHook::IsInTrampolinePool(entryTarget) || entryTarget == reinterpret_cast<void*>(DetourPresent)) {
        return nullptr;
    }
    if (!IsCallableForeignPresentHandler(entryTarget)) {
        return nullptr;
    }

    const auto refreshed = reinterpret_cast<PFN_Present>(entryTarget);
    if (refreshed != dxgi_shared_g_externalOverlayPresentHook) {
        static std::atomic<int> s_refreshLogCount{0};
        const int n = s_refreshLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (n <= 10 || (n % 200) == 0) {
            char ownerPath[MAX_PATH] = {};
            const bool resolvedOwner = ResolveExternalPresentHookOwnerPath(entryTarget, ownerPath, sizeof(ownerPath));
            HookLogImportant(
                "DXGIShared: Refreshed stale external Present hook #%d (%p -> %p owner=%s) from the live entry %p",
                n, (void*)dxgi_shared_g_externalOverlayPresentHook, entryTarget,
                resolvedOwner ? ownerPath : "<unresolved thunk>", (void*)liveEntry);
        }
        dxgi_shared_g_externalOverlayPresentHook = refreshed;
    }
    return refreshed;
}
}

namespace DXGIShared {
// Validated accessor for the saved foreign Present handler: returns a handler that is safe to
// call right now, refreshing it from the live entry when the recorded one has gone stale, or
// nullptr when there is none (callers must then use CE's clean DXGI bypass).
PFN_Present GetCallableExternalOverlayPresentHook() {
    PFN_Present handler = dxgi_shared_g_externalOverlayPresentHook;
    if (handler && handler != DetourPresent && IsCallableForeignPresentHandler(reinterpret_cast<void*>(handler))) {
        return handler;
    }

    static std::atomic<int> s_staleLogCount{0};
    const int n = s_staleLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (n <= 10 || (n % 500) == 0) {
        HookLogImportant(
            "DXGIShared: Saved external Present hook %p is no longer callable #%d (the owning overlay rebuilt or "
            "freed its thunk) - re-deriving from the live entry",
            (void*)handler, n);
    }
    return RefreshExternalOverlayPresentHookFromLiveEntry();
}
}
