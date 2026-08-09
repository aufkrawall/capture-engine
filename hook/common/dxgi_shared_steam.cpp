#include "dxgi_shared_internal.h"

namespace DXGIShared {
// Fallback only: Steam's NULL Present-shaped callbacks should normally be
// patched to CE's DXGI bypass trampoline so Steam can keep chaining to a real
// Present.  This no-op is used only when a bypass trampoline is not available.
HRESULT WINAPI SteamDummyRenderingCallback(IDXGISwapChain* /*pSwapChain*/, UINT /*SyncInterval*/,
                                                  UINT /*Flags*/) {
    return S_OK;
}
}

namespace DXGIShared {
void* SelectSteamNullCallbackRecoveryTarget(const SteamNullCallbackRecoveryContext& recoveryContext) {
    const bool hasBypass = recoveryContext.bypass != nullptr;
    return DXGIShared::SelectSteamNullCallbackRecoveryPatchTarget(hasBypass) ==
                   DXGIShared::SteamNullCallbackRecoveryPatchTarget::DXGIBypassPresent
               ? recoveryContext.bypass
               : reinterpret_cast<void*>(SteamDummyRenderingCallback);
}
}

namespace DXGIShared {
bool IsSteamOverlayModule(const char* overlayModule) {
    return overlayModule && ce::overlay_compat::detail::ContainsInsensitive(overlayModule, "gameoverlayrenderer");
}
}

namespace DXGIShared {
bool IsStreamlineModuleHandle(HMODULE moduleHandle) {
    if (!moduleHandle) {
        return false;
    }

    char modulePath[MAX_PATH] = {};
    if (GetModuleFileNameA(moduleHandle, modulePath, MAX_PATH) == 0) {
        return false;
    }

    return ce::overlay_compat::detail::ContainsInsensitive(modulePath, "sl.interposer") ||
           ce::overlay_compat::detail::ContainsInsensitive(modulePath, "sl.common") ||
           ce::overlay_compat::detail::ContainsInsensitive(modulePath, "sl.dlss_g");
}
}

namespace DXGIShared {
bool IsCaptureHookModulePath(const char* modulePath) {
    return modulePath && ce::overlay_compat::detail::ContainsInsensitive(modulePath, "capture_hook");
}
}

namespace DXGIShared {
bool IsCodeAddressFromStreamlineModule(const void* codeAddress) {
    if (!codeAddress) {
        return false;
    }

    HMODULE callerModule = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            reinterpret_cast<LPCSTR>(codeAddress), &callerModule)) {
        return false;
    }
    return IsStreamlineModuleHandle(callerModule);
}
}

namespace DXGIShared {
bool TryGetModulePathFromCodeAddress(const void* codeAddress, char* modulePathOut, size_t modulePathOutCount,
                                            HMODULE* moduleOut ) {
    return ce::overlay_compat::TryGetModulePathFromCodeAddress(codeAddress, modulePathOut, modulePathOutCount,
                                                               moduleOut);
}
}

namespace DXGIShared {
bool HasStartupBlockingOverlayModuleInCurrentStack() {
    constexpr USHORT kMaxFrames = 16;
    void* stackFrames[kMaxFrames] = {};
    const USHORT frameCount = CaptureStackBackTrace(0, kMaxFrames, stackFrames, nullptr);
    for (USHORT i = 0; i < frameCount; ++i) {
        char modulePath[MAX_PATH] = {};
        if (TryGetModulePathFromCodeAddress(stackFrames[i], modulePath, sizeof(modulePath)) &&
            ce::overlay_compat::IsStartupBlockingOverlayModulePath(modulePath)) {
            return true;
        }
    }

    return false;
}
}

namespace DXGIShared {
bool HasStreamlineModuleInCurrentStack() {
    constexpr USHORT kMaxFrames = 24;
    void* stackFrames[kMaxFrames] = {};
    const USHORT frameCount = CaptureStackBackTrace(0, kMaxFrames, stackFrames, nullptr);
    for (USHORT i = 0; i < frameCount; ++i) {
        HMODULE module = nullptr;
        char modulePath[MAX_PATH] = {};
        if (TryGetModulePathFromCodeAddress(stackFrames[i], modulePath, sizeof(modulePath), &module) &&
            IsStreamlineModuleHandle(module)) {
            return true;
        }
    }

    return false;
}
}

namespace DXGIShared {
bool ShouldForceSteamDX12Bypass(IDXGISwapChain* pSwapChain, bool bypassAvailable, bool slLoaded,
                                const char** overlayModuleOut, bool* isD3D12SwapChainOut) {
    const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
    if (overlayModuleOut) {
        *overlayModuleOut = overlayModule;
    }
    if (isD3D12SwapChainOut) {
        *isD3D12SwapChainOut = false;
    }
    if (!pSwapChain || !bypassAvailable || !IsSteamOverlayModule(overlayModule)) {
        return false;
    }

    const bool isD3D12SwapChain = DetectAPIType(pSwapChain) == APIType::D3D12;
    if (isD3D12SwapChainOut) {
        *isD3D12SwapChainOut = isD3D12SwapChain;
    }

    return ShouldForceSteamDX12BypassForState(
        bypassAvailable, true, isD3D12SwapChain, IsInWrapperPresent(),
        IsWrappedSwapChainObject(pSwapChain), slLoaded, g_FGCompat.GetRuntimeMode(),
        g_StreamlineFGRunning.load(std::memory_order_acquire), g_FGCompat.IsNvPresentLoaded());
}
}

namespace DXGIShared {
bool ShouldForceThirdPartyOverlayBypass(IDXGISwapChain* pSwapChain, bool bypassAvailable,
                                               const char** overlayModuleOut ) {
    const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
    if (overlayModuleOut) {
        *overlayModuleOut = overlayModule;
    }
    if (!pSwapChain || !bypassAvailable || !overlayModule) {
        return false;
    }

    if (!IsInWrapperPresent() && !IsWrappedSwapChainObject(pSwapChain)) {
        return false;
    }

    return true;
}
}

namespace DXGIShared {
DX12StartupPresentMode GetDX12StartupPresentMode(bool bypassAvailable, const char** overlayModuleOut ,
                                                        int* passIndexOut ) {
    const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
    if (overlayModuleOut) {
        *overlayModuleOut = overlayModule;
    }
    const bool steamBypassShouldOwnPath = ShouldForceSteamDX12BypassForState(
        bypassAvailable, IsSteamOverlayModule(overlayModule), true, false, false,
        ce::overlay_compat::IsStreamlineInterposerModuleLoaded(), g_FGCompat.GetRuntimeMode(),
        g_StreamlineFGRunning.load(std::memory_order_acquire), g_FGCompat.IsNvPresentLoaded());
    const bool bypassReady = EnsurePresentBypassTrampoline() != nullptr;
    if (!DXGIShared::ShouldAllowDX12StartupPresentPassForState(overlayModule != nullptr, dxgi_shared_oPresentTrampoline != nullptr,
                                                               dxgi_shared_oPresent1Trampoline != nullptr, steamBypassShouldOwnPath,
                                                               bypassReady, g_FGCompat.GetRuntimeMode(),
                                                               g_StreamlineFGRunning.load(std::memory_order_acquire))) {
        static std::atomic<int> s_startupPassBlockLogCount{0};
        const int blockNum = s_startupPassBlockLogCount.fetch_add(1, std::memory_order_relaxed);
        if (blockNum < 5) {
            HookLogImportant(
                "GetDX12StartupPresentMode: Startup compat pass blocked "
                "(overlay=%d trampoline=%d bypass=%d steamBypassOwn=%d runtimeMode=%d slFG=%d)",
                overlayModule != nullptr ? 1 : 0, dxgi_shared_oPresentTrampoline != nullptr ? 1 : 0, bypassReady ? 1 : 0,
                steamBypassShouldOwnPath ? 1 : 0, static_cast<int>(g_FGCompat.GetRuntimeMode()),
                g_StreamlineFGRunning.load(std::memory_order_acquire) ? 1 : 0);
        }
        return DX12StartupPresentMode::kNone;
    }

    static std::atomic<int> s_startupPassCount{0};
    const bool steamOverlay = IsSteamOverlayModule(overlayModule);
    const int startupCompatFrames = (steamOverlay && bypassAvailable) ? 16 : 3;
    int expected = s_startupPassCount.load(std::memory_order_acquire);
    while (expected < startupCompatFrames) {
        if (s_startupPassCount.compare_exchange_weak(expected, expected + 1, std::memory_order_acq_rel,
                                                     std::memory_order_acquire)) {
            if (passIndexOut) {
                *passIndexOut = expected + 1;
            }
            return DX12StartupPresentMode::kPassThroughOriginal;
        }
    }
    return DX12StartupPresentMode::kNone;
}
}

namespace DXGIShared {
bool TryReadSteamOverlayNullCallbackSlot(void** callbackValueOut) {
    if (!callbackValueOut) {
        return false;
    }

#ifdef _WIN64
    const wchar_t* steamModuleName = L"gameoverlayrenderer64.dll";
    const uintptr_t kSteamCallbackRva = 0x1621d8;
#else
    const wchar_t* steamModuleName = L"gameoverlayrenderer.dll";
    const uintptr_t kSteamCallbackRva = 0x1621d8;
#endif

    HMODULE steamMod = GetModuleHandleW(steamModuleName);
    if (!steamMod) {
        return false;
    }

    void** callbackSlot = reinterpret_cast<void**>(reinterpret_cast<uintptr_t>(steamMod) + kSteamCallbackRva);
    if (!IsReadableMemory(reinterpret_cast<const void*>(callbackSlot), sizeof(void*))) {
        return false;
    }

    *callbackValueOut = *callbackSlot;
    return true;
}
}

namespace DXGIShared {
bool TryGetSwapChainBackBufferIndex(IDXGISwapChain* pSwapChain, UINT* indexOut) {
    if (!pSwapChain || !indexOut) {
        return false;
    }

    IDXGISwapChain3* sc3 = nullptr;
    if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&sc3))) || !sc3) {
        return false;
    }

    *indexOut = sc3->GetCurrentBackBufferIndex();
    sc3->Release();
    return true;
}
}

namespace DXGIShared {
bool TryInvokeGuardedExternalSteamOverlayPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                                                        const char* reason, HRESULT* resultOut) {
    if (!pSwapChain || !resultOut) {
        return false;
    }

    PFN_Present externalPresent = dxgi_shared_g_externalOverlayPresentHook;
    PFN_Present presentBypass = EnsurePresentBypassTrampoline();
    const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
    const bool isSteamOverlay = IsSteamOverlayModule(overlayModule);
    const bool isD3D12SwapChain = DetectAPIType(pSwapChain) == APIType::D3D12;
    const bool streamlineStackActive = isD3D12SwapChain && HasStreamlineModuleInCurrentStack();
    const bool streamlinePluginLookupGuardReady = StreamlineHook::IsExternalOverlayPluginLookupGuardReady();
    const bool streamlineFGRunning = g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool postSLConfirmedRendering = isD3D12SwapChain && HookIsPostSLOverlayConfirmedRendering();
    const bool postSLConfirmedButStartupSettling = isD3D12SwapChain && HookIsPostSLOverlayConfirmedButStartupSettling();
    const bool startupTransitionWindowActive =
        isD3D12SwapChain && DXGIShared::IsStreamlineStartupTransitionWindowActive();
    const auto runtimeMode = g_FGCompat.GetRuntimeMode();
    const uint32_t currentThreadId = GetCurrentThreadId();
    const uint32_t trackedSourcePresentThreadId = isD3D12SwapChain ? DX12_GetGamePresentThreadId() : 0;
    const bool runtimeCanPresentFromWorker = DXGIShared::CanRuntimePresentFromWorkerForExternalOverlay(
        isD3D12SwapChain, streamlineStackActive, streamlineFGRunning, postSLConfirmedRendering,
        ce::fg_runtime::RuntimeModeUsesFSR(runtimeMode), g_FGCompat.IsFSRFGApiActive(),
        HookHasRuntimeOwnedNativeFGPresentPath(), DoesFGRuntimeOwnSwapchain());
    const bool synchronousPresentThreadAllowed =
        DXGIShared::ShouldInvokeSynchronousExternalOverlayPresentForThreadState(
            runtimeCanPresentFromWorker, trackedSourcePresentThreadId, currentThreadId);
    if (!synchronousPresentThreadAllowed) {
        if (externalPresent && presentBypass && isSteamOverlay && isD3D12SwapChain) {
            static std::atomic<int> s_guardedSteamRuntimeWorkerSkipLogCount{0};
            const int skipNum = s_guardedSteamRuntimeWorkerSkipLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (skipNum <= 20 || skipNum == 50 || (skipNum % 500) == 0) {
                HookLogImportant(
                    "DXGIShared: Skipping guarded Steam Present hook #%d for %s because an FG runtime can Present "
                    "from workers and this is not the verified source Present thread; using bypass trampoline "
                    "instead (runtime=%s slFG=%d confirmed=%d streamlineStack=%d sourceTid=0x%04X "
                    "currentTid=0x%04X)",
                    skipNum, reason ? reason : "Present", ce::fg_runtime::GetRuntimeModeName(runtimeMode),
                    streamlineFGRunning ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
                    streamlineStackActive ? 1 : 0, trackedSourcePresentThreadId, currentThreadId);
            }
        }
        return false;
    }

    void* steamCallbackBefore = nullptr;
    const bool steamCallbackReadable = TryReadSteamOverlayNullCallbackSlot(&steamCallbackBefore);
    const auto steamCallbackAddress = reinterpret_cast<uintptr_t>(steamCallbackBefore);
    const bool steamCallbackIsNull = steamCallbackReadable && steamCallbackBefore == nullptr;
    const bool steamCallbackIsCEDummy =
        steamCallbackReadable && steamCallbackBefore == reinterpret_cast<void*>(SteamDummyRenderingCallback);
    const bool steamCallbackIsInvalidLowAddress =
        steamCallbackReadable && steamCallbackBefore != nullptr && steamCallbackAddress < 0x10000;
    ScopedSteamNullCallbackRecoveryGuard steamNullCallbackGuard(
        externalPresent != nullptr && externalPresent != DetourPresent && presentBypass != nullptr && isSteamOverlay &&
            isD3D12SwapChain,
        "guarded external Present", reason, reinterpret_cast<void*>(externalPresent),
        reinterpret_cast<void*>(presentBypass), streamlineStackActive, streamlinePluginLookupGuardReady);
    const bool steamNullCallbackRecoveryReady = steamNullCallbackGuard.IsInstalled();
    const bool basePolicyAllowsGuardedSteamInvoke = DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForState(
        externalPresent != nullptr && externalPresent != DetourPresent, presentBypass != nullptr, isSteamOverlay,
        isD3D12SwapChain, IsInWrapperPresent(), IsWrappedSwapChainObject(pSwapChain),
        dxgi_shared_s_externalOverlayPresentInvokeDepth > 0, streamlineStackActive, synchronousPresentThreadAllowed,
        streamlinePluginLookupGuardReady, steamNullCallbackRecoveryReady);
    const bool callbackStateAllowsGuardedSteamInvoke =
        DXGIShared::ShouldInvokeGuardedExternalSteamOverlayPresentForCallbackState(
            basePolicyAllowsGuardedSteamInvoke, steamCallbackReadable, steamCallbackIsNull, steamCallbackIsCEDummy,
            steamCallbackIsInvalidLowAddress, steamNullCallbackRecoveryReady);
    if (!callbackStateAllowsGuardedSteamInvoke) {
        if (basePolicyAllowsGuardedSteamInvoke && externalPresent && presentBypass && isSteamOverlay &&
            isD3D12SwapChain) {
            static std::atomic<int> s_guardedSteamCallbackStateSkipLogCount{0};
            const int skipNum = s_guardedSteamCallbackStateSkipLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (skipNum <= 20 || skipNum == 50 || (skipNum % 500) == 0) {
                HookLogImportant(
                    "DXGIShared: Skipping guarded Steam Present hook #%d for %s because Steam callback state is not "
                    "a real renderer; using bypass trampoline instead (callbackReadable=%d null=%d ceDummy=%d "
                    "lowAddress=%d callback=%p steamNullGuard=%d streamlineStack=%d pluginGuard=%d tid=0x%04X)",
                    skipNum, reason ? reason : "Present", steamCallbackReadable ? 1 : 0, steamCallbackIsNull ? 1 : 0,
                    steamCallbackIsCEDummy ? 1 : 0, steamCallbackIsInvalidLowAddress ? 1 : 0, steamCallbackBefore,
                    steamNullCallbackRecoveryReady ? 1 : 0, streamlineStackActive ? 1 : 0,
                    streamlinePluginLookupGuardReady ? 1 : 0, GetCurrentThreadId());
            }
        }
        if (externalPresent && presentBypass && isSteamOverlay && isD3D12SwapChain && streamlineStackActive) {
            static std::atomic<int> s_guardedSteamStreamlineStackSkipLogCount{0};
            const int skipNum = s_guardedSteamStreamlineStackSkipLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (skipNum <= 20 || skipNum == 50 || (skipNum % 500) == 0) {
                HookLogImportant(
                    "DXGIShared: Skipping guarded Steam Present hook #%d for %s because current stack is inside "
                    "Streamline startup/FG routing and required guard state is not ready; using bypass trampoline "
                    "instead (slFG=%d confirmed=%d settling=%d startupWindow=%d pluginGuard=%d invokeDepth=%d "
                    "steamNullGuard=%d steamCallbackReadable=%d steamCallback=%p tid=0x%04X)",
                    skipNum, reason ? reason : "Present", streamlineFGRunning ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
                    postSLConfirmedButStartupSettling ? 1 : 0, startupTransitionWindowActive ? 1 : 0,
                    streamlinePluginLookupGuardReady ? 1 : 0, dxgi_shared_s_externalOverlayPresentInvokeDepth,
                    steamNullCallbackRecoveryReady ? 1 : 0, steamCallbackReadable ? 1 : 0, steamCallbackBefore,
                    currentThreadId);
            }
        }
        return false;
    }

    static std::atomic<int> s_guardedSteamInvokeLogCount{0};
    const int invokeNum = s_guardedSteamInvokeLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (invokeNum <= 20 || invokeNum == 50 || (invokeNum % 500) == 0) {
        HookLogImportant(
            "DXGIShared: Invoking guarded Steam Present hook #%d for %s "
            "(hook=%p bypass=%p slLoaded=%d streamlineFG=%d streamlineStack=%d pluginGuard=%d "
            "steamNullGuard=%d steamCallbackReadable=%d steamCallback=%p sourceTid=0x%04X tid=0x%04X)",
            invokeNum, reason ? reason : "Present", (void*)externalPresent, (void*)presentBypass,
            ce::overlay_compat::IsStreamlineInterposerModuleLoaded() ? 1 : 0, streamlineFGRunning ? 1 : 0,
            streamlineStackActive ? 1 : 0, streamlinePluginLookupGuardReady ? 1 : 0,
            steamNullCallbackRecoveryReady ? 1 : 0, steamCallbackReadable ? 1 : 0, steamCallbackBefore,
            trackedSourcePresentThreadId, currentThreadId);
    }

    ++dxgi_shared_s_externalOverlayPresentInvokeDepth;
    auto depthGuard = ce::make_scope_guard([]() {
        if (dxgi_shared_s_externalOverlayPresentInvokeDepth > 0) {
            --dxgi_shared_s_externalOverlayPresentInvokeDepth;
        }
    });
    StreamlineHook::ExternalOverlayPresentGuard slGuard;

    UINT bbIdxBefore = UINT_MAX;
    UINT bbIdxAfter = UINT_MAX;
    const bool bbIdxBeforeMeasured = TryGetSwapChainBackBufferIndex(pSwapChain, &bbIdxBefore);
    const HRESULT hr = externalPresent(pSwapChain, SyncInterval, Flags);
    const bool bbIdxAfterMeasured = TryGetSwapChainBackBufferIndex(pSwapChain, &bbIdxAfter);
    const bool bbIdxMeasured = bbIdxBeforeMeasured && bbIdxAfterMeasured;
    const bool bbIdxAdvanced = bbIdxMeasured && bbIdxAfter != bbIdxBefore;
    if (FAILED(hr)) {
        static std::atomic<int> s_guardedSteamFailureLogCount{0};
        const int failureNum = s_guardedSteamFailureLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (failureNum <= 10 || (failureNum % 100) == 0) {
            HookLogImportant("DXGIShared: Guarded Steam Present hook failed for %s hr=0x%08X (failure #%d)",
                             reason ? reason : "Present", (unsigned)hr, failureNum);
        }
    }

    if (DXGIShared::ShouldFallbackGuardedExternalSteamOverlayPresentForResult(presentBypass != nullptr, hr,
                                                                              bbIdxMeasured, bbIdxAdvanced)) {
        static std::atomic<int> s_guardedSteamBypassFallbackLogCount{0};
        const int fallbackNum = s_guardedSteamBypassFallbackLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (fallbackNum <= 20 || fallbackNum == 50 || (fallbackNum % 500) == 0) {
            HookLogImportant(
                "DXGIShared: Guarded Steam Present hook fallback #%d for %s via bypass=%p "
                "(hr=0x%08X bbMeasured=%d bbIdx=%u->%u steamNullGuard=%d tid=0x%04X)",
                fallbackNum, reason ? reason : "Present", (void*)presentBypass, (unsigned)hr, bbIdxMeasured ? 1 : 0,
                bbIdxBefore, bbIdxAfter, steamNullCallbackRecoveryReady ? 1 : 0, GetCurrentThreadId());
        }
        *resultOut = presentBypass(pSwapChain, SyncInterval, Flags);
        return true;
    }

    *resultOut = hr;
    return true;
}
}

namespace DXGIShared {
// Detect if SL has hooked the Present function with an E9 JMP or FF 25
// indirect JMP.  If so, set up routing so our final Present call goes
// through SL's hook chain instead of bypassing it via the trampoline.
void DetectSLPresentHook() {
    if (dxgi_shared_s_slRoutingActive.load(std::memory_order_acquire))
        return;
    if (!dxgi_shared_oPresent || !dxgi_shared_oPresentTrampoline) {
        // Vtable hook path (externally hooked Present): oPresentTrampoline is
        // NULL because we use vtable hooking instead of inline hooking when an
        // external E9 JMP (e.g. Steam overlay) is detected on dxgi!Present.
        // In this path, oPresent is the vtable's Present entry (whose dxgi.dll
        // bytes may be owned by Steam/SL), so SL routing detection via E9 JMP on
        // oPresent bytes does not apply here.  CE uses guarded Steam-overlay
        // invocations for the bypass-only paths and otherwise lets normal
        // vtable routing decide the live Present chain.
        // Log once so post-mortem analysis can distinguish the vtable path from
        // a missing inline hook bug.
        static std::atomic<uint32_t> s_vtablePathLogCount{0};
        if (s_vtablePathLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLogImportant(
                "DetectSLPresentHook: Skipping — vtable hook path (oPresent=%p, oPresentTrampoline=NULL). "
                "SL routing detection is not applicable on the external-overlay vtable path.",
                dxgi_shared_oPresent);
        }
        return;
    }

    // If oPresent is our own trampoline, SL hasn't hooked the vtable yet.
    // The vtable repair code sets oPresent to SL's hook when detected.
    if (dxgi_shared_oPresent == dxgi_shared_oPresentTrampoline)
        return;

    auto* funcBytes = (const uint8_t*)dxgi_shared_oPresent;
    if (!IsReadableMemory(funcBytes, 16))
        return;

    // Rate-limit diagnostic logging to avoid per-frame spam.
    static int s_checkCount = 0;
    int checkNum = ++s_checkCount;
    if (checkNum <= 5 || (checkNum <= 50 && (checkNum % 10) == 0) || (checkNum % 500) == 0) {
        HookLogImportant("DetectSLPresentHook: oPresent=%p bytes: %02X %02X %02X %02X %02X %02X (check #%d)", dxgi_shared_oPresent,
                         funcBytes[0], funcBytes[1], funcBytes[2], funcBytes[3], funcBytes[4], funcBytes[5], checkNum);
    }

    // Detect SL hooks: E9 relative JMP or FF 25 indirect JMP (JMP [RIP+0]).
    // SL may use either pattern depending on version and game.
    bool isE9 = (funcBytes[0] == 0xE9);
    bool isFF25 = (funcBytes[0] == 0xFF && funcBytes[1] == 0x25);

    if (!isE9 && !isFF25) {
        return;
    }

    void* hookTarget = isE9 ? ResolveE9JmpTarget((void*)dxgi_shared_oPresent) : ResolveFF25JmpTarget((void*)dxgi_shared_oPresent);
    char hookTargetModulePath[MAX_PATH] = {};
    HMODULE hookTargetModule = nullptr;
    const bool hookTargetResolved =
        hookTarget && TryGetModulePathFromCodeAddress(hookTarget, hookTargetModulePath, sizeof(hookTargetModulePath),
                                                      &hookTargetModule);
    const bool hookTargetFromStreamline = hookTargetResolved && IsStreamlineModuleHandle(hookTargetModule);
    const bool hookTargetFromCaptureHook = hookTargetResolved && IsCaptureHookModulePath(hookTargetModulePath);
    if (!DXGIShared::ShouldActivateStreamlinePresentRoutingForHookTarget(
            true, hookTargetResolved, hookTargetFromStreamline, hookTargetFromCaptureHook)) {
        static std::atomic<uint32_t> s_rejectedHookTargetLogCount{0};
        const uint32_t rejectedLogCount = s_rejectedHookTargetLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (rejectedLogCount <= 20 || (rejectedLogCount % 500) == 0) {
            HookLogImportant(
                "DetectSLPresentHook: %s JMP at oPresent=%p rejected as non-Streamline target "
                "(target=%p resolved=%d module=%s captureHook=%d streamline=%d log=%u)",
                isE9 ? "E9" : "FF25", dxgi_shared_oPresent, hookTarget, hookTargetResolved ? 1 : 0,
                hookTargetModulePath[0] ? hookTargetModulePath : "unknown", hookTargetFromCaptureHook ? 1 : 0,
                hookTargetFromStreamline ? 1 : 0, rejectedLogCount);
        }
        return;
    }

    // Verify that our trampoline is different (it should have the original
    // function bytes, not a JMP).
    auto* trampolineBytes = (const uint8_t*)dxgi_shared_oPresentTrampoline;
    static std::atomic<uint32_t> s_trampolineBytesLogCount{0};
    const uint32_t trampolineLogCount = s_trampolineBytesLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (trampolineLogCount <= 5 || (trampolineLogCount % 500) == 0) {
        HookLogImportant("DetectSLPresentHook: trampoline=%p bytes: %02X %02X %02X %02X %02X %02X (trampolineLog=%u)",
                         dxgi_shared_oPresentTrampoline, trampolineBytes[0], trampolineBytes[1], trampolineBytes[2],
                         trampolineBytes[3], trampolineBytes[4], trampolineBytes[5], trampolineLogCount);
    }

    ce::fg_runtime::RuntimeMode runtimeMode = ce::fg_runtime::RuntimeMode::kOff;
    bool runtimeOwnedNativeFGPresentPath = false;

    // Don't re-enable SL routing while the native/runtime-owned FSR path still
    // owns presentation. GTA showed that the old runtime=FSR_FG-only guard was
    // too narrow: during the explicit native-FSR OFF teardown window, the FFX
    // runtime can still own presentation even though ffxConfigure briefly
    // publishes Off. Re-attaching Streamline's Present hook chain in that window
    // reintroduces mixed-runtime routing on the native FSR path.
    if (ShouldKeepSLPresentRoutingDisabledNow(&runtimeMode, &runtimeOwnedNativeFGPresentPath)) {
        static int s_suppressedCount = 0;
        int suppressedNum = ++s_suppressedCount;
        if (suppressedNum <= 5 || (suppressedNum % 500) == 0) {
            HookLogImportant(
                "DetectSLPresentHook: SL %s JMP detected at oPresent=%p but SL routing NOT re-enabled "
                "(native FG path owns Present routing, suppressed #%d, runtime=%s "
                "runtimeOwnedNativeFG=%d)",
                isE9 ? "E9" : "FF25", dxgi_shared_oPresent, suppressedNum, ce::fg_runtime::GetRuntimeModeName(runtimeMode),
                runtimeOwnedNativeFGPresentPath ? 1 : 0);
        }
        return;
    }

    dxgi_shared_s_slRoutingActive.store(true, std::memory_order_release);
    HookLogImportant(
        "SL routing ACTIVE: Present calls will go through oPresent=%p "
        "(%s JMP target=%p module=%s) instead of trampoline=%p.  SL FG chain will execute.",
        dxgi_shared_oPresent, isE9 ? "E9" : "FF25", hookTarget, hookTargetModulePath[0] ? hookTargetModulePath : "unknown",
        dxgi_shared_oPresentTrampoline);
}
}

namespace DXGIShared {
void UpdateDXGIPresentMetricsAndPublish(bool isFirstHook, const char* publicationSource) {
    if (!isFirstHook) {
        return;
    }

    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kPresentObserved, publicationSource);

    static int64_t qpcFreq = 0;
    if (qpcFreq == 0) {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        qpcFreq = f.QuadPart;
    }

    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    const int64_t us = (qpc.QuadPart * 1000000) / qpcFreq;
    dxgi_shared_g_DXGIPerfMetrics.Update(us);
    const ce::fg_session::FGActionPlan plan = ce::fg_session::GetLatestFGActionPlan();
    ce::overlay_metrics::PublishOverlayFGMetrics(&dxgi_shared_g_DXGIPerfMetrics, plan, g_FGCompat.GetOutputFPS(),
                                                 g_FGCompat.GetBaseFPS(), g_FGCompat.GetFGMultiplier(),
                                                 publicationSource);
}
}

namespace DXGIShared {
void RefreshLivePresentHooksForSwapchainIfNeeded(IDXGISwapChain* pSwapChain, const char* source) {
    if (!pSwapChain || !IsReadableMemory(pSwapChain, sizeof(void*))) {
        return;
    }

    void** vtable = *(void***)pSwapChain;
    const bool hasReadableVtable = vtable && IsReadableMemory(reinterpret_cast<const void*>(vtable), 23 * sizeof(void*));
    const bool trackedVtableMatchesCurrent = hasReadableVtable && dxgi_shared_s_hookedVTable == vtable;
    const bool presentHookInstalled = hasReadableVtable && vtable[8] == (void*)DetourPresent;
    const bool present1HookInstalled = hasReadableVtable && vtable[22] == (void*)DetourPresent1;

    if (!DXGIShared::ShouldRefreshLivePresentHooksForSwapchainPath(hasReadableVtable, trackedVtableMatchesCurrent,
                                                                   presentHookInstalled, present1HookInstalled)) {
        return;
    }

    HookLogImportant(
        "DXGIShared: Refreshing live Present hook path via %s swapchain %p (oldVtable=%p newVtable=%p hooked8=%d "
        "hooked22=%d)",
        source ? source : "runtime", pSwapChain, dxgi_shared_s_hookedVTable, vtable, presentHookInstalled ? 1 : 0,
        present1HookInstalled ? 1 : 0);

    InstallHooks(pSwapChain, true);
    RepairVTableHooksIfNeeded();

    if (IsSLInterposerLoaded() && !ce::fg_runtime::RuntimeModeUsesFSR(g_FGCompat.GetRuntimeMode())) {
        DetectSLPresentHook();
    }
}
}

namespace DXGIShared {
// RTSS-style: draw the overlay present-time before a Streamline-startup bypass present so the
// toggle-on / DLSS-G-init frozen frame carries the overlay. Opt-in + gated; HandleDX12ProcessFrame
// resolves the submit queue and does the same-queue safety check internally (see the pre-SL un-gate
// in dx12_hook.cpp). Gated so steady-state FG and the round-1..3 wins are untouched: D3D12 only,
// DLSS FG turning on, PostSL not yet confirmed (once PostSL owns the overlay this bypass path is not
// taken), and pure DLSS (no FSR history).
void MaybeEagerDrawOverlayBeforeStreamlineStartupBypass(IDXGISwapChain* pSwapChain, bool isD3D12,
                                                               bool streamlineFGRunning, bool postSLConfirmedRendering,
                                                               bool hadFSRFGPhase, const char* site) {
    if (!IsDlssToggleEagerOverlayEnabled() || !isD3D12 || !streamlineFGRunning || postSLConfirmedRendering ||
        hadFSRFGPhase)
        return;
    static std::atomic<int> s_log{0};
    const int n = s_log.fetch_add(1, std::memory_order_relaxed);
    if (n < 10 || (n % 120) == 0)
        HookLogImportant(
            "DX12: Eager present-time overlay draw before Streamline-startup bypass (RTSS-style, site=%s sc=%p)", site,
            (void*)pSwapChain);
    HandleDX12ProcessFrame(pSwapChain, false, true);
}
}

namespace DXGIShared {
// SAFETY NET: Attempt one-time Steam DX12 overlay initialization.
//
// The PRIMARY fix (InstallPresentInlineHooks) pre-initializes Steam overlay on
// the temp swapchain BEFORE our vtable hook is installed.  This function is a
// fallback for cases where pre-init didn't occur:
//   - Steam overlay loaded AFTER hook installation
//   - Another thread/process context
//
// It temporarily restores vtable[8] to the real dxgi!Present, calls through
// Steam's E9 JMP, then re-hooks vtable[8] to DetourPresent. If Steam still
// reaches a lazy NULL callback on the real swapchain, the scoped VEH guard
// patches the exact faulting slot to CE's DXGI bypass Present and retries.
//
// Thread safety: only one thread wins the compare-exchange.  The brief window
// where vtable[8] is unhooked is microseconds wide and limited to frame 1.
//
// Returns true if this thread performed the init call (result in *resultOut).
// Returns false if another thread won the init race or if init was skipped.
bool AttemptSteamDX12OverlayInit(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags,
                                        PFN_Present presentOriginal, PFN_Present presentBypass, HRESULT* resultOut) {
    if (!pSwapChain || !resultOut || !dxgi_shared_s_hookedVTable || !presentOriginal || dxgi_shared_s_steamInitCrashed) {
        return false;
    }

    if (!IsReadableMemory(reinterpret_cast<const void*>(dxgi_shared_s_hookedVTable), 9 * sizeof(void*))) {
        return false;
    }

    // Only one thread wins the init race
    bool expected = false;
    if (!dxgi_shared_s_steamDX12InitAttempted.compare_exchange_strong(expected, true, std::memory_order_acq_rel,
                                                          std::memory_order_acquire)) {
        return false;  // Another thread is already handling init
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        HookLogImportant(
            "AttemptSteamDX12OverlayInit: VirtualProtect failed to unhook vtable[8] — will retry on next frame");
        dxgi_shared_s_steamDX12InitAttempted.store(false, std::memory_order_release);
        return false;
    }

    // Save current vtable[8] (= DetourPresent) and restore to the real dxgi!Present
    void* savedVtable8 = dxgi_shared_s_hookedVTable[8];
    dxgi_shared_s_hookedVTable[8] = (void*)presentOriginal;
    VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), oldProtect, &oldProtect);

    HookLogImportant(
        "AttemptSteamDX12OverlayInit: vtable[8] temporarily restored to dxgi!Present=%p — "
        "calling through E9 JMP for Steam overlay init (with VEH protection) "
        "[s_originalVtable8Present=%p, same=%d]",
        (void*)presentOriginal, (void*)dxgi_shared_s_originalVtable8Present, dxgi_shared_s_originalVtable8Present == presentOriginal ? 1 : 0);

    // Call through oPresent (E9 JMP at dxgi!Present) WITH VEH protection.
    //
    // Steam's OverlayHookD3D3 can still have lazy NULL callback slots on first
    // entry through the E9 JMP on a REAL game swapchain (the temp swapchain pre-
    // init in InstallPresentInlineHooks doesn't trigger full initialization
    // because Steam skips rendering on a 2x2 hidden-window swapchain).
    //
    // The SteamOverlayInitVehHandler catches this specific crash (RIP=0, RAX=0,
    // return address inside gameoverlayrenderer64.dll), patches the exact NULL
    // slot to CE's bypass Present when possible, and retries the `call rax` so
    // Steam completes its initialization and real Present chaining survives.
    //
    // If the crash is NOT the expected NULL callback (e.g. a different Steam bug),
    // the handler returns EXCEPTION_CONTINUE_SEARCH and CE's existing VEH crash
    // handler catches it and writes a crash dump.
    ScopedSteamNullCallbackRecoveryGuard steamInitGuard(true, "non-SL Steam init", "AttemptSteamDX12OverlayInit",
                                                        reinterpret_cast<void*>(presentOriginal),
                                                        reinterpret_cast<void*>(presentBypass), false, false);
    HRESULT initHr = presentOriginal(pSwapChain, SyncInterval, Flags);

    // Re-hook vtable[8] with DetourPresent (our vtable hook)
    if (VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        dxgi_shared_s_hookedVTable[8] = (void*)DetourPresent;
        VirtualProtect(reinterpret_cast<void*>(&dxgi_shared_s_hookedVTable[8]), sizeof(void*), oldProtect, &oldProtect);
    } else {
        // CRITICAL: VirtualProtect for re-hook failed — vtable[8] is exposed.
        // Our DetourPresent hook may be lost. Log prominently and continue.
        HookLogImportant(
            "AttemptSteamDX12OverlayInit: CRITICAL — VirtualProtect failed to re-hook vtable[8]! "
            "CE overlay may be disabled for this session.");
    }

    // Check what Steam's legacy known callback slot contains after the init call.
    // New Steam builds can use nearby slots too; the VEH log reports the exact
    // dynamically resolved slot when it differs from this legacy address.
    {
        HMODULE steamMod = GetModuleHandleW(L"gameoverlayrenderer64.dll");
        if (steamMod) {
            void** steamCallbackPtr = (void**)((uintptr_t)steamMod + 0x1621d8);
            if (IsReadableMemory(reinterpret_cast<const void*>(steamCallbackPtr), sizeof(void*))) {
                void* callbackAfterInit = *steamCallbackPtr;
                if (callbackAfterInit != nullptr && callbackAfterInit != (void*)SteamDummyRenderingCallback &&
                    callbackAfterInit != (void*)presentBypass) {
                    HookLogImportant(
                        "AttemptSteamDX12OverlayInit: Steam legacy callback slot contains Steam-owned function %p "
                        "(bypass=%p dummy=%p)",
                        callbackAfterInit, (void*)presentBypass, (void*)SteamDummyRenderingCallback);
                } else {
                    HookLogImportant(
                        "AttemptSteamDX12OverlayInit: Steam legacy callback slot is %s (%p) "
                        "(bypass=%p dummy=%p)",
                        callbackAfterInit == nullptr
                            ? "NULL"
                            : (callbackAfterInit == (void*)presentBypass ? "CE bypass" : "CE dummy"),
                        callbackAfterInit, (void*)presentBypass, (void*)SteamDummyRenderingCallback);
                }
            } else {
                HookLog("AttemptSteamDX12OverlayInit: Cannot read Steam callback pointer (not readable)");
            }
        } else {
            HookLog("AttemptSteamDX12OverlayInit: gameoverlayrenderer64.dll not loaded");
        }
    }

    HookLogImportant(
        "AttemptSteamDX12OverlayInit: Steam overlay init completed (hr=0x%08X) — "
        "vtable[8] re-hooked to DetourPresent.  Subsequent frames will invoke Steam "
        "overlay via g_externalOverlayPresentHook (explicit hook target, bypass trampoline fallback).",
        (unsigned)initHr);

    *resultOut = initHr;
    return true;
}
}
