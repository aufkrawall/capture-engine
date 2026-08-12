#include "dxgi_shared_internal.h"
#include "../wrappers/vtable_hook_policy.h"

// Swapchain Present/Present1/ResizeBuffers vtable-slot ownership: claim repair, detach for a
// runtime handoff, and full teardown. Split out of dxgi_shared_hooks_present.cpp (which owns
// the entry/body inline-hook install decision) to keep both units inside the file-size ceiling;
// the two share nothing but the DXGIShared state they operate on.
//
// The single rule every function here obeys: CE only ever writes back a slot it can prove it
// still owns, with an interlocked compare against its own detour, and preserves any foreign
// replacement it finds instead. The class vftable page is read-only between operations, so
// observation must be a plain volatile read — a locked read-modify-write faults before
// VirtualProtect runs (build 0.1.5914, crash 20260811_192706).

namespace {
enum class VTableDetachResult {
    Detached,
    ForeignPreserved,
    Failed,
};

VTableDetachResult DetachOwnedVTableSlot(void** entry, void* detour, void* predecessor, const char* method) {
    // The hooked swapchain vtable is the class vftable inside the DXGI image,
    // whose page stays read-only between repair/detach operations. `lock
    // cmpxchg` requires write access even when it is only used as a read, so
    // observe with a plain volatile read; the atomic exchange below runs
    // inside the VirtualProtect region.
    void* current = *reinterpret_cast<void* volatile*>(entry);
    if (predecessor && current == predecessor)
        return VTableDetachResult::Detached;
    if (current != detour) {
        if (!predecessor)
            return VTableDetachResult::Detached;
        HookLogImportant("DXGIShared: Preserving foreign %s vtable replacement %p during detach", method, current);
        return VTableDetachResult::ForeignPreserved;
    }
    if (!predecessor) {
        HookLogImportant("DXGIShared: Cannot detach %s vtable hook without its predecessor", method);
        return VTableDetachResult::Failed;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(entry), sizeof(void*), PAGE_READWRITE, &oldProtect))
        return VTableDetachResult::Failed;
    void* replaced = InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(entry), predecessor, detour);
    DWORD ignoredProtect = 0;
    VirtualProtect(reinterpret_cast<void*>(entry), sizeof(void*), oldProtect, &ignoredProtect);
    if (replaced != detour) {
        HookLogImportant("DXGIShared: Preserving concurrent foreign %s vtable replacement %p during detach", method,
                         replaced);
        return VTableDetachResult::ForeignPreserved;
    }

    HookLog("DXGIShared: Detached %s vtable hook", method);
    return VTableDetachResult::Detached;
}

bool IsDetached(VTableDetachResult result) {
    return result == VTableDetachResult::Detached;
}
}

namespace DXGIShared {
void RemovePresentHooks() {
    InlineHook::RemoveAll();
    dxgi_shared_oSetColorSpace1Trampoline.store(nullptr, std::memory_order_release);

    dxgi_shared_s_slRoutingActive.store(false, std::memory_order_release);
    dxgi_shared_oPresentBypass = nullptr;
    dxgi_shared_oPresent1Bypass = nullptr;

    std::lock_guard<std::mutex> lock(g_SharedMutex);
    if (!dxgi_shared_s_hookedVTable)
        return;
    if (!IsReadableMemory(reinterpret_cast<const void*>(dxgi_shared_s_hookedVTable), 23 * sizeof(void*)))
        return;
    DetachOwnedVTableSlot(&dxgi_shared_s_hookedVTable[8], (void*)DetourPresent, (void*)dxgi_shared_oPresent,
                          "Present");
    DetachOwnedVTableSlot(&dxgi_shared_s_hookedVTable[22], (void*)DetourPresent1, (void*)dxgi_shared_oPresent1,
                          "Present1");

}
}

namespace DXGIShared {
void ReleaseSwapchainPresentVTableHooksForRuntimeHandoff(const char* reason) {
    std::lock_guard<std::mutex> lock(g_SharedMutex);
    if (!dxgi_shared_s_hookedVTable) {
        return;
    }
    if (!IsReadableMemory(reinterpret_cast<const void*>(dxgi_shared_s_hookedVTable), 40 * sizeof(void*))) {
        HookLogImportant(
            "DXGIShared: Cannot release Present vtable hooks for runtime handoff; vtable %p is not readable "
            "(reason=%s)",
            dxgi_shared_s_hookedVTable, reason ? reason : "unknown");
        return;
    }

    const VTableDetachResult presentResult = DetachOwnedVTableSlot(
        &dxgi_shared_s_hookedVTable[8], (void*)DetourPresent, (void*)dxgi_shared_oPresent, "Present");
    const VTableDetachResult present1Result = DetachOwnedVTableSlot(
        &dxgi_shared_s_hookedVTable[22], (void*)DetourPresent1, (void*)dxgi_shared_oPresent1, "Present1");
    const bool restoredPresent = IsDetached(presentResult);
    const bool restoredPresent1 = IsDetached(present1Result);
    const bool resizeChainRetained =
        dxgi_shared_oResizeBuffers && dxgi_shared_s_hookedVTable[13] != (void*)dxgi_shared_oResizeBuffers;
    const bool resize1ChainRetained =
        dxgi_shared_oResizeBuffers1 && dxgi_shared_s_hookedVTable[39] != (void*)dxgi_shared_oResizeBuffers1;

    if (restoredPresent || restoredPresent1) {
        HookLogImportant(
            "DXGIShared: Released swapchain Present vtable hooks for runtime handoff "
            "(present=%d present1=%d vtable=%p restored8=%p restored22=%p reason=%s)",
            restoredPresent ? 1 : 0, restoredPresent1 ? 1 : 0, dxgi_shared_s_hookedVTable,
            restoredPresent ? (void*)dxgi_shared_oPresent : dxgi_shared_s_hookedVTable[8],
            restoredPresent1 ? (void*)dxgi_shared_oPresent1 : dxgi_shared_s_hookedVTable[22], reason ? reason : "unknown");
        if (restoredPresent && restoredPresent1 && !resizeChainRetained && !resize1ChainRetained) {
            dxgi_shared_s_hookedVTable = nullptr;
            dxgi_shared_s_slRoutingActive.store(false, std::memory_order_release);
            dxgi_shared_oPresentBypass = nullptr;
            dxgi_shared_oPresent1Bypass = nullptr;
        } else {
            HookLogImportant(
                "DXGIShared: Retaining vtable chain state because a foreign Present replacement may still call CE");
        }
    }
}
}

namespace DXGIShared {
void RepairVTableHooksIfNeeded() {
    // CRITICAL: Do NOT access the swapchain vtable during Streamline's critical
    // initialization window.  Inside Hooked_slDLSSGGetState (called during
    // sl_common!slGetPluginFunction from SL's DllMain), reading the vtable
    // triggers Steam's overlay hook chain (gameoverlayrenderer64!OverlayHookD3D3)
    // which may still be partially initialized and crash with a null function
    // pointer call (RIP=0, RAX=0).  This guard is state-based (PostSL confirmed
    // rendering) rather than timer-based because SL's background DllMain duration
    // varies and can exceed the startup window timer.
    if (DXGIShared::ShouldDeferVTableRepairDuringStreamlineStartup(
            g_StreamlineFGRunning.load(std::memory_order_acquire), DXGIShared::IsStreamlineStartupHandoffPending(),
            DXGIShared::IsStreamlineStartupTransitionWindowActive(), HookIsPostSLOverlayConfirmedRendering())) {
        return;
    }

    std::lock_guard<std::mutex> lock(g_SharedMutex);
    if (!dxgi_shared_s_hookedVTable) {
        static std::atomic<uint32_t> s_nullLogCount{0};
        if (s_nullLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLogImportant("DXGIShared: RepairVTable — s_hookedVTable is NULL, cannot repair");
        }
        return;
    }
    if (!IsReadableMemory(reinterpret_cast<const void*>(dxgi_shared_s_hookedVTable), 23 * sizeof(void*))) {
        HookLogImportant("DXGIShared: RepairVTable — s_hookedVTable %p not readable", dxgi_shared_s_hookedVTable);
        return;
    }

    bool repaired = false;
    bool foreignPreserved = false;
    static std::atomic<uint32_t> s_foreignPreserveLogCount{0};
    const auto repairRestoredSlot = [&](void** entry, void* detour, void* predecessor, const char* method) {
        if (!predecessor)
            return false;
        // Same read-only class-vftable constraint as DetachOwnedVTableSlot:
        // observation must be a plain volatile read, never a locked operation,
        // which would fault on the read-only page before VirtualProtect runs.
        void* current = *reinterpret_cast<void* volatile*>(entry);
        if (current == detour)
            return false;
        if (ce::vtable_hook_policy::ShouldPreserveForeignFollower(current, detour, predecessor)) {
            foreignPreserved = true;
            const uint32_t occurrence = s_foreignPreserveLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (occurrence <= 3 || occurrence % 300 == 0) {
                HookLogImportant(
                    "DXGIShared: Preserving foreign %s vtable replacement %p; it may retain CE as its next link "
                    "(occurrence=%u)",
                    method, current, occurrence);
            }
            return false;
        }
        if (!ce::vtable_hook_policy::ShouldReclaimRestoredSlot(current, detour, predecessor))
            return false;

        DWORD oldProtect = 0;
        if (!VirtualProtect(reinterpret_cast<void*>(entry), sizeof(void*), PAGE_READWRITE, &oldProtect))
            return false;
        void* replaced = InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(entry), detour,
                                                           predecessor);
        DWORD ignoredProtect = 0;
        VirtualProtect(reinterpret_cast<void*>(entry), sizeof(void*), oldProtect, &ignoredProtect);
        if (replaced != predecessor) {
            foreignPreserved = true;
            HookLogImportant("DXGIShared: Preserved concurrent %s replacement %p during repair", method, replaced);
            return false;
        }
        HookLogImportant("DXGIShared: Reclaimed %s slot after predecessor restoration (predecessor=%p)", method,
                         predecessor);
        return true;
    };

    repaired |= repairRestoredSlot(&dxgi_shared_s_hookedVTable[8], (void*)DetourPresent,
                                   (void*)dxgi_shared_oPresent, "Present");
    repaired |= repairRestoredSlot(&dxgi_shared_s_hookedVTable[22], (void*)DetourPresent1,
                                   (void*)dxgi_shared_oPresent1, "Present1");

    static std::atomic<uint32_t> s_intactLogCount{0};
    if (repaired) {
        s_intactLogCount.store(0, std::memory_order_relaxed);
    } else if (!foreignPreserved) {
        if (s_intactLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLogImportant("DXGIShared: RepairVTableHooksIfNeeded — hooks intact (vtable=%p, [8]=%p, [22]=%p)",
                             dxgi_shared_s_hookedVTable, dxgi_shared_s_hookedVTable[8], dxgi_shared_s_hookedVTable[22]);
        }
    }
}
}

namespace DXGIShared {
void RemoveSwapchainVTableHooks() {
    InlineHook::RemoveAll();
    dxgi_shared_oSetColorSpace1Trampoline.store(nullptr, std::memory_order_release);

    dxgi_shared_s_slRoutingActive.store(false, std::memory_order_release);
    dxgi_shared_oPresentBypass = nullptr;
    dxgi_shared_oPresent1Bypass = nullptr;

    std::lock_guard<std::mutex> lock(g_SharedMutex);
    if (!dxgi_shared_s_hookedVTable)
        return;
    if (!IsReadableMemory(reinterpret_cast<const void*>(dxgi_shared_s_hookedVTable), 40 * sizeof(void*))) {
        HookLogImportant("DXGIShared: Cannot detach unreadable swapchain vtable %p", dxgi_shared_s_hookedVTable);
        return;
    }
    const bool presentDetached = IsDetached(DetachOwnedVTableSlot(
        &dxgi_shared_s_hookedVTable[8], (void*)DetourPresent, (void*)dxgi_shared_oPresent, "Present"));
    const bool present1Detached = IsDetached(DetachOwnedVTableSlot(
        &dxgi_shared_s_hookedVTable[22], (void*)DetourPresent1, (void*)dxgi_shared_oPresent1, "Present1"));
    const bool resizeDetached = IsDetached(DetachOwnedVTableSlot(
        &dxgi_shared_s_hookedVTable[13], (void*)DetourResizeBuffers, (void*)dxgi_shared_oResizeBuffers,
        "ResizeBuffers"));
    const bool resize1Detached = IsDetached(DetachOwnedVTableSlot(
        &dxgi_shared_s_hookedVTable[39], (void*)DetourResizeBuffers1, (void*)dxgi_shared_oResizeBuffers1,
        "ResizeBuffers1"));

    if (presentDetached && present1Detached && resizeDetached && resize1Detached) {
        dxgi_shared_s_hookedVTable = nullptr;
        HookLog("DXGIShared: All swapchain vtable hooks detached");
    } else {
        HookLogImportant("DXGIShared: Retaining vtable chain state for foreign followers");
    }
}
}
