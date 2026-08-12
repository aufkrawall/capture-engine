#include "dxgi_shared_internal.h"

// Split out of dxgi_shared_original.cpp to keep both units under the source-size
// ceiling: CallOriginalPresent1 and the SL present-routing switch.

namespace DXGIShared {
HRESULT CallOriginalPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                             const DXGI_PRESENT_PARAMETERS* pParams) {
    if (!pSwapChain) {
        return DXGI_ERROR_INVALID_CALL;
    }
    if (HookIsShuttingDown()) {
        // Same rule as Present: a deep body hook means the foreign chain already ran above
        // this call, so the entry forward below must not re-enter it.
        if (dxgi_shared_oPresent1DeepBody) {
            return dxgi_shared_oPresent1DeepBody(pSwapChain, SyncInterval, Flags, pParams);
        }
        if (IsPresentEntryLeftToForeignChain() && dxgi_shared_oPresent1 &&
            dxgi_shared_oPresent1 != DetourPresent1) {
            return dxgi_shared_oPresent1(pSwapChain, SyncInterval, Flags, pParams);
        }
        // Same Steam external-chain hazard as Present - use the clean Present1
        // bypass instead of re-entering Steam without VEH recovery.
        if (IsSteamExternalChainTrampoline((void*)dxgi_shared_oPresent1Trampoline, nullptr,
                                           DetectAPIType(pSwapChain) == APIType::D3D12) &&
            dxgi_shared_oPresent1Bypass) {
            return dxgi_shared_oPresent1Bypass(pSwapChain, SyncInterval, Flags, pParams);
        }
        if (dxgi_shared_oPresent1Trampoline)
            return dxgi_shared_oPresent1Trampoline(pSwapChain, SyncInterval, Flags, pParams);
        if (dxgi_shared_oPresent1 && dxgi_shared_oPresent1 != DetourPresent1)
            return dxgi_shared_oPresent1(pSwapChain, SyncInterval, Flags, pParams);
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    WaitBackbufferFrameLatency(pSwapChain);

    // Multi-overlay foreign chain with CE below it: the deep trampoline is the remaining real
    // body. Re-entering the live Present1 entry would re-run the foreign chain and recurse.
    if (dxgi_shared_oPresent1DeepBody) {
        static std::atomic<int> s_deepBodyPresent1ForwardCount{0};
        const int forwardNum = s_deepBodyPresent1ForwardCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (forwardNum <= 5 || (forwardNum % 5000) == 0) {
            HookLogImportant("CallOriginalPresent1: foreign-chain deep body forward #%d (trampoline=%p)", forwardNum,
                             (void*)dxgi_shared_oPresent1DeepBody);
        }
        return dxgi_shared_oPresent1DeepBody(pSwapChain, SyncInterval, Flags, pParams);
    }

    // Multi-overlay foreign chain without a deep body hook: same rule as Present — CE owns no
    // entry bytes, so run the live Present1 entry and let the foreign chain compose itself.
    if (IsPresentEntryLeftToForeignChain()) {
        if (dxgi_shared_oPresent1 && dxgi_shared_oPresent1 != DetourPresent1) {
            static std::atomic<int> s_foreignChainPresent1ForwardCount{0};
            const int forwardNum = s_foreignChainPresent1ForwardCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (forwardNum <= 5 || (forwardNum % 5000) == 0) {
                HookLogImportant("CallOriginalPresent1: foreign-chain entry forward #%d (entry=%p)", forwardNum,
                                 (void*)dxgi_shared_oPresent1);
            }
            return dxgi_shared_oPresent1(pSwapChain, SyncInterval, Flags, pParams);
        }
        return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
    }

    const PFN_Present1 present1Trampoline = dxgi_shared_oPresent1Trampoline;
    const PFN_Present1 present1Original = dxgi_shared_oPresent1;
    const PFN_Present1 present1Bypass = EnsurePresent1BypassTrampoline();
    bool slLoaded = IsSLInterposerLoaded();

    const char* forcedBypassOverlay = nullptr;
    bool isD3D12SteamSwapChain = false;
    const bool forceSteamDX12Bypass = ShouldForceSteamDX12Bypass(
        pSwapChain, present1Bypass != nullptr, slLoaded, &forcedBypassOverlay, &isD3D12SteamSwapChain);

    // A preserved foreign Present1 trampoline is still a synchronous call into
    // that overlay. Apply the same provenance boundary as Present before the
    // natural inline chain can be entered from an FG runtime worker.
    if (present1Bypass && IsSteamOverlayModule(forcedBypassOverlay)) {
        const auto runtimeMode = g_FGCompat.GetRuntimeMode();
        const bool streamlineFGRunning = g_StreamlineFGRunning.load(std::memory_order_acquire);
        const bool postSLConfirmedRendering = HookIsPostSLOverlayConfirmedRendering();
        const bool runtimeCanPresentFromWorker = DXGIShared::CanRuntimePresentFromWorkerForExternalOverlay(
            isD3D12SteamSwapChain, false, streamlineFGRunning, postSLConfirmedRendering,
            ce::fg_runtime::RuntimeModeUsesFSR(runtimeMode), g_FGCompat.IsFSRFGApiActive(),
            HookHasRuntimeOwnedNativeFGPresentPath(), DoesFGRuntimeOwnSwapchain());
        const uint32_t currentThreadId = GetCurrentThreadId();
        const uint32_t trackedSourcePresentThreadId = DX12_GetGamePresentThreadId();
        if (runtimeCanPresentFromWorker &&
            !DXGIShared::ShouldInvokeSynchronousExternalOverlayPresentForThreadState(
                true, trackedSourcePresentThreadId, currentThreadId)) {
            static std::atomic<int> s_steamPresent1RuntimeWorkerBypassLogCount{0};
            const int bypassNum =
                s_steamPresent1RuntimeWorkerBypassLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (bypassNum <= 20 || bypassNum == 50 || (bypassNum % 500) == 0) {
                HookLogImportant(
                    "CallOriginalPresent1: refusing Steam Present1 transport on runtime worker #%d; using DXGI "
                    "bypass (runtime=%s slFG=%d confirmed=%d sourceTid=0x%04X currentTid=0x%04X)",
                    bypassNum, ce::fg_runtime::GetRuntimeModeName(runtimeMode), streamlineFGRunning ? 1 : 0,
                    postSLConfirmedRendering ? 1 : 0, trackedSourcePresentThreadId, currentThreadId);
            }
            return present1Bypass(pSwapChain, SyncInterval, Flags, pParams);
        }
    }

    // Inline-hook path: a trampoline prepended over Steam's Present1 entry
    // re-enters Steam's chain; there is no Present1 NULL-callback guard, so
    // the clean Present1 bypass (or the guarded Present transport) replaces
    // the bare trampoline call for that transport.
    if (present1Trampoline) {
        if (IsSteamExternalChainTrampoline((void*)present1Trampoline, nullptr,
                                           DetectAPIType(pSwapChain) == APIType::D3D12)) {
            if (present1Bypass) {
                return present1Bypass(pSwapChain, SyncInterval, Flags, pParams);
            }
            return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
        }
        return present1Trampoline(pSwapChain, SyncInterval, Flags, pParams);
    }

    if (forceSteamDX12Bypass) {
        if (slLoaded) {
            // SL case: use bypass trampoline (same as before, no Present1 guard available).
            static int s_forcedBypass1LogCount = 0;
            if (s_forcedBypass1LogCount++ < 10) {
                HookLogImportant("CallOriginalPresent1: forcing DXGI bypass for %s (slLoaded=%d)",
                                 forcedBypassOverlay ? forcedBypassOverlay : "overlay", slLoaded ? 1 : 0);
            }
        } else {
            // Non-Streamline case (e.g. Strange Brigade DX12 with only Steam overlay):
            // Same root cause as CallOriginalPresent: Steam's OverlayHookD3D3
            // needs vtable[8] = dxgi!Present to initialize.  The init is handled
            // by CallOriginalPresent on the first Present call.  For Present1,
            // only route through oPresent1 if Steam init has been completed;
            // otherwise use the bypass trampoline (safe fallback).
            //
            // Steam does NOT hook Present1 with an E9 JMP, so calling
            // present1Original directly on an already-initialized Steam is safe.
            if (!dxgi_shared_s_steamInitCrashed && dxgi_shared_s_steamDX12InitAttempted.load(std::memory_order_acquire) && present1Original &&
                present1Original != DetourPresent1 && IsReadableMemory(pSwapChain, sizeof(void*))) {
                static std::atomic<int> s_steamNonSLPresent1ViaE9JmpCount{0};
                if (s_steamNonSLPresent1ViaE9JmpCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                    HookLogImportant(
                        "CallOriginalPresent1: routing non-SL Steam overlay through "
                        "present1Original at %p (Steam init done)",
                        (void*)present1Original);
                }
                return present1Original(pSwapChain, SyncInterval, Flags, pParams);
            }

            static std::atomic<int> s_steamNonSLPresent1FallbackCount{0};
            if (s_steamNonSLPresent1FallbackCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                HookLogImportant(
                    "CallOriginalPresent1: non-SL Steam overlay — Present1 bypass "
                    "(initAttempted=%d initCrashed=%d)",
                    dxgi_shared_s_steamDX12InitAttempted.load(std::memory_order_acquire) ? 1 : 0, dxgi_shared_s_steamInitCrashed ? 1 : 0);
            }
        }
        return present1Bypass(pSwapChain, SyncInterval, Flags, pParams);
    }

    const char* thirdPartyBypassOverlay = nullptr;
    if (ShouldForceThirdPartyOverlayBypass(pSwapChain, present1Bypass != nullptr, &thirdPartyBypassOverlay)) {
        static int s_wrapperBypassLogCount = 0;
        if (s_wrapperBypassLogCount++ < 10) {
            HookLogImportant("CallOriginalPresent1: forcing bypass for wrapped present under overlay %s",
                             thirdPartyBypassOverlay);
        }
        return present1Bypass(pSwapChain, SyncInterval, Flags, pParams);
    }

    // CRITICAL: SL worker thread guard — same as CallOriginalPresent.
    // When SL is loaded, call oPresent1 directly (same reason as Present).
    if (slLoaded && present1Original && present1Original != DetourPresent1) {
        return present1Original(pSwapChain, SyncInterval, Flags, pParams);
    }

    // Prefer the current object's Present1 slot when it is not detoured.
    if (IsReadableMemory(pSwapChain, sizeof(void*))) {
        void** vtable = *(void***)pSwapChain;
        if (vtable && IsReadableMemory(reinterpret_cast<const void*>(vtable), 23 * sizeof(void*)) && vtable[22]) {
            auto currentPresent1 = reinterpret_cast<PFN_Present1>(vtable[22]);
            if (currentPresent1 != DetourPresent1) {
                return currentPresent1(pSwapChain, SyncInterval, Flags, pParams);
            }
        }
    }

    // Vtable-hook path fallback: use saved original only if it is not detoured.
    if (present1Original && present1Original != DetourPresent1) {
        return present1Original(pSwapChain, SyncInterval, Flags, pParams);
    }

    // Last resort: fall back to Present.
    return CallOriginalPresent(pSwapChain, SyncInterval, Flags);
}
}

namespace DXGIShared {
void DisableSLPresentRouting() {
    bool wasActive = dxgi_shared_s_slRoutingActive.exchange(false, std::memory_order_acq_rel);
    if (wasActive) {
        HookLogImportant(
            "SL routing DISABLED: Present calls will bypass SL hook chain and "
            "go through trampoline=%p directly (FSR FG or runtime-owned FG takeover)",
            dxgi_shared_oPresentTrampoline);
    }
}
}
