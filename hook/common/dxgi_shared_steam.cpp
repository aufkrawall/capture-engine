#include "dxgi_shared_internal.h"

#include "dxgi_shared_detail/steam_null_callback.h"

#include <cstring>

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
// RTSS and Steam both hook dxgi!Present through runtime-allocated thunks that are
// not backed by any module (GetModuleHandleExA(FROM_ADDRESS) fails for them, so the
// install-time owner lookup reports "unknown"). Both thunk layouts share the x64
// indirect form `FF 25 00 00 00 00` followed by an absolute pointer to the real
// handler inside the overlay DLL. Resolving that pointer identifies the chain
// owner. Falls back to returning the hook address itself when it is not a
// resolvable thunk.
const void* ResolveExternalPresentHookThunkTarget(const void* externalHook) {
    if (!externalHook || !IsReadableMemory(externalHook, 16)) {
        return externalHook;
    }
    const auto* code = static_cast<const uint8_t*>(externalHook);
    if (code[0] != 0xFF || code[1] != 0x25) {
        return externalHook;
    }
    const void* target = nullptr;
    void* targetStorage = static_cast<void*>(&target);
    if (code[2] == 0 && code[3] == 0 && code[4] == 0 && code[5] == 0) {
        // FF 25 00 00 00 00 : JMP qword ptr [rip+0] -> pointer at +6.
        memcpy(targetStorage, code + 6, sizeof(target));
    } else {
        // FF 25 disp32 : JMP qword ptr [rip+disp32].
        int32_t disp = 0;
        memcpy(&disp, code + 2, sizeof(disp));
        const uintptr_t nextRip = reinterpret_cast<uintptr_t>(externalHook) + 6;
        const uintptr_t pointerAddress = nextRip + static_cast<intptr_t>(disp);
        if (!IsReadableMemory(reinterpret_cast<const void*>(pointerAddress), sizeof(target))) {
            return externalHook;
        }
        memcpy(targetStorage, reinterpret_cast<const void*>(pointerAddress), sizeof(target));
    }
    if (!target || !IsReadableMemory(target, 1)) {
        return externalHook;
    }
    return target;
}
}

namespace DXGIShared {
bool ResolveExternalPresentHookOwnerPath(const void* externalHook, char* modulePathOut, size_t modulePathOutCount) {
    if (!modulePathOut || modulePathOutCount == 0) {
        return false;
    }
    modulePathOut[0] = '\0';
    if (!externalHook) {
        return false;
    }
    const void* handler = ResolveExternalPresentHookThunkTarget(externalHook);
    return TryGetModulePathFromCodeAddress(handler, modulePathOut, modulePathOutCount);
}
}

namespace DXGIShared {
// Authoritative "is the current foreign Present chain Steam's" decision.
//
// The loaded-overlay module name alone is NOT sufficient: the module table reports
// the first loaded tracked overlay by list priority, so when Steam AND RTSS are
// both loaded it reports gameoverlayrenderer64.dll even though RTSS loaded later,
// displaced Steam's entry jump, and therefore owns the E9/FF25 chain that CE
// preserved (Strange Brigade + Steam + RTSS, session 20260811_233748 — RTSS's OSD
// stopped rendering while CE serviced RTSS's thunk as a "Steam" chain). The chain
// owner is classified from the resolved thunk target when possible; for
// unresolvable thunks the last real load notification decides (the later overlay
// displaced the earlier one).
bool IsExternalPresentHookSteamChain(const void* externalHook) {
    if (!externalHook) {
        return false;
    }
    char ownerPath[MAX_PATH] = {};
    if (ResolveExternalPresentHookOwnerPath(externalHook, ownerPath, sizeof(ownerPath))) {
        return IsSteamOverlayModule(ownerPath);
    }
    // Unresolvable thunk (no module backs the handler address): use load-order
    // evidence. RTSS as the most recently loaded overlay owns the chain.
    const char* lastLoaded = ce::overlay_compat::GetLastLoadedTrackedOverlayModuleName();
    return ce::overlay_compat::IsSteamExternalChainOwnerByLoadOrderEvidence(
        lastLoaded, ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName());
}
}

namespace DXGIShared {
// Present-hot-path wrapper: classify the currently saved external Present hook.
// When no foreign hook was saved, fall back to the loaded-module name so Steam
// overlays that hook only via the swapchain vtable keep their existing treatment.
bool IsCurrentExternalPresentHookSteamChain() {
    const void* externalHook = reinterpret_cast<const void*>(dxgi_shared_g_externalOverlayPresentHook);
    if (!externalHook) {
        return IsSteamOverlayModule(ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName());
    }
    return IsExternalPresentHookSteamChain(externalHook);
}
}

namespace DXGIShared {
namespace {
// NVIDIA Streamline can load its runtime DLLs under obfuscated hashed names
// (e.g. "1B0_E658703.dll") that contain none of the sl.* path tokens. Those
// modules still export the Streamline plugin API, so recognition falls back to
// the exports. Results are cached per module handle because this runs on the
// Present classification path (CapturePresentCallContext).
struct StreamlineModuleCacheEntry {
    HMODULE module = nullptr;
    bool isStreamline = false;
};
constexpr size_t kStreamlineModuleCacheSize = 16;
StreamlineModuleCacheEntry g_streamlineModuleCache[kStreamlineModuleCacheSize];
std::mutex g_streamlineModuleCacheMutex;

bool ResolveIsStreamlineModuleHandle(HMODULE moduleHandle) {
    char modulePath[MAX_PATH] = {};
    if (GetModuleFileNameA(moduleHandle, modulePath, MAX_PATH) == 0) {
        return false;
    }

    if (ce::overlay_compat::detail::ContainsInsensitive(modulePath, "sl.interposer") ||
        ce::overlay_compat::detail::ContainsInsensitive(modulePath, "sl.common") ||
        ce::overlay_compat::detail::ContainsInsensitive(modulePath, "sl.dlss_g")) {
        return true;
    }

    // Obfuscated Streamline runtime (RoboCop: Rogue City loads its runtime as
    // "1B0_E658703.dll"): recognized by its plugin API export.
    return GetProcAddress(moduleHandle, "slGetPluginFunction") != nullptr ||
           GetProcAddress(moduleHandle, "slGetFeatureFunction") != nullptr;
}
} // namespace

bool IsStreamlineModuleHandle(HMODULE moduleHandle) {
    if (!moduleHandle) {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(g_streamlineModuleCacheMutex);
        for (const auto& entry : g_streamlineModuleCache) {
            if (entry.module == moduleHandle) {
                return entry.isStreamline;
            }
        }
    }

    const bool resolved = ResolveIsStreamlineModuleHandle(moduleHandle);
    {
        std::lock_guard<std::mutex> lock(g_streamlineModuleCacheMutex);
        for (auto& entry : g_streamlineModuleCache) {
            if (entry.module == nullptr) {
                entry.module = moduleHandle;
                entry.isStreamline = resolved;
                break;
            }
        }
    }
    return resolved;
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
    // The forced-bypass route exists to keep Steam's DX12 chain from faulting
    // through lazy NULL callbacks. It must apply only when Steam actually owns the
    // preserved external chain: when RTSS displaced Steam's entry jump (Steam and
    // RTSS both loaded), the chain is RTSS's and forcing the bypass would skip
    // RTSS's Present handler entirely, killing its overlay.
    if (!pSwapChain || !bypassAvailable || !IsCurrentExternalPresentHookSteamChain()) {
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
        bypassAvailable, IsCurrentExternalPresentHookSteamChain(), true, false, false,
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
    const bool steamOverlay = IsCurrentExternalPresentHookSteamChain();
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
#else
    const wchar_t* steamModuleName = L"gameoverlayrenderer.dll";
#endif

    HMODULE steamMod = GetModuleHandleW(steamModuleName);
    if (!steamMod) {
        return false;
    }

    // Readiness, not "the first slot CE happened to find". The discovery scan returns 137
    // Present-shaped `mov (e)ax,[slot] ... call (e)ax` candidates in a current Steam build,
    // most of which Steam never initializes; reporting the first readable one made an
    // arbitrary always-NULL slot look like "Steam has no renderer" and suppressed the Steam
    // invoke on every frame (Talos 20260812_031449 - Steam AND RTSS both went invisible
    // because the decline falls back to the DXGI bypass, which skips the whole chain).
    // Steam is initialized when ANY of those slots holds a real code pointer.
    uintptr_t slots[detail::kSteamNullCallbackMaxSlots] = {};
    const size_t slotCount = DiscoverSteamNullCallbackSlots(steamMod, slots, detail::kSteamNullCallbackMaxSlots);
    bool anyReadable = false;
    void* firstReadableValue = nullptr;
    for (size_t i = 0; i < slotCount; ++i) {
        auto* callbackSlot = reinterpret_cast<void**>(slots[i]);
        if (!IsReadableMemory(reinterpret_cast<const void*>(callbackSlot), sizeof(void*))) {
            continue;
        }
        void* value = *callbackSlot;
        if (!anyReadable) {
            anyReadable = true;
            firstReadableValue = value;
        }
        if (value && IsExecutableCodeAddress(value)) {
            *callbackValueOut = value;
            return true;
        }
    }
    if (anyReadable) {
        *callbackValueOut = firstReadableValue;
        return true;
    }
    return false;
}
}

namespace DXGIShared {
size_t DiscoverSteamNullCallbackSlots(HMODULE steamModule, uintptr_t* slotsOut, size_t maxSlots) {
    if (!steamModule || !slotsOut || maxSlots == 0) {
        return 0;
    }

    MODULEINFO modInfo = {};
    if (!GetModuleInformation(GetCurrentProcess(), steamModule, &modInfo, sizeof(modInfo))) {
        return 0;
    }
    const auto* code = static_cast<const uint8_t*>(modInfo.lpBaseOfDll);
    const uintptr_t moduleStart = reinterpret_cast<uintptr_t>(modInfo.lpBaseOfDll);
    const uintptr_t moduleEnd = moduleStart + modInfo.SizeOfImage;
    if (!IsReadableMemory(code, modInfo.SizeOfImage)) {
        return 0;
    }
#ifdef _WIN64
    constexpr bool kX64Pattern = true;
#else
    constexpr bool kX64Pattern = false;
#endif
    return detail::FindSteamNullCallbackSlotCandidates(code, modInfo.SizeOfImage, moduleStart, moduleEnd,
                                                       slotsOut, maxSlots, kX64Pattern);
}
}

namespace DXGIShared {
// CE's inline-hook trampoline re-issues the foreign entry jump when CE
// prepended over an external overlay's E9/FF25 (the trampoline does not hold
// original code bytes in that case). Steam's handler can still fault through
// lazy NULL rendering callbacks on a fresh swapchain (DLSS->FSR switch,
// 20260811_195131), so Steam transports that chain through such a trampoline
// must run under the NULL-callback VEH recovery, never bare.
bool TrampolineChainsToExternalOverlay(void* trampoline, void* externalHook) {
    if (!trampoline) {
        return false;
    }
    const auto* code = static_cast<const uint8_t*>(trampoline);
    if (!IsReadableMemory(code, 16)) {
        return false;
    }
    void* resolved = nullptr;
    if (code[0] == 0xE9) {
        resolved = ResolveE9JmpTarget(trampoline);
    } else if (code[0] == 0xFF && code[1] == 0x25) {
        resolved = ResolveFF25JmpTarget(trampoline);
    }
    if (!resolved) {
        return false;
    }
    if (externalHook) {
        return resolved == externalHook;
    }
    // No preserved target (e.g. Present1): any chain target outside dxgi.dll
    // is a foreign overlay entry, matching the install-time detection rule.
    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    if (!hDXGI) {
        return false;
    }
    MODULEINFO dxgiInfo = {};
    if (!GetModuleInformation(GetCurrentProcess(), hDXGI, &dxgiInfo, sizeof(dxgiInfo))) {
        return false;
    }
    const uintptr_t dxgiStart = reinterpret_cast<uintptr_t>(hDXGI);
    const uintptr_t jumpTarget = reinterpret_cast<uintptr_t>(resolved);
    return jumpTarget < dxgiStart || jumpTarget >= dxgiStart + dxgiInfo.SizeOfImage;
}
}

namespace DXGIShared {
bool IsSteamExternalChainTrampoline(void* trampoline, void* externalHook, bool isD3D12SwapChain) {
    if (!isD3D12SwapChain || !trampoline) {
        return false;
    }
    if (!TrampolineChainsToExternalOverlay(trampoline, externalHook)) {
        return false;
    }
    if (externalHook) {
        // Classify by the chain owner, not the priority-ordered loaded-module
        // name: with Steam AND RTSS both loaded the name cache reports Steam
        // even though RTSS (loaded later) owns the preserved entry jump.
        return IsExternalPresentHookSteamChain(externalHook);
    }
    // No saved external hook (e.g. Present1, whose trampoline chains outside
    // dxgi.dll): classify from loaded-overlay evidence. A later-loaded RTSS owns
    // the chains on both Present entry points, so exclude it explicitly.
    const char* lastLoaded = ce::overlay_compat::GetLastLoadedTrackedOverlayModuleName();
    return ce::overlay_compat::IsSteamExternalChainOwnerByLoadOrderEvidence(
        lastLoaded, ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName());
}
}

namespace DXGIShared {
// REMOVED (2026-08-12): the pre-emptive wholesale patch of Steam's NULL
// Present-shaped callback slots.
//
// It scanned `gameoverlayrenderer64.dll` for `mov (e)ax,[slot] ... call (e)ax`
// sites and wrote CE's DXGI bypass into every slot that was still NULL (20 of
// them in Strange Brigade and Talos). Those slots are Steam's own hook-install
// outputs: `gameoverlayrenderer64+0x8da00` takes `&slot` and each call site
// tests `cmpq $0, slot` right after, i.e. Steam initializes them lazily and
// treats non-NULL as "already installed". Pre-filling them therefore
//   (a) makes Steam skip its own initialization, and
//   (b) turns Steam's "call original" into a raw dxgi!Present copy that skips
//       every hook chained BELOW Steam.
// In Talos + DLSS FG + RTSS (session 20260812_022607) frame 1 ran
// game -> sl.dlss_g -> CE -> Steam -> RTSS with all three overlays drawing;
// from frame 2 every guarded invoke reported `steamCallback=<CE's bypass>` -
// the exact value CE had written - and RTSS never submitted again.
//
// The NULL dispatch it was meant to prevent is handled by the two mechanisms
// that were already there and are sound: the callback-state gate refuses the
// Steam invoke unless the recovery is armed, and
// `SteamOverlayInitVehHandler` resolves the EXACT faulting slot from the fault
// context and recovers only that one. CE must never speculatively write into
// another tool's internal state.
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

    // Never transfer control to a saved foreign handler without proving it is still callable:
    // the overlays that share this entry rebuild their thunks, and a frozen pointer into a
    // freed one executes heap data (Talos 20260812_024730, DEP violation at 0x295C8999101).
    PFN_Present externalPresent = GetCallableExternalOverlayPresentHook();
    PFN_Present presentBypass = EnsurePresentBypassTrampoline();
    const bool isSteamOverlay = IsCurrentExternalPresentHookSteamChain();
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
    const bool isTrackedGameThread =
        trackedSourcePresentThreadId != 0 && trackedSourcePresentThreadId == currentThreadId;
    // During DLSS FG the frames actually displayed are the worker's generated
    // presents; Steam's GUI drawn only on the game thread's source-frame
    // presents is overwritten by the runtime and stays invisible. Steady-state
    // DLSS FG may therefore service Steam from the worker (the 015416 stall was
    // confined to the startup transition window, which stays strict).
    const bool streamlineWorkerSteamAllowed =
        !isTrackedGameThread &&
        DXGIShared::ShouldInvokeSteamOnStreamlineWorkerPresent(
            streamlineFGRunning, postSLConfirmedRendering, startupTransitionWindowActive,
            postSLConfirmedButStartupSettling, HookHasRuntimeOwnedNativeFGPresentPath(),
            ce::fg_runtime::RuntimeModeUsesFSR(runtimeMode), g_FGCompat.IsFSRFGApiActive());
    const bool synchronousPresentThreadAllowed =
        isTrackedGameThread || streamlineWorkerSteamAllowed ||
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
                    "instead (runtime=%s slFG=%d confirmed=%d settling=%d startupWindow=%d streamlineStack=%d "
                    "workerSteam=%d sourceTid=0x%04X "
                    "currentTid=0x%04X)",
                    skipNum, reason ? reason : "Present", ce::fg_runtime::GetRuntimeModeName(runtimeMode),
                    streamlineFGRunning ? 1 : 0, postSLConfirmedRendering ? 1 : 0,
                    postSLConfirmedButStartupSettling ? 1 : 0, startupTransitionWindowActive ? 1 : 0,
                    streamlineStackActive ? 1 : 0, streamlineWorkerSteamAllowed ? 1 : 0, trackedSourcePresentThreadId,
                    currentThreadId);
            }
        }
        return false;
    }

    // CE does not write into Steam's callback slots. A NULL dispatch is handled
    // by the callback-state gate below (which refuses the invoke unless the
    // recovery is armed) and, if it still faults, by SteamOverlayInitVehHandler
    // resolving the exact faulting slot. Pre-filling slots Steam has not
    // initialized makes it skip its own install and chain to a raw Present,
    // which silently drops every overlay below Steam (Talos 20260812_022607).
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

    // Own a reference across the foreign call. A foreign overlay chain can release the
    // swapchain during Present - Streamline recreates its runtime-owned swapchain on an FG
    // transition - and the post-invoke back-buffer measurement then dereferences freed
    // memory. Talos 20260812_032301 crashed exactly there, four milliseconds after
    // `foreign re-hook took the Present entry from CE`:
    // TryGetSwapChainBackBufferIndex+0x11 `mov rax,[rax]`, AV READ, RAX holding code bytes.
    // CE's swapchain wrapper already keeps this reference across its own Present; the
    // guarded foreign transport must do the same.
    pSwapChain->AddRef();
    auto swapChainLifetimeGuard = ce::make_scope_guard([pSwapChain]() { pSwapChain->Release(); });

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
