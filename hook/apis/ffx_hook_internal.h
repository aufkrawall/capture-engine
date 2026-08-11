#pragma once

#include "ffx_hook.h"

#include <psapi.h>

#include <atomic>

#include <cstdint>

#include <cstring>

#include <mutex>

#include <unordered_map>

#include <unordered_set>

#include <vector>

#include "../common/dx12_overlay_policy.h"

#include "../common/dxgi_shared.h"

#include "../common/ffx_api_parsing.h"

#include "../common/fg_detection.h"

#include "../common/fg_session_state.h"

#include "../common/hook_common.h"

#include "../common/module_enumeration.h"

#include "../common/overlay_compat.h"

#include "../wrappers/iat_hook.h"

#include "../wrappers/inline_hook.h"

#include "dx12_hook.h"

#include "ffx_cached_pointer_router.h"

extern void DX12_OnNativeFSRFrameGenerationConfigured(bool enabled, bool retainedPresentCallbackBridge);

extern void DX12_ClearNativeFSRRuntimeOwnedTeardown(const char* ffx_hook_reason);

typedef void* ffxContext;

typedef uint32_t ffxReturnCode_t;

typedef uint64_t ffxStructType_t;

typedef struct ffxApiHeader {
    ffxStructType_t type;
    struct ffxApiHeader* pNext;
} ffxApiHeader;

typedef ffxApiHeader ffxCreateContextDescHeader;

typedef ffxApiHeader ffxConfigureDescHeader;

typedef void* (*ffxAlloc)(void* pUserData, uint64_t size);

typedef void (*ffxDealloc)(void* pUserData, void* pMem);

typedef struct ffxAllocationCallbacks {
    void* pUserData;
    ffxAlloc alloc;
    ffxDealloc dealloc;
} ffxAllocationCallbacks;

// Function signatures
typedef ffxReturnCode_t (*PfnFfxCreateContext)(ffxContext* ffx_hook_context, ffxCreateContextDescHeader* ffx_hook_desc,
                                               const ffxAllocationCallbacks* memCb);

typedef ffxReturnCode_t (*PfnFfxDestroyContext)(ffxContext* ffx_hook_context, const ffxAllocationCallbacks* memCb);

typedef ffxReturnCode_t (*PfnFfxConfigure)(ffxContext* ffx_hook_context, const ffxConfigureDescHeader* ffx_hook_desc);

FFXSubstituteUiReRegistrationResult FFXHook_ReRegisterSubstituteUiResource();

void FFXHook_LogSubstituteReRegFreezeDiagnostics(const char* ffx_hook_reason);

void FFXHook_ClearSubstituteUiReRegistration();

void FFXHook_ResetVehDisarmAndRearm();

namespace FFXHook {
void* GetPresentCallbackBridgeKey(void* ffx_hook_context);
}

namespace FFXHook {
void RegisterDynamicHooks();
}

namespace FFXHook {
bool InstallBridgeOnTrackedContexts(void* swapChain);
}

namespace FFXHook {
void Init();
}

namespace FFXHook {
bool IsInitialized();
}

namespace FFXHook {
void Shutdown();
}

// FFX Effect IDs - match values from ffx_api.h
inline constexpr uint32_t ffx_hook_FFX_API_EFFECT_MASK = 0x00ff0000u;

inline constexpr uint32_t ffx_hook_FFX_API_EFFECT_ID_FRAMEGENERATION = 0x00020000u;

inline constexpr uint32_t ffx_hook_FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN = 0x00030000u;

// Return codes
inline constexpr ffxReturnCode_t ffx_hook_FFX_API_RETURN_OK = 0;

inline std::mutex ffx_hook_g_InitMutex;

inline std::atomic<bool> ffx_hook_g_Initialized{false};

inline std::atomic<int> ffx_hook_g_FGContextCount{0};

inline std::atomic<bool> ffx_hook_g_NoModulesLogged{false};

inline std::once_flag ffx_hook_g_DynamicHookRegistrationOnce;

// CRITICAL FIX: Track context types to know if destroyed context is FG
inline std::mutex ffx_hook_g_ContextMapMutex;

inline std::unordered_map<ffxContext, uint32_t> ffx_hook_g_ContextTypeMap;

inline std::unordered_set<ffxContext> ffx_hook_g_VulkanContextSet;

// Serializes post-provider transition publication with successful context destruction. The FFX provider may invoke
// configure from multiple threads; CE's global routing/session mutations must never overlap or race context erasure.
inline std::mutex ffx_hook_g_FrameGenerationRoutingTransitionMutex;

namespace {
struct FrameGenerationRoutingState {
    bool enabled = false;
    bool bridgeActive = false;
    bool appCallbackProvided = false;
};
}

inline std::unordered_map<ffxContext, FrameGenerationRoutingState> ffx_hook_g_FrameGenerationRoutingByContext;

inline std::mutex ffx_hook_g_PresentCallbackBridgeMutex;

inline std::unordered_set<void*> ffx_hook_g_PresentCallbackBridgeKeys;

// Original function pointers
inline PfnFfxCreateContext ffx_hook_g_Original_ffxCreateContext = nullptr;

inline PfnFfxDestroyContext ffx_hook_g_Original_ffxDestroyContext = nullptr;

inline PfnFfxConfigure ffx_hook_g_Original_ffxConfigure = nullptr;

inline thread_local PfnFfxConfigure ffx_hook_t_FfxConfigureOriginalOverride = nullptr;

inline std::atomic<bool> ffx_hook_g_ffxCreateContextInlineHooked{false};

inline std::atomic<bool> ffx_hook_g_ffxDestroyContextInlineHooked{false};

inline std::atomic<bool> ffx_hook_g_ffxConfigureInlineHooked{false};

inline std::atomic<void*> ffx_hook_g_ffxCreateContextTarget{nullptr};

inline std::atomic<void*> ffx_hook_g_ffxDestroyContextTarget{nullptr};

inline std::atomic<void*> ffx_hook_g_ffxConfigureTarget{nullptr};

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
inline std::recursive_mutex ffx_hook_g_FfxConfigureBreakpointMutex;

inline void* ffx_hook_g_ffxConfigureVehHandle = nullptr;

inline uint8_t ffx_hook_g_ffxConfigureOriginalFirstByte = 0;

inline std::atomic<bool> ffx_hook_g_ffxConfigureVehArmed{false};

// One-shot VEH detection: after CE processes the first ENABLED no-callback ffxConfigure, the breakpoint is
// permanently disarmed (byte restored, no re-arm). Calls already routed through IAT/GetProcAddress or the
// cached-pointer router remain observable without touching AMD's code page. This eliminates the multi-threaded
// 0xCC contention that desyncs AMD's QPC-timed pacing (ffxQuery+0x225fe). GTA's ffxConfigure calls come from
// 6-7 threads; the 0xCC byte mutation
// on a code page those threads call into creates timing irregularities. The synthetic test app (single-
// threaded VEH hits) is stable; GTA (multi-threaded) wedges in ~8-10 frames. The latch resets on FG-off only
// when no durable caller-owned pointer route is available for the next on-transition.
inline std::atomic<bool> ffx_hook_g_ffxConfigureVehPermanentlyDisarmed{false};

// A stable client-owned ffxConfigure pointer route survives context destruction. While it is active, resetting
// FG state must not re-arm AMD's code-page breakpoint: the pointer route observes the next enable and every
// suspend/resume transition without executable-page mutation.
inline std::atomic<bool> ffx_hook_g_DurableCachedConfigureRouteActive{false};

inline std::atomic<int> ffx_hook_g_FfxConfigureOriginalForwardDepth{0};

inline std::atomic<bool> ffx_hook_g_FfxConfigureDeferredRearm{false};

inline std::atomic<void*> ffx_hook_g_FfxConfigureDeferredRearmTarget{nullptr};

bool ArmFfxConfigureBreakpoint(PfnFfxConfigure target, const char* ffx_hook_moduleName, const char* ffx_hook_reason);

void RestoreFfxConfigureBreakpointIfCurrent(void* target, const char* ffx_hook_reason);

ffxReturnCode_t CallFfxConfigureOriginalGuarded(PfnFfxConfigure originalConfigure, ffxContext* ffx_hook_context,
                                                       const ffxConfigureDescHeader* ffx_hook_desc);

void ClearSubstituteUiReRegistrationForContext(ffxContext ffx_hook_context);bool IsCommittedReadableCodeAddress(void* address);

inline template <typename T>
void RefreshDirectOriginalForModuleReload(T& original, T resolved, std::atomic<bool>& inlineHooked,
                                          std::atomic<void*>& targetSlot, const char* hookName,
                                          const char* ffx_hook_moduleName) {
    (void)targetSlot;
    if (!resolved) {
        return;
    }

    if (inlineHooked.load(std::memory_order_acquire)) {
        return;
    }

    if (original && original != resolved) {
        HookLogImportant("FFX Hook: Refreshing %s original after FFX module reload/rescan (old=%p new=%p module=%s)",
                         hookName, reinterpret_cast<void*>(original), reinterpret_cast<void*>(resolved),
                         ffx_hook_moduleName && ffx_hook_moduleName[0] ? ffx_hook_moduleName : "unknown");
    }

    original = resolved;
}

template <typename T>
struct FfxInlineHookPublication {
    T* destination = nullptr;
    T fallback = nullptr;
};

template <typename T>
void PublishFfxInlineHookTrampoline(void* trampoline, void* context) {
    auto* publication = static_cast<FfxInlineHookPublication<T>*>(context);
    *publication->destination =
        trampoline ? reinterpret_cast<T>(trampoline) : publication->fallback;
}

inline template <typename T>
bool InstallInlineHookOnce(void* target, void* detour, T& original, std::atomic<bool>& installedFlag,
                           std::atomic<void*>& targetSlot, const char* hookName) {
    if (!target) {
        return false;
    }

    if (target == detour) {
        original = nullptr;
        targetSlot.store(target, std::memory_order_release);
        installedFlag.store(true, std::memory_order_release);
        return true;
    }

    const void* installedTarget = targetSlot.load(std::memory_order_acquire);
    if (installedFlag.load(std::memory_order_acquire) && installedTarget == target) {
        const auto probeResult = FFXHook::detail::ProbeExpectedInlineDetourInstalled(target, detour);
        if (probeResult.state == FFXHook::detail::InlineDetourProbeState::kInstalledExpected) {
            return false;
        }

        if (probeResult.state == FFXHook::detail::InlineDetourProbeState::kUnreadableTarget) {
            static std::atomic<int> s_unreadableValidationLogCount{0};
            const int logCount = s_unreadableValidationLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 10) {
                HookLogImportant(
                    "FFX Hook: %s at %p became unreadable during inline-detour validation (error=%lu) - deferring "
                    "refresh until a later rescan",
                    hookName, target, probeResult.win32Error);
            }
            return false;
        }

        HookLogImportant("FFX Hook: %s at %p lost the expected detour patch; refreshing inline hook", hookName, target);
        if (!InlineHook::Remove(target)) {
            HookLogImportant(
                "FFX Hook: %s at %p could not remove stale inline-hook bookkeeping cleanly; retrying install", hookName,
                target);
        }
        installedFlag.store(false, std::memory_order_release);
        targetSlot.store(nullptr, std::memory_order_release);
    }

    FfxInlineHookPublication<T> publication{&original, original};
    void* trampoline = nullptr;
    if (!InlineHook::InstallPublished(target, detour, &trampoline, PublishFfxInlineHookTrampoline<T>,
                                      &publication)) {
        HookLogImportant("FFX Hook: Failed to inline hook %s at %p", hookName, target);
        return false;
    }

    targetSlot.store(target, std::memory_order_release);
    installedFlag.store(true, std::memory_order_release);
    HookLogImportant("FFX Hook: Inline hook installed for %s at %p (trampoline=%p)", hookName, target, trampoline);
    return true;
}

// Track which module we hooked (for cleanup)
inline HMODULE ffx_hook_g_HookedModule = nullptr;

inline ce::ffx_api::PresentCallback ffx_hook_g_DefaultPresentCallback = nullptr;

// Flag: VEH breakpoint hook installed for protected official AMD ffxConfigure
// (defined after Hooked_ffxConfigure below)
inline bool ffx_hook_g_ffxConfigureVehInstalled = false;

bool InstallFfxConfigureBreakpointHook(PfnFfxConfigure target, const char* ffx_hook_moduleName);void* GetOrCreatePresentCallbackBridgeKey(ffxContext ffx_hook_context);void* GetPresentCallbackBridgeKey(ffxContext ffx_hook_context);bool HasTrackedPresentCallbackBridgeKey(void* key);

// Extract effect ID from structure type
uint32_t GetEffectId(ffxStructType_t type);ffxReturnCode_t Hooked_ffxCreateContext(ffxContext* ffx_hook_context, ffxCreateContextDescHeader* ffx_hook_desc,
                                        const ffxAllocationCallbacks* memCb);ffxReturnCode_t Hooked_ffxDestroyContext(ffxContext* ffx_hook_context, const ffxAllocationCallbacks* memCb);

// --- Per-present substitute UI-resource re-registration (GTA no-callback FSR FG) ------------------------
// GTA Enhanced registers a 1x1 placeholder UI resource EVERY frame. CE substitutes its own backbuffer-sized
// texture, but only on the few RegisterUiResource calls intercepted BEFORE the one-shot ffxConfigure VEH
// disarms (session 20260624_004915: cfgFrame stops at 4). After that, GTA's per-frame RegisterUiResource(1x1)
// calls reach AMD directly and REVERT the registered UI resource to the empty 1x1, so AMD composites nothing
// and the overlay (correctly drawn onto CE's substitute every present) is invisible. To keep AMD compositing
// CE's substitute, CE re-asserts it once per present from DetourPresent's no-callback branch (after the
// composite, before the real Present), using the same ffxConfigure(RegisterUiResource) call GTA itself makes.
// This is the documented UI-composition mechanism — NOT GPU work on AMD's present queue/backbuffer — so it
// stays within the crash boundary. Only stored for the degenerate-substitute path (the test app re-registers
// its OWN real texture every frame, so it needs no re-assertion). Cleared when the substitute is released.
inline std::mutex ffx_hook_g_SubstReRegMutex;

inline ffxContext ffx_hook_g_SubstReRegContext = nullptr;

inline PfnFfxConfigure ffx_hook_g_SubstReRegConfigure = nullptr;

inline ce::ffx_api::ConfigureDescFrameGenerationSwapChainRegisterUiResource ffx_hook_g_SubstReRegDesc = {};

inline std::atomic<bool> ffx_hook_g_SubstReRegActive{false};void ClearSubstituteUiReRegistrationForContext(ffxContext ffx_hook_context);void StoreSubstituteUiReRegistration(
    ffxContext* ffx_hook_context, PfnFfxConfigure originalConfigure,
    const ce::ffx_api::ConfigureDescFrameGenerationSwapChainRegisterUiResource& substitutedDesc);ffxReturnCode_t Hooked_ffxConfigure(ffxContext* ffx_hook_context, const ffxConfigureDescHeader* ffx_hook_desc);bool IsFFXDynamicHookOwnerModule(const char* moduleBaseName, HMODULE module);void ffx_hook_RegisterDynamicHooksOnce();bool ffx_hook_InstallHooksForModule(HMODULE hModule, const char* ffx_hook_moduleName);void RestoreFfxConfigureBreakpointIfCurrent(void* target, const char* ffx_hook_reason);bool ArmFfxConfigureBreakpoint(PfnFfxConfigure target, const char* ffx_hook_moduleName, const char* ffx_hook_reason);ffxReturnCode_t CallFfxConfigureOriginalGuarded(PfnFfxConfigure originalConfigure, ffxContext* ffx_hook_context,
                                                       const ffxConfigureDescHeader* ffx_hook_desc);LONG WINAPI FfxConfigureBreakpointVEH(EXCEPTION_POINTERS* ep);bool InstallFfxConfigureBreakpointHook(PfnFfxConfigure target, const char* ffx_hook_moduleName);

// Retroactive ffxConfigure call: when FSR FG activates through Streamline's
// authoritative takeover (no direct ffxConfigure intercepted), CE calls
// g_Original_ffxConfigure on all tracked FG contexts to install the present
// callback bridge.  This is necessary because ffxConfigure is called during
// AMD module init, before CE can intercept it.
bool InstallBridgeOnTrackedContextsImpl(void* swapChain);
