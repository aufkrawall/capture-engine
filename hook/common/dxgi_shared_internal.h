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
bool ShouldForceSteamDX12Bypass(IDXGISwapChain* pSwapChain, bool bypassAvailable, bool slLoaded, const char** overlayModuleOut = nullptr);
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
bool IsVulkanActive();
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
PFN_Present EnsurePresentBypassTrampoline();
}

namespace DXGIShared {
PFN_Present1 EnsurePresent1BypassTrampoline();
}

namespace DXGIShared {
bool TryReadSteamOverlayNullCallbackSlot(void** callbackValueOut);
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

namespace DXGIShared {
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
extern SharedState g_SharedState;
}

namespace DXGIShared {
extern std::mutex g_SharedMutex;
}

namespace DXGIShared {
// Post-SL FG overlay callback (set by dx12_hook.cpp when SL FG is active).
extern std::atomic<PostSLOverlayRenderFn> g_PostSLOverlayRenderCallback;
}

namespace DXGIShared {
// Direct Streamline FG running signal (set by streamline_hook.cpp).
extern std::atomic<bool> g_StreamlineFGRunning;
}

namespace DXGIShared {
// Present call counter — incremented by DetourPresent and DetourPresent1, read by
// SL hook to detect bypass.
extern std::atomic<uint64_t> g_PresentCallCounter;
}

namespace DXGIShared {
// Global metrics for DXGI-based APIs
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
extern PerformanceMetrics dxgi_shared_g_DXGIPerfMetrics;
}

namespace DXGIShared {
// Recursion detection globals (avoiding thread_local which requires runtime
// init)
extern std::atomic<DWORD> dxgi_shared_g_presentThreadId;
}

namespace DXGIShared {
extern std::atomic<int> dxgi_shared_g_presentDepth;
}

namespace DXGIShared {
extern std::atomic<DWORD> dxgi_shared_g_resizeThreadId;
}

namespace DXGIShared {
extern std::atomic<int> dxgi_shared_g_resizeDepth;
}

namespace DXGIShared {
extern PFN_Present dxgi_shared_oPresent;
}

namespace DXGIShared {
extern PFN_Present1 dxgi_shared_oPresent1;
}

namespace DXGIShared {
extern PFN_ResizeBuffers dxgi_shared_oResizeBuffers;
}

namespace DXGIShared {
extern PFN_ResizeBuffers1 dxgi_shared_oResizeBuffers1;
}

namespace DXGIShared {
// Inline hook trampolines - calling these bypasses the hook entirely
extern PFN_Present dxgi_shared_oPresentTrampoline;
}

namespace DXGIShared {
extern PFN_Present1 dxgi_shared_oPresent1Trampoline;
}

namespace DXGIShared {
extern std::atomic<PFN_SetColorSpace1> dxgi_shared_oSetColorSpace1Trampoline;
}

namespace DXGIShared {
extern std::mutex dxgi_shared_s_setColorSpace1HookMutex;
}

namespace DXGIShared {
extern thread_local unsigned dxgi_shared_s_wrapperSetColorSpaceForwardDepth;
}

namespace DXGIShared {
// Bypass trampolines — skip external E9/FF25 hooks (e.g. Streamline) at the
// function entry point by executing original prologue bytes read from disk.
// Used in re-entrant Present calls to actually present the frame without
// re-entering the external hook chain.
extern PFN_Present dxgi_shared_oPresentBypass;
}

namespace DXGIShared {
extern PFN_Present1 dxgi_shared_oPresent1Bypass;
}

namespace DXGIShared {
// Saved target of the external E9 JMP on dxgi!Present, installed by Steam overlay
// (gameoverlayrenderer64!OverlayHookD3D3).  Captured during InstallPresentInlineHooks
// BEFORE Streamline overwrites it with its own JMP.  CE may invoke this target
// from SL-originated Present stacks only while the Streamline plugin-lookup guard
// is active; otherwise those paths use the bypass trampoline.
extern PFN_Present dxgi_shared_g_externalOverlayPresentHook;
}

namespace DXGIShared {
extern thread_local int dxgi_shared_s_externalOverlayPresentInvokeDepth;
}

namespace DXGIShared {
// Stored vtable pointer for unhooking Present when COM wrapper takes over
extern void** dxgi_shared_s_hookedVTable;
}

namespace DXGIShared {
// Saved original vtable[8] Present COM method captured from the temp swapchain
// at InstallPresentInlineHooks time, before any vtable modifications.  This is
// the real IDXGISwapChain::Present COM method (dxgi!CDXGISwapChain::Present or
// equivalent), not the inner dxgi!Present function that Steam hooks with an E9
// JMP.  Used in the E9 JMP path of CallOriginalPresent and
// AttemptSteamDX12OverlayInit to ensure DXGI COM method state management runs
// before dxgi!Present is called with Steam's E9 JMP.  Without this, calling
// dxgi!Present directly skips COM state management, which causes black screen
// on some DX12 games (e.g. Strange Brigade).
extern PFN_Present dxgi_shared_s_originalVtable8Present;
}

namespace DXGIShared {
// State for one-time Steam DX12 overlay initialization.
// Steam's OverlayHookD3D3 lazily initializes its internal "next" Present handler
// on first E9 JMP entry by reading vtable[8].  When vtable[8] = DetourPresent
// (our vtable hook), Steam's init fails and sets "next" = NULL, causing RIP=0.
//
// Fix: temporarily restore vtable[8] to the original dxgi!Present on the very
// first non-SL Steam overlay Present call, allowing Steam's init to complete.
// Re-hook vtable[8] to DetourPresent after Steam returns.
extern std::atomic<bool> dxgi_shared_s_steamDX12InitAttempted;
}

namespace DXGIShared {
extern bool dxgi_shared_s_steamInitCrashed;
}

namespace DXGIShared {
extern thread_local SteamNullCallbackRecoveryContext dxgi_shared_s_steamNullCallbackRecoveryContext;
}

namespace DXGIShared {
// Streamline FG routing state.
//
// Problem: When SL hooks Present with an E9 JMP at the function entry, our
// inline hook trampoline (oPresentTrampoline) bypasses SL's hook entirely,
// because the trampoline contains the ORIGINAL function bytes (from before
// any hooks).  With SL bypassed, Frame Generation never runs.
//
// Solution: Detect SL's E9 JMP on the Present function and route through it
// instead of through the trampoline.  This way:
//   Game → vtable[8] (DetourPresent) → overlay render →
//   oPresent (has SL E9 JMP) → SL_Detour → SL trampoline (has our FF 25) →
//   DetourPresent (re-entrant, forwarded to oPresentTrampoline) →
//   real Present → SL post-Present FG → return
//
// The vtable already points to DetourPresent (from inline hook install).
// We just need to change the FINAL call from oPresentTrampoline to oPresent.
extern std::atomic<bool> dxgi_shared_s_slRoutingActive;
}

namespace DXGIShared {
// Lazy hook installation - installs hooks on first Present if they were
// deferred during swapchain creation
extern IDXGISwapChain* dxgi_shared_s_PendingSwapChainForLazyHook;
}

namespace DXGIShared {
extern std::atomic<bool> dxgi_shared_s_LazyHooksInstalled;
}

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
