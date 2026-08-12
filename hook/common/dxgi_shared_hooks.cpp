#include "dxgi_shared_internal.h"

namespace DXGIShared {
bool HasExternalEntryHook(const void* target) {
    const auto* code = static_cast<const uint8_t*>(target);
    if (!IsReadableMemory(code, 16)) {
        return false;
    }
    return code[0] == 0xE9 || (code[0] == 0xFF && code[1] == 0x25);
}
}

namespace DXGIShared {
// Resolves the target of an E9 (near JMP) hook at the given function address.
// Returns the absolute address of the hook handler, or nullptr if no E9 JMP
// is present or the function body is unreadable.
void* ResolveE9JmpTarget(void* funcAddress) {
    if (!funcAddress) {
        return nullptr;
    }
    const auto* code = static_cast<const uint8_t*>(funcAddress);
    if (!IsReadableMemory(code, 5)) {
        return nullptr;
    }
    if (code[0] != 0xE9) {
        return nullptr;
    }
    int32_t relOffset;
    memcpy(&relOffset, code + 1, sizeof(relOffset));
    return static_cast<uint8_t*>(funcAddress) + 5 + relOffset;
}
}

namespace DXGIShared {
void* ResolveFF25JmpTarget(void* funcAddress) {
    if (!funcAddress) {
        return nullptr;
    }
    const auto* code = static_cast<const uint8_t*>(funcAddress);
    if (!IsReadableMemory(code, 14)) {
        return nullptr;
    }
    if (code[0] != 0xFF || code[1] != 0x25) {
        return nullptr;
    }

    int32_t dispOffset = 0;
    memcpy(&dispOffset, code + 2, sizeof(dispOffset));
    const void* targetSlot = code + 6 + dispOffset;
    if (!IsReadableMemory(targetSlot, sizeof(void*))) {
        return nullptr;
    }
    // Foreign hook thunks are byte-packed, so the pointer slot is not guaranteed to be
    // 8-byte aligned; a direct pointer load there is UB (UBSan misaligned-load report).
    void* target = nullptr;
    memcpy(static_cast<void*>(&target), targetSlot, sizeof(target));
    return target;
}
}

namespace DXGIShared {
PFN_Present EnsurePresentBypassTrampoline() {
    if (dxgi_shared_oPresentBypass) {
        return dxgi_shared_oPresentBypass;
    }

    std::lock_guard<std::mutex> lock(g_SharedMutex);
    if (dxgi_shared_oPresentBypass) {
        return dxgi_shared_oPresentBypass;
    }

    const PFN_Present presentOriginal = dxgi_shared_oPresent;
    if (!presentOriginal || presentOriginal == DetourPresent || !HasExternalEntryHook((const void*)presentOriginal)) {
        return nullptr;
    }

    void* bypass = InlineHook::CreateBypassTrampoline((void*)presentOriginal);
    if (!bypass) {
        static int s_bypassFailLogCount = 0;
        if (s_bypassFailLogCount++ < 5) {
            HookLogImportant("DXGIShared: Failed to lazily create Present bypass trampoline from %p", presentOriginal);
        }
        return nullptr;
    }

    dxgi_shared_oPresentBypass = (PFN_Present)bypass;
    HookLogImportant("DXGIShared: Lazily created Present bypass trampoline at %p from %p", bypass, presentOriginal);
    return dxgi_shared_oPresentBypass;
}
}

namespace DXGIShared {
PFN_Present1 EnsurePresent1BypassTrampoline() {
    if (dxgi_shared_oPresent1Bypass) {
        return dxgi_shared_oPresent1Bypass;
    }

    std::lock_guard<std::mutex> lock(g_SharedMutex);
    if (dxgi_shared_oPresent1Bypass) {
        return dxgi_shared_oPresent1Bypass;
    }

    const PFN_Present1 present1Original = dxgi_shared_oPresent1;
    if (!present1Original || present1Original == DetourPresent1 ||
        !HasExternalEntryHook((const void*)present1Original)) {
        return nullptr;
    }

    void* bypass = InlineHook::CreateBypassTrampoline((void*)present1Original);
    if (!bypass) {
        static int s_bypassFailLogCount = 0;
        if (s_bypassFailLogCount++ < 5) {
            HookLogImportant("DXGIShared: Failed to lazily create Present1 bypass trampoline from %p",
                             present1Original);
        }
        return nullptr;
    }

    dxgi_shared_oPresent1Bypass = (PFN_Present1)bypass;
    HookLogImportant("DXGIShared: Lazily created Present1 bypass trampoline at %p from %p", bypass, present1Original);
    return dxgi_shared_oPresent1Bypass;
}
}

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourSetColorSpace1(IDXGISwapChain* pSwapChain, DXGI_COLOR_SPACE_TYPE colorSpace) {
    const PFN_SetColorSpace1 original = dxgi_shared_oSetColorSpace1Trampoline.load(std::memory_order_acquire);
    if (!original) {
        static std::atomic<int> s_missingTrampolineLogCount{0};
        if (s_missingTrampolineLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            HookLogImportant(
                "DXGI: SetColorSpace1 detour entered without a published trampoline; failing closed sc=%p cs=%d",
                pSwapChain, static_cast<int>(colorSpace));
        }
        return DXGI_ERROR_UNSUPPORTED;
    }

    const HRESULT result = original(pSwapChain, colorSpace);
    if (!HookIsShuttingDown() && SUCCEEDED(result) &&
        ce::presentation_color::ShouldRecordDetouredColorSpaceChange(dxgi_shared_s_wrapperSetColorSpaceForwardDepth)) {
        bool changed = false;
        if (RecordSwapChainColorSpace(pSwapChain, colorSpace, &changed) && changed) {
            HookLogImportant("DXGI: Swapchain presentation color space changed source=inline sc=%p cs=%d",
                             pSwapChain, static_cast<int>(colorSpace));
        }
    }
    return result;
}
}

namespace DXGIShared {
HRESULT SetSwapChainColorSpaceFromWrapper(IDXGISwapChain3* callableSwapChain, IDXGISwapChain* identitySwapChain,
                                          DXGI_COLOR_SPACE_TYPE colorSpace) {
    if (!callableSwapChain) {
        return DXGI_ERROR_UNSUPPORTED;
    }

    ++dxgi_shared_s_wrapperSetColorSpaceForwardDepth;
    const auto depthGuard = ce::make_scope_guard([]() { --dxgi_shared_s_wrapperSetColorSpaceForwardDepth; });
    const HRESULT result = callableSwapChain->SetColorSpace1(colorSpace);
    if (SUCCEEDED(result)) {
        IDXGISwapChain* identity = identitySwapChain ? identitySwapChain : callableSwapChain;
        bool changed = false;
        if (RecordSwapChainColorSpace(identity, colorSpace, &changed) && changed) {
            HookLogImportant("DXGI: Swapchain presentation color space changed source=wrapper sc=%p cs=%d",
                             identity, static_cast<int>(colorSpace));
        }
    }
    return result;
}
}

namespace DXGIShared {
void PublishSetColorSpace1Trampoline(void* trampoline, void*) {
    dxgi_shared_oSetColorSpace1Trampoline.store(reinterpret_cast<PFN_SetColorSpace1>(trampoline), std::memory_order_release);
}
}

namespace DXGIShared {
bool InstallSetColorSpace1InlineHook(IDXGISwapChain* pSwapChain, const char* source) {
    if (!pSwapChain) {
        return false;
    }
    if (dxgi_shared_oSetColorSpace1Trampoline.load(std::memory_order_acquire)) {
        return true;
    }

    // Inline-hook the real DXGI implementation only. Hooking the wrapper method
    // would make the detour's trampoline call back into the wrapper and recreate
    // the unsafe wrapper/detour composition that this path is designed to avoid.
    if (IsWrappedSwapChainObject(pSwapChain)) {
        static std::atomic<int> s_wrapperTargetLogCount{0};
        if (s_wrapperTargetLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLog("DXGI: SetColorSpace1 inline tracking skipped for wrapped %s swapchain %p",
                    source ? source : "unknown", pSwapChain);
        }
        return false;
    }

    std::lock_guard<std::mutex> installLock(dxgi_shared_s_setColorSpace1HookMutex);
    if (dxgi_shared_oSetColorSpace1Trampoline.load(std::memory_order_acquire)) {
        return true;
    }

    IDXGISwapChain3* colorSpaceSwapChain = nullptr;
    if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&colorSpaceSwapChain))) || !colorSpaceSwapChain) {
        static std::atomic<int> s_unsupportedLogCount{0};
        if (s_unsupportedLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLog("DXGI: SetColorSpace1 tracking unavailable for %s swapchain %p (IDXGISwapChain3 unsupported)",
                    source ? source : "unknown", pSwapChain);
        }
        return false;
    }

    void** colorSpaceVtable = *reinterpret_cast<void***>(colorSpaceSwapChain);
    void* colorSpaceAddress =
        colorSpaceVtable && IsReadableMemory(reinterpret_cast<const void*>(&colorSpaceVtable[38]), sizeof(void*)) ? colorSpaceVtable[38] : nullptr;
    colorSpaceSwapChain->Release();
    if (!colorSpaceAddress || colorSpaceAddress == reinterpret_cast<void*>(DetourSetColorSpace1)) {
        HookLogImportant("DXGI: Refusing unsafe SetColorSpace1 hook target source=%s sc=%p target=%p",
                         source ? source : "unknown", pSwapChain, colorSpaceAddress);
        return false;
    }

    void* colorSpaceTrampoline = nullptr;
    if (!InlineHook::InstallPublished(colorSpaceAddress, reinterpret_cast<void*>(DetourSetColorSpace1),
                                      &colorSpaceTrampoline, PublishSetColorSpace1Trampoline, nullptr)) {
        if (dxgi_shared_oSetColorSpace1Trampoline.load(std::memory_order_acquire)) {
            return true;
        }
        HookLogImportant(
            "DXGI: SetColorSpace1 inline tracking unavailable source=%s sc=%p target=%p; wrapper tracking remains available",
            source ? source : "unknown", pSwapChain, colorSpaceAddress);
        return false;
    }

    HookLogImportant("DXGI: SetColorSpace1 inline tracking installed source=%s target=%p trampoline=%p",
                     source ? source : "unknown", colorSpaceAddress, colorSpaceTrampoline);
    return true;
}
}

namespace DXGIShared {
bool InstallHooks(IDXGISwapChain* pSwapChain, bool presentOnly) {
    // NOTE: This function should only be called for DX11/DX10 games.
    // DX12 games use wrapper-based Present interception (CWrapDXGISwapChain).
    // Calling this for DX12 can cause conflicts and stack overflow crashes
    // due to two competing Present interception mechanisms.
    // See: dx12_hook.cpp for the wrapper-based approach.

    if (!pSwapChain)
        return false;

    static std::atomic<int> s_installCount{0};
    int count = s_installCount.fetch_add(1);
    HookLog("DXGIShared::InstallHooks CALLED #%d (swapchain=%p, presentOnly=%d)", count, pSwapChain,
            presentOnly ? 1 : 0);

    InstallSetColorSpace1InlineHook(pSwapChain, "vtable-path");

    // Third-party overlays can install their own DXGI hooks and form recursive
    // Present chains with vtable patching. In that case the wrapper-based path
    // remains active and avoids hook wars.
    if (!DXGIShared::ShouldInstallSwapchainHooksWithThirdPartyOverlay(IsThirdPartyOverlayLoaded(),
                                                                      HasPresentDetourHooks(),
                                                                      IsPresentEntryLeftToForeignChain())) {
        HookLogImportant(
            "DXGIShared::InstallHooks: Multi-overlay foreign Present chain owns the entry, keeping the swapchain "
            "vtable pristine (wrapper-only interception)");
        return true;
    }

    std::lock_guard<std::mutex> installLock(g_SharedMutex);

    if (dxgi_shared_s_hookedVTable) {
        void** newVTable = *(void***)pSwapChain;
        if (newVTable == dxgi_shared_s_hookedVTable) {
            HookLog("DXGIShared::InstallHooks: Hooks already installed on vtable %p", dxgi_shared_s_hookedVTable);
            return true;
        }
        // The detours use one predecessor set. Replacing that set while the old
        // vtable can still call CE would route in-flight calls through the wrong
        // implementation. Inline/wrapper interception remains available for a
        // distinct proxy vtable, so preserve the established chain.
        HookLogImportant(
            "DXGIShared::InstallHooks: Preserving established vtable chain old=%p new=%p; "
            "using inline/wrapper interception for the distinct vtable",
            dxgi_shared_s_hookedVTable, newVTable);
        return true;
    }

    void** vtable = *(void***)pSwapChain;
    if (!vtable) {
        HookLog("DXGIShared::InstallHooks: Invalid vtable");
        return false;
    }

    DWORD oldProtect;
    if (!VirtualProtect(reinterpret_cast<void*>(vtable), 40 * sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        HookLog("DXGIShared::InstallHooks: VirtualProtect failed");
        return false;
    }

    const auto claimSlot = [&]<typename FunctionPointer>(size_t index, void* detour, FunctionPointer* predecessor,
                                                         const char* method) {
        void** entry = &vtable[index];
        void* current = InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(entry), nullptr, nullptr);
        if (!current || current == detour) {
            HookLogImportant("DXGIShared: Refusing ambiguous %s vtable claim entry=%p current=%p", method, entry,
                             current);
            return false;
        }

        // Publish the predecessor before making the detour callable. The shared
        // install mutex serializes competing CE installs; CAS preserves a foreign
        // injector that wins after our observation.
        FunctionPointer previousPredecessor = *predecessor;
        *predecessor = reinterpret_cast<FunctionPointer>(current);
        void* replaced = InterlockedCompareExchangePointer(reinterpret_cast<PVOID volatile*>(entry), detour, current);
        if (replaced != current) {
            *predecessor = previousPredecessor;
            HookLogImportant(
                "DXGIShared: Preserving concurrent foreign %s vtable replacement entry=%p expected=%p observed=%p",
                method, entry, current, replaced);
            return false;
        }

        if (*entry != detour) {
            HookLogImportant(
                "DXGIShared: Foreign %s hook followed CE at entry=%p current=%p; retaining CE predecessor=%p",
                method, entry, *entry, current);
        }
        HookLog("DXGIShared: Hooked %s at vtable[%zu] (original=%p, detour=%p)", method, index, current,
                detour);
        return true;
    };

    if (!claimSlot(8, (void*)DetourPresent, &dxgi_shared_oPresent, "Present")) {
        VirtualProtect(reinterpret_cast<void*>(vtable), 40 * sizeof(void*), oldProtect, &oldProtect);
        return false;
    }
    dxgi_shared_s_hookedVTable = vtable;

    claimSlot(22, (void*)DetourPresent1, &dxgi_shared_oPresent1, "Present1");

    if (!presentOnly) {
        claimSlot(13, (void*)DetourResizeBuffers, &dxgi_shared_oResizeBuffers, "ResizeBuffers");
        claimSlot(39, (void*)DetourResizeBuffers1, &dxgi_shared_oResizeBuffers1, "ResizeBuffers1");
    }

    VirtualProtect(reinterpret_cast<void*>(vtable), 40 * sizeof(void*), oldProtect, &oldProtect);
    HookLog("DXGIShared::InstallHooks: All vtable hooks installed successfully");
    return true;
}
}

namespace DXGIShared {
bool HasPresentInlineHooks() {
    return dxgi_shared_oPresentTrampoline != nullptr || dxgi_shared_oPresent1Trampoline != nullptr;
}
}

namespace DXGIShared {
bool HasPresentDetourHooks() {
    return dxgi_shared_s_hookedVTable != nullptr || dxgi_shared_oPresentTrampoline != nullptr || dxgi_shared_oPresent1Trampoline != nullptr;
}
}

namespace DXGIShared {
bool CanSafelyInstallExternalPresentDetourPath(bool requiresBypassTrampoline, bool bypassTrampolineAvailable) {
    return !requiresBypassTrampoline || bypassTrampolineAvailable;
}
}

namespace DXGIShared {
void SetPendingSwapChainForLazyHook(IDXGISwapChain* pSwapChain) {
    if (pSwapChain) {
        pSwapChain->AddRef();
    }
    if (dxgi_shared_s_PendingSwapChainForLazyHook) {
        dxgi_shared_s_PendingSwapChainForLazyHook->Release();
    }
    dxgi_shared_s_PendingSwapChainForLazyHook = pSwapChain;
    HookLog("DXGIShared: SetPendingSwapChainForLazyHook called");
}
}

namespace DXGIShared {
void InstallHooksIfPending(IDXGISwapChain* pSwapChain) {
    if (dxgi_shared_s_LazyHooksInstalled.load(std::memory_order_acquire))
        return;

    // Check if this is the pending swapchain
    if (pSwapChain == dxgi_shared_s_PendingSwapChainForLazyHook) {
        HookLog("DXGIShared: Installing hooks lazily on first Present");
        // CRITICAL: Use presentOnly=true to only hook Present/Present1
        // ResizeBuffers hooks can cause stack overflow crashes with some overlays
        InstallHooks(pSwapChain, true);
        dxgi_shared_s_LazyHooksInstalled.store(true, std::memory_order_release);
        if (dxgi_shared_s_PendingSwapChainForLazyHook) {
            dxgi_shared_s_PendingSwapChainForLazyHook->Release();
            dxgi_shared_s_PendingSwapChainForLazyHook = nullptr;
        }
    }
}
}

namespace DXGIShared {
void Init() {
    g_SharedState.lastSwapchainCreation = std::chrono::steady_clock::now();
    // Early detection of NVIDIA Smooth Motion module
    g_FGCompat.CheckForNvPresent();
}
}
