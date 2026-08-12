#pragma once

namespace DXGIShared {
struct SteamNullCallbackRecoveryContext;
}

namespace DXGIShared {
class ScopedSteamNullCallbackRecoveryGuard;
}

namespace DXGIShared {
struct PresentCallContext;
}

#include "dxgi_shared.h"

#include "dxgi_shared_detail/steam_null_callback.h"

#include "../../common/raii_helpers.h"

#include "../apis/dx11_hook.h"

#include "../apis/dx12_hook.h"

#include "../apis/streamline_hook.h"

#include "../wrappers/inline_hook.h"

#include "../wrappers/vtable_hook.h"

#include "../wrappers/wrapper_base.h"

#include "config.h"

#include "dx12_overlay_policy.h"

#include "fg_detection.h"

#include "fg_session_state.h"

#include "fps_limiter.h"

#include "freeze_watchdog.h"

#include "hook_common.h"

#include "logging.h"

#include "overlay_compat.h"

#include "overlay_metrics_publisher.h"

#include "perf_logger.h"

#include "performance_metrics.h"

#include <d3d10.h>

#include <d3d10_1.h>

#include <d3d11.h>

#include <d3d12.h>

#include <dxgi1_4.h>

#include <intrin.h>

#include <psapi.h>

#include <windows.h>

#include <atomic>

#include <chrono>

#include <cmath>

#include <cstdint>

#include <mutex>

#include <unordered_set>

// Original function pointers
typedef HRESULT(STDMETHODCALLTYPE* PFN_Present)(IDXGISwapChain*, UINT, UINT);

typedef HRESULT(STDMETHODCALLTYPE* PFN_Present1)(IDXGISwapChain*, UINT, UINT, const DXGI_PRESENT_PARAMETERS*);

typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);

typedef HRESULT(STDMETHODCALLTYPE* PFN_ResizeBuffers1)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                                       const UINT*, IUnknown* const*);

typedef HRESULT(STDMETHODCALLTYPE* PFN_SetColorSpace1)(IDXGISwapChain*, DXGI_COLOR_SPACE_TYPE);

namespace DXGIShared {
enum class DX12StartupPresentMode {
    kNone,
    kPassThroughOriginal,
};
}

#if defined(__clang__) || defined(__GNUC__)
#define CE_CAPTURE_RETURN_ADDRESS() __builtin_return_address(0)
#elif defined(_MSC_VER)
#define CE_CAPTURE_RETURN_ADDRESS() _ReturnAddress()
#else
#define CE_CAPTURE_RETURN_ADDRESS() nullptr
#endif

bool IsShuttingDown();

extern bool IsInWrapperPresent();

void NoteDX12PresentResultForVtablePath(IDXGISwapChain* swapChain, const char* presentName, UINT syncInterval, UINT presentFlags, HRESULT presentHr);

void InvokeDX12WaitForOverlayCompletion(ID3D12CommandQueue* pQueue);

void InvokeDX12FlushDeferredSignal();

namespace DXGIShared {
bool QuerySwapChainColorSpace(IDXGISwapChain* swapChain, DXGI_COLOR_SPACE_TYPE& colorSpace);
}

namespace DXGIShared {
bool RecordSwapChainColorSpace(IDXGISwapChain* swapChain, DXGI_COLOR_SPACE_TYPE colorSpace, bool* changed);
}

namespace DXGIShared {
ce::presentation_color::Encoding ResolveSwapChainPresentationEncoding(IDXGISwapChain* swapChain, DXGI_FORMAT format, DXGI_COLOR_SPACE_TYPE* trackedColorSpace, bool* hasTrackedColorSpace);
}

namespace DXGIShared {
void DX12_RegisterThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain, const char* creatorModulePath);
}

namespace DXGIShared {
void DX12_UnregisterThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
bool DX12_IsThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
bool DX12_IsStartupBlockingOverlayTaggedSwapchain(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
void BeginPostSLOffKeepAlivePresentScope();
}

namespace DXGIShared {
void EndPostSLOffKeepAlivePresentScope();
}

namespace DXGIShared {
void MarkPostSLOffKeepAlivePrePresentDrawn();
}

namespace DXGIShared {
bool WasPostSLOffKeepAlivePrePresentDrawn();
}

namespace DXGIShared {
bool IsRecursivePresent();
}

namespace DXGIShared {
void ReleasePresent();
}

namespace DXGIShared {
bool IsRecursiveResize();
}

namespace DXGIShared {
void ReleaseResize();
}

namespace DXGIShared {
bool ShouldBypassDX12InvisibleWindowPresent(IDXGISwapChain* pSwapChain, const char* presentName);
}

namespace DXGIShared {
HRESULT WINAPI SteamDummyRenderingCallback(IDXGISwapChain* /*pSwapChain*/, UINT /*SyncInterval*/, UINT /*Flags*/);
}

namespace DXGIShared {
bool IsReadableMemory(const void* ptr, size_t size);
}

namespace DXGIShared {
void* SelectSteamNullCallbackRecoveryTarget(const SteamNullCallbackRecoveryContext& recoveryContext);
}

namespace DXGIShared {
void** ResolveSteamNullCallbackSlotFromFault(uintptr_t returnAddress, uintptr_t steamStart, uintptr_t steamEnd);
}

namespace DXGIShared {
LONG CALLBACK SteamOverlayInitVehHandler(PEXCEPTION_POINTERS ep);
}

namespace DXGIShared {
PFN_Present EnsurePresentBypassTrampoline();
}

namespace DXGIShared {
bool IsSteamOverlayModule(const char* overlayModule);
}

namespace DXGIShared {
// Resolve a runtime-allocated Present hook thunk (`FF 25 00 00 00 00` + absolute
// pointer) to the real handler address inside the owning overlay DLL. Returns the
// hook address unchanged when it is not a resolvable thunk.
const void* ResolveExternalPresentHookThunkTarget(const void* externalHook);
}

namespace DXGIShared {
// Resolve the owning module path of a foreign Present hook (following its thunk
// when present). Returns false when no module backs the handler.
bool ResolveExternalPresentHookOwnerPath(const void* externalHook, char* modulePathOut, size_t modulePathOutCount);
}

namespace DXGIShared {
// True when control may still be transferred to this foreign Present handler: the entry, and
// for an E9/FF25 thunk the address it forwards to, must be committed executable memory. A
// handler captured once at install time goes stale when the owning overlay rebuilds or frees
// its runtime-allocated thunk.
bool IsCallableForeignPresentHandler(const void* handler);

// Re-derive the saved foreign Present hook from whoever owns the live dxgi!Present entry now.
PFN_Present RefreshExternalOverlayPresentHookFromLiveEntry();

// Saved foreign Present handler, validated (and refreshed when stale). nullptr means there is
// no safe foreign transport and the caller must use CE's clean DXGI bypass.
PFN_Present GetCallableExternalOverlayPresentHook();
}

namespace DXGIShared {
// Authoritative "does this foreign Present chain belong to Steam" classification.
// Resolves the thunk target into a module when possible; for unresolvable thunks
// falls back to load-order evidence (the most recently loaded overlay owns the
// entry jump) and then to the loaded-module name.
bool IsExternalPresentHookSteamChain(const void* externalHook);
}

namespace DXGIShared {
// Present-hot-path wrapper classifying the currently saved external Present hook.
bool IsCurrentExternalPresentHookSteamChain();
}

namespace DXGIShared {
bool IsStreamlineModuleHandle(HMODULE moduleHandle);
}

namespace DXGIShared {
bool IsCaptureHookModulePath(const char* modulePath);
}

namespace DXGIShared {
bool IsCodeAddressFromStreamlineModule(const void* codeAddress);
}

namespace DXGIShared {
bool TryGetModulePathFromCodeAddress(const void* codeAddress, char* modulePathOut, size_t modulePathOutCount, HMODULE* moduleOut = nullptr);
}

namespace DXGIShared {
bool HasStartupBlockingOverlayModuleInCurrentStack();
}

namespace DXGIShared {
bool HasStreamlineModuleInCurrentStack();
}

namespace DXGIShared {
bool IsWrappedSwapChainObject(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
APIType DetectAPIType(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
bool ShouldForceSteamDX12Bypass(IDXGISwapChain* pSwapChain, bool bypassAvailable, bool slLoaded,
                                const char** overlayModuleOut = nullptr, bool* isD3D12SwapChainOut = nullptr);
}

namespace DXGIShared {
bool ShouldForceThirdPartyOverlayBypass(IDXGISwapChain* pSwapChain, bool bypassAvailable, const char** overlayModuleOut = nullptr);
}

namespace DXGIShared {
DX12StartupPresentMode GetDX12StartupPresentMode(bool bypassAvailable, const char** overlayModuleOut = nullptr, int* passIndexOut = nullptr);
}

namespace DXGIShared {
bool IsVulkanPrimary();
}

namespace DXGIShared {
UINT ResolvePresentFrameLatencyOverride(const char** sourceOut);
}

namespace DXGIShared {
void WaitBackbufferFrameLatency(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
void ApplyPresentFrameLatencyOverrides(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
PerformanceMetrics* GetPerformanceMetrics();
}

namespace DXGIShared {
uint32_t GetLatestSourceFrameIndex();
}

namespace DXGIShared {
void SetLatestSourceFrameIndex(uint32_t frameIndex);
}

namespace DXGIShared {
APIType DetectAPIType(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
void* GetPresentAddress(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
void* GetPresent1Address(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
void InstallHooksIfPending(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
bool IsThirdPartyOverlayLoaded();
}

namespace DXGIShared {
bool IsReadableMemory(const void* ptr, size_t size);
}

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
}

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);
}

namespace DXGIShared {
bool HasExternalEntryHook(const void* target);
}

namespace DXGIShared {
void* ResolveE9JmpTarget(void* funcAddress);
}

namespace DXGIShared {
void* ResolveFF25JmpTarget(void* funcAddress);
}

namespace DXGIShared {
bool TrampolineChainsToExternalOverlay(void* trampoline, void* externalHook);
}

namespace DXGIShared {
bool IsSteamExternalChainTrampoline(void* trampoline, void* externalHook, bool isD3D12SwapChain);
}

namespace DXGIShared {
PFN_Present EnsurePresentBypassTrampoline();
}

namespace DXGIShared {
PFN_Present1 EnsurePresent1BypassTrampoline();
}

namespace DXGIShared {
bool TryReadSteamOverlayNullCallbackSlot(void** callbackValueOut);
}

namespace DXGIShared {
// Discover Steam's Present-shaped callback slot(s) in the loaded overlay module.
// Read-only: CE inspects them to decide whether invoking Steam is safe, and never
// writes into them (a speculative write makes Steam skip its own hook install and
// chain to a raw Present, dropping every overlay below Steam).
size_t DiscoverSteamNullCallbackSlots(HMODULE steamModule, uintptr_t* slotsOut, size_t maxSlots);
}

namespace DXGIShared {
bool TryGetSwapChainBackBufferIndex(IDXGISwapChain* pSwapChain, UINT* indexOut);
}

namespace DXGIShared {
bool TryInvokeGuardedExternalSteamOverlayPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, const char* reason, HRESULT* resultOut);
}

namespace DXGIShared {
bool IsSLInterposerLoaded();
}

namespace DXGIShared {
bool IsSLInterposerLoaded();
}

namespace DXGIShared {
bool ShouldKeepSLPresentRoutingDisabledNow(ce::fg_runtime::RuntimeMode* runtimeModeOut = nullptr, bool* runtimeOwnedNativeFGPresentPathOut = nullptr);
}

namespace DXGIShared {
void DetectSLPresentHook();
}

namespace DXGIShared {
void UpdateDXGIPresentMetricsAndPublish(bool isFirstHook, const char* publicationSource);
}

namespace DXGIShared {
void RefreshLivePresentHooksForSwapchainIfNeeded(IDXGISwapChain* pSwapChain, const char* source);
}

namespace DXGIShared {
bool IsDlssToggleEagerOverlayEnabled();
}

namespace DXGIShared {
void MaybeEagerDrawOverlayBeforeStreamlineStartupBypass(IDXGISwapChain* pSwapChain, bool isD3D12, bool streamlineFGRunning, bool postSLConfirmedRendering, bool hadFSRFGPhase, const char* site);
}

namespace DXGIShared {
PresentCallContext CapturePresentCallContext(IDXGISwapChain* pSwapChain, const void* detourCallerAddress, APIType api, bool presentBypassAvailable);
}

namespace DXGIShared {
HRESULT ExecuteStartupRouting(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, PresentCallContext& ctx, bool* earlyReturn);
}

namespace DXGIShared {
HRESULT ExecutePresentCore(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, const PresentCallContext& ctx);
}

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
}

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pPresentParameters);
}

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourResizeBuffers(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);
}

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourResizeBuffers1(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags, const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue);
}

namespace DXGIShared {
HRESULT STDMETHODCALLTYPE DetourSetColorSpace1(IDXGISwapChain* pSwapChain, DXGI_COLOR_SPACE_TYPE colorSpace);
}

namespace DXGIShared {
HRESULT SetSwapChainColorSpaceFromWrapper(IDXGISwapChain3* callableSwapChain, IDXGISwapChain* identitySwapChain, DXGI_COLOR_SPACE_TYPE colorSpace);
}

namespace DXGIShared {
void PublishSetColorSpace1Trampoline(void* trampoline, void*);
}

namespace DXGIShared {
bool InstallSetColorSpace1InlineHook(IDXGISwapChain* pSwapChain, const char* source);
}

namespace DXGIShared {
bool InstallHooks(IDXGISwapChain* pSwapChain, bool presentOnly);
}

namespace DXGIShared {
bool HasPresentInlineHooks();
}

namespace DXGIShared {
bool HasPresentDetourHooks();
}

namespace DXGIShared {
bool CanSafelyInstallExternalPresentDetourPath(bool requiresBypassTrampoline, bool bypassTrampolineAvailable);
}

namespace DXGIShared {
void SetPendingSwapChainForLazyHook(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
void InstallHooksIfPending(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
void Init();
}

namespace DXGIShared {
bool InstallPresentInlineHooks(IDXGISwapChain* pSwapChain);
}

namespace DXGIShared {
void RemovePresentHooks();
}

namespace DXGIShared {
void ReleaseSwapchainPresentVTableHooksForRuntimeHandoff(const char* reason);
}

namespace DXGIShared {
void RepairVTableHooksIfNeeded();
}

namespace DXGIShared {
void RemoveSwapchainVTableHooks();
}

namespace DXGIShared {
bool AttemptSteamDX12OverlayInit(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, PFN_Present presentOriginal, PFN_Present presentBypass, HRESULT* resultOut);
}

namespace DXGIShared {
HRESULT CallOriginalPresent(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);
}

namespace DXGIShared {
HRESULT CallOriginalPresent1(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags, const DXGI_PRESENT_PARAMETERS* pParams);
}

namespace DXGIShared {
void DisableSLPresentRouting();
}

#include "dxgi_shared_detail/present_state_globals.h"

namespace DXGIShared {
struct SteamNullCallbackRecoveryContext {
    const char* context = "unknown";
    const char* reason = nullptr;
    void* hook = nullptr;
    void* bypass = nullptr;
    bool streamlineStackActive = false;
    bool pluginLookupGuardReady = false;
};
}

namespace DXGIShared {
class ScopedSteamNullCallbackRecoveryGuard {
public:
    ScopedSteamNullCallbackRecoveryGuard(bool enabled, const char* context, const char* reason, void* hook,
                                         void* bypass, bool streamlineStackActive, bool pluginLookupGuardReady)
        : previousContext_(dxgi_shared_s_steamNullCallbackRecoveryContext) {
        if (!enabled) {
            return;
        }

        dxgi_shared_s_steamNullCallbackRecoveryContext = SteamNullCallbackRecoveryContext{
            context ? context : "unknown", reason, hook, bypass, streamlineStackActive, pluginLookupGuardReady,
        };
        handle_ = AddVectoredExceptionHandler(1, SteamOverlayInitVehHandler);
        if (handle_) {
            static std::atomic<int> s_guardInstallLogCount{0};
            const int logCount = s_guardInstallLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logCount <= 20 || logCount == 50 || (logCount % 500) == 0) {
                HookLogImportant(
                    "Guarded Steam Present hook installed Steam null-callback VEH recovery #%d "
                    "(context=%s reason=%s hook=%p bypass=%p streamlineStack=%d pluginGuard=%d tid=0x%04X)",
                    logCount, context ? context : "unknown", reason ? reason : "Present", hook, bypass,
                    streamlineStackActive ? 1 : 0, pluginLookupGuardReady ? 1 : 0, GetCurrentThreadId());
            }
        } else {
            HookLogImportant(
                "Guarded Steam Present hook failed to install Steam null-callback VEH recovery "
                "(context=%s reason=%s hook=%p bypass=%p streamlineStack=%d pluginGuard=%d err=%lu)",
                context ? context : "unknown", reason ? reason : "Present", hook, bypass, streamlineStackActive ? 1 : 0,
                pluginLookupGuardReady ? 1 : 0, GetLastError());
        }
    }

    ~ScopedSteamNullCallbackRecoveryGuard() {
        if (handle_) {
            RemoveVectoredExceptionHandler(handle_);
        }
        dxgi_shared_s_steamNullCallbackRecoveryContext = previousContext_;
    }

    ScopedSteamNullCallbackRecoveryGuard(const ScopedSteamNullCallbackRecoveryGuard&) = delete;
    ScopedSteamNullCallbackRecoveryGuard& operator=(const ScopedSteamNullCallbackRecoveryGuard&) = delete;

    bool IsInstalled() const {
        return handle_ != nullptr;
    }

private:
    SteamNullCallbackRecoveryContext previousContext_;
    PVOID handle_ = nullptr;
};
}

namespace DXGIShared {
struct PresentCallContext {
    APIType api = APIType::Unknown;
    bool wrappedSwapchain = false;
    bool inWrapperPresent = false;
    bool streamlineFGRunning = false;
    DWORD currentThreadId = 0;
    bool steamOverlayLoaded = false;
    bool presentBypassAvailable = false;
    char detourCallerModulePath[MAX_PATH] = {};
    bool callerFromThirdPartyOverlay = false;
    bool streamlineStartupHandoffPending = false;
    bool streamlineStartupTransitionWindowActive = false;
    bool streamlineStartupHandoffInProgress = false;
    DWORD presentOwner = 0;
    int presentDepthVal = 0;
    bool presentOwnershipActive = false;
    DWORD expectedPresentThreadId = 0;
    bool matchesExpectedPresentThread = false;
    bool callerFromStreamlineModule = false;
    bool callerFromFFXFrameGenerationModule = false;
    bool recentLargePresentGap = false;
    bool startupTopLevelPresentAlreadyConsumed = false;
    bool postSLStartupActivationPending = false;
    bool postSLActiveButUnconfirmed = false;
    bool postSLStartupActivationEntered = false;
    bool postSLConfirmedRendering = false;
    bool postSLConfirmedButStartupSettling = false;
    bool hadFSRFGPhase = false;
    bool explicitSetOptionsActivation = false;
    bool activeDLSSFGRuntimeSignalObserved = false;
    bool safePostFSRBootstrapPath = false;
    bool runtimeOwnedSwapchainActive = false;
    bool staleThirdPartyPresentHookRisk = false;
    bool observerOnlyMode = false;
    bool observerStartupPresentOnlyMode = false;
    bool ffxStartupBypass = false;
    bool streamlineSyntheticReentrant = false;
    bool startupTopLevelCandidate = false;
    bool stalePostFSRStartupHandoffPresentHookRisk = false;
    bool startupHandoffSteamRisk = false;
    bool postFSRRuntimeStartupHandoffRisk = false;
    bool streamlineStartupHandoffTransportRisk = false;
};
}
