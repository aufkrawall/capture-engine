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

inline bool ArmFfxConfigureBreakpoint(PfnFfxConfigure target, const char* ffx_hook_moduleName, const char* ffx_hook_reason);

inline void RestoreFfxConfigureBreakpointIfCurrent(void* target, const char* ffx_hook_reason);

inline ffxReturnCode_t CallFfxConfigureOriginalGuarded(PfnFfxConfigure originalConfigure, ffxContext* ffx_hook_context,
                                                       const ffxConfigureDescHeader* ffx_hook_desc);

inline void ClearSubstituteUiReRegistrationForContext(ffxContext ffx_hook_context);

inline bool IsCommittedReadableCodeAddress(void* address) {
    if (!address) {
        return false;
    }

    MEMORY_BASIC_INFORMATION mbi = {};
    if (!VirtualQuery(address, &mbi, sizeof(mbi))) {
        return false;
    }
    if (mbi.State != MEM_COMMIT || (mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) {
        return false;
    }
    return true;
}

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

    void* trampoline = nullptr;
    if (!InlineHook::Install(target, detour, &trampoline)) {
        HookLogImportant("FFX Hook: Failed to inline hook %s at %p", hookName, target);
        return false;
    }

    original = reinterpret_cast<T>(trampoline);
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

inline bool InstallFfxConfigureBreakpointHook(PfnFfxConfigure target, const char* ffx_hook_moduleName);

inline void* GetOrCreatePresentCallbackBridgeKey(ffxContext ffx_hook_context) {
    if (!ffx_hook_context) {
        return nullptr;
    }

    // This value is passed to the official FSR runtime as an opaque callback
    // user context. Keep it aligned and real; some runtime paths are stricter
    // than the SDK smoke tests and can reject or touch tagged pointers.
    void* key = reinterpret_cast<void*>(ffx_hook_context);
    std::lock_guard<std::mutex> lock(ffx_hook_g_PresentCallbackBridgeMutex);
    ffx_hook_g_PresentCallbackBridgeKeys.insert(key);
    return key;
}

inline void* GetPresentCallbackBridgeKey(ffxContext ffx_hook_context) {
    if (!ffx_hook_context) {
        return nullptr;
    }

    return reinterpret_cast<void*>(ffx_hook_context);
}

inline bool HasTrackedPresentCallbackBridgeKey(void* key) {
    if (!key) {
        return false;
    }

    std::lock_guard<std::mutex> lock(ffx_hook_g_PresentCallbackBridgeMutex);
    return ffx_hook_g_PresentCallbackBridgeKeys.find(key) != ffx_hook_g_PresentCallbackBridgeKeys.end();
}

// Extract effect ID from structure type
inline uint32_t GetEffectId(ffxStructType_t type) {
    return static_cast<uint32_t>(type) & ffx_hook_FFX_API_EFFECT_MASK;
}

inline ffxReturnCode_t Hooked_ffxCreateContext(ffxContext* ffx_hook_context, ffxCreateContextDescHeader* ffx_hook_desc,
                                        const ffxAllocationCallbacks* memCb) {
    if (!ffx_hook_g_Original_ffxCreateContext) {
        HookLog("FFX Hook: ffxCreateContext called but original not set!");
        return 1;  // Error
    }

    // Parse the swapchain creation descriptor before forwarding: the output pointer is populated by AMD, while
    // the exact game/presentation queue is an input. Direct proxy-backbuffer work is legal only on this queue.
    const auto parsedSwapChainCreate =
        ce::ffx_api::ParseFrameGenerationSwapChainCreateState(reinterpret_cast<const ce::ffx_api::ApiHeader*>(ffx_hook_desc));
    const auto contextBackend =
        ce::ffx_api::ParseCreateContextBackend(reinterpret_cast<const ce::ffx_api::ApiHeader*>(ffx_hook_desc));
    const bool duringStreamlineStartup = DXGIShared::IsStreamlineStartupTransitionWindowActive();

    // Call original first
    ffxReturnCode_t result = ffx_hook_g_Original_ffxCreateContext(ffx_hook_context, ffx_hook_desc, memCb);

    if (result == ffx_hook_FFX_API_RETURN_OK && ffx_hook_desc) {
        uint32_t effectId = GetEffectId(ffx_hook_desc->type);

        if (parsedSwapChainCreate.recognized && ffx_hook_context && *ffx_hook_context && parsedSwapChainCreate.swapChainOutput &&
            *parsedSwapChainCreate.swapChainOutput && parsedSwapChainCreate.gameQueue) {
            DX12_RegisterNativeFSRSwapchainPresentationQueue(
                *ffx_hook_context, *parsedSwapChainCreate.swapChainOutput,
                static_cast<ID3D12CommandQueue*>(parsedSwapChainCreate.gameQueue));

            // ffxCreateContext has already returned the game-facing proxy and
            // its exact producer queue. Install the same module-validated
            // proxy Present prework used by ffxConfigure now, before the first
            // passthrough Present can race the later enabled configure. The
            // protected inner real swapchain remains fully quiesced; only the
            // proxy backbuffer receives CE work on the descriptor game queue.
            void* runtimeAnchor = ffx_hook_g_ffxCreateContextTarget.load(std::memory_order_acquire);
            if (!runtimeAnchor) {
                runtimeAnchor = reinterpret_cast<void*>(ffx_hook_g_Original_ffxCreateContext);
            }
            DX12_TryInstallFFXProxyPresentHook(*parsedSwapChainCreate.swapChainOutput, runtimeAnchor,
                                               "ffxCreateContext(FrameGenerationSwapChain)");
        }

        // Track successful context creation even during Streamline startup. This is passive bookkeeping only;
        // skipping it leaks the context count and queue binding when the matching destroy arrives later.
        bool newlyTrackedContext = false;
        {
            std::lock_guard<std::mutex> lock(ffx_hook_g_ContextMapMutex);
            const auto [it, inserted] = ffx_hook_g_ContextTypeMap.emplace(ffx_hook_context ? *ffx_hook_context : nullptr, effectId);
            newlyTrackedContext = inserted;
            if (!inserted) {
                it->second = effectId;
            }
            if (!ce::ffx_api::ShouldUseDX12FrameGenerationInterop(contextBackend)) {
                ffx_hook_g_VulkanContextSet.insert(ffx_hook_context ? *ffx_hook_context : nullptr);
            } else {
                ffx_hook_g_VulkanContextSet.erase(ffx_hook_context ? *ffx_hook_context : nullptr);
            }
        }

        // Check if this is a Frame Generation context
        if (newlyTrackedContext && !ce::ffx_api::ShouldUseDX12FrameGenerationInterop(contextBackend) &&
            ce::ffx_api::IsFrameGenerationEffectType(ffx_hook_desc->type)) {
            HookLogImportant(
                "FFX Hook: Vulkan Frame Generation context CREATED; DXGI/DX12 interop intentionally bypassed "
                "(context=%p type=0x%llx effectId=0x%x)",
                ffx_hook_context ? *ffx_hook_context : nullptr, (unsigned long long)ffx_hook_desc->type, effectId);
        } else if (newlyTrackedContext && (effectId == ffx_hook_FFX_API_EFFECT_ID_FRAMEGENERATION ||
                                           effectId == ffx_hook_FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN)) {
            int prevCount = ffx_hook_g_FGContextCount.fetch_add(1, std::memory_order_acq_rel);
            HookLog(
                "FFX Hook: Frame Generation context CREATED (type=0x%llx, "
                "effectId=0x%x, liveContexts=%d, streamlineStartup=%d)",
                (unsigned long long)ffx_hook_desc->type, effectId, prevCount + 1, duringStreamlineStartup ? 1 : 0);
            // Context creation proves support/lifetime only. Many titles pre-create the FSR contexts while FG
            // remains disabled; activation begins only after a successful enabled ffxConfigure/callback proof.
            (void)prevCount;
        }
    }

    return result;
}

inline ffxReturnCode_t Hooked_ffxDestroyContext(ffxContext* ffx_hook_context, const ffxAllocationCallbacks* memCb) {
    if (!ffx_hook_g_Original_ffxDestroyContext) {
        HookLog("FFX Hook: ffxDestroyContext called but original not set!");
        return 1;  // Error
    }

    const ffxContext contextHandle = ffx_hook_context ? *ffx_hook_context : nullptr;

    // Inspect before forwarding but commit no bookkeeping changes until AMD confirms destruction succeeded.
    // This preserves callback delegation and queue ownership if the provider rejects the destroy.
    bool isFGContext = false;
    bool isVulkanContext = false;
    bool isVulkanFGContext = false;
    {
        std::lock_guard<std::mutex> lock(ffx_hook_g_ContextMapMutex);
        auto it = ffx_hook_g_ContextTypeMap.find(contextHandle);
        if (it != ffx_hook_g_ContextTypeMap.end()) {
            uint32_t effectId = it->second;
            isVulkanContext = ffx_hook_g_VulkanContextSet.find(contextHandle) != ffx_hook_g_VulkanContextSet.end();
            isVulkanFGContext = isVulkanContext && ce::ffx_api::IsFrameGenerationEffectType(effectId);
            isFGContext = !isVulkanContext && (effectId == ffx_hook_FFX_API_EFFECT_ID_FRAMEGENERATION ||
                                               effectId == ffx_hook_FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN);
        }
    }

    // Call original
    ffxReturnCode_t result = ffx_hook_g_Original_ffxDestroyContext(ffx_hook_context, memCb);

    std::unique_lock<std::mutex> transitionLock;
    if (result == ffx_hook_FFX_API_RETURN_OK) {
        transitionLock = std::unique_lock<std::mutex>(ffx_hook_g_FrameGenerationRoutingTransitionMutex);
        {
            std::lock_guard<std::mutex> lock(ffx_hook_g_ContextMapMutex);
            ffx_hook_g_ContextTypeMap.erase(contextHandle);
            ffx_hook_g_VulkanContextSet.erase(contextHandle);
            ffx_hook_g_FrameGenerationRoutingByContext.erase(contextHandle);
        }
        if (!isVulkanContext) {
            {
                std::lock_guard<std::mutex> lock(ffx_hook_g_PresentCallbackBridgeMutex);
                ffx_hook_g_PresentCallbackBridgeKeys.erase(contextHandle);
            }
            DX12_ClearFFXPresentCallbackBridge(contextHandle);
            DX12_UnregisterNativeFSRSwapchainPresentationQueue(contextHandle, "FFX swapchain context destroyed");
            ClearSubstituteUiReRegistrationForContext(contextHandle);
        }
    }

    // Only decrement if this was actually an FG context
    if (result == ffx_hook_FFX_API_RETURN_OK && isFGContext) {
        int newCount = ffx_hook_g_FGContextCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (newCount < 0) {
            ffx_hook_g_FGContextCount.store(0, std::memory_order_release);
            newCount = 0;
        }

        HookLog("FFX Hook: FG Context destroyed (liveContexts=%d)", newCount);

        // Signal FG deactivation when all contexts are destroyed
        if (newCount == 0) {
            HookLog(
                "FFX Hook: FSR Frame Generation DEACTIVATED (all contexts "
                "destroyed)");
            DX12_ClearNativeFSRStartupConfigureArming("FFX FG context destroy");
            DX12_ClearNativeFSRRuntimeOwnedTeardown("FFX FG context destroy");
            // CRITICAL: Clear the progress-resolved assumption when the FFX
            // runtime destroys all FG contexts.  This ensures that if GTA
            // subsequently recreates FG contexts (e.g. after loading a save
            // game), the stale progress-resolved latch from the initial boot
            // does not permanently block the normal overlay fallback when the
            // FFX present-callback bridge fails to fire for the new session.
            DX12_ClearOfficialFFXRuntimeOwnedPresentPathAssumption("FFX FG context destroy");
            DX12_OnNativeFSRFrameGenerationContextsDestroyed();
            g_FGCompat.SetFSRFGActive(false);
            ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kFFXContextDestroy,
                                        "FFXHook::Hooked_ffxDestroyContext", reinterpret_cast<void*>(ffx_hook_context), nullptr,
                                        ce::fg_runtime::RuntimeMode::kOff, false, true);
        }
    } else if (result == ffx_hook_FFX_API_RETURN_OK && isVulkanFGContext) {
        HookLogImportant(
            "FFX Hook: Vulkan Frame Generation context destroyed; DXGI/DX12 teardown intentionally bypassed "
            "(context=%p)",
            contextHandle);
    } else if (result == ffx_hook_FFX_API_RETURN_OK && !isFGContext) {
        HookLog("FFX Hook: Non-FG Context destroyed");
    }

    return result;
}

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

inline std::atomic<bool> ffx_hook_g_SubstReRegActive{false};

inline void ClearSubstituteUiReRegistrationForContext(ffxContext ffx_hook_context) {
    if (!ffx_hook_context) {
        return;
    }
    std::lock_guard<std::mutex> lock(ffx_hook_g_SubstReRegMutex);
    if (ffx_hook_g_SubstReRegContext != ffx_hook_context) {
        return;
    }
    ffx_hook_g_SubstReRegActive.store(false, std::memory_order_release);
    ffx_hook_g_SubstReRegContext = nullptr;
    ffx_hook_g_SubstReRegConfigure = nullptr;
    ffx_hook_g_SubstReRegDesc = {};
    HookLogImportant("FFX Hook: Cleared substitute UI re-registration for destroyed context %p", ffx_hook_context);
}

inline void StoreSubstituteUiReRegistration(
    ffxContext* ffx_hook_context, PfnFfxConfigure originalConfigure,
    const ce::ffx_api::ConfigureDescFrameGenerationSwapChainRegisterUiResource& substitutedDesc) {
    std::lock_guard<std::mutex> lock(ffx_hook_g_SubstReRegMutex);
    ffx_hook_g_SubstReRegContext = ffx_hook_context ? *ffx_hook_context : nullptr;
    ffx_hook_g_SubstReRegConfigure = originalConfigure;
    ffx_hook_g_SubstReRegDesc = substitutedDesc;
    ffx_hook_g_SubstReRegActive.store(true, std::memory_order_release);
}

inline ffxReturnCode_t Hooked_ffxConfigure(ffxContext* ffx_hook_context, const ffxConfigureDescHeader* ffx_hook_desc) {
    PfnFfxConfigure originalConfigure =
        ffx_hook_t_FfxConfigureOriginalOverride ? ffx_hook_t_FfxConfigureOriginalOverride : ffx_hook_g_Original_ffxConfigure;
    if (!originalConfigure) {
        HookLog("FFX Hook: ffxConfigure called but original not set!");
        return 1;  // Error
    }
    const ffxContext contextHandle = ffx_hook_context ? *ffx_hook_context : nullptr;

    bool isVulkanContext = false;
    {
        std::lock_guard<std::mutex> lock(ffx_hook_g_ContextMapMutex);
        isVulkanContext = ffx_hook_g_VulkanContextSet.find(contextHandle) != ffx_hook_g_VulkanContextSet.end();
    }
    if (isVulkanContext) {
        const ffxReturnCode_t result = CallFfxConfigureOriginalGuarded(originalConfigure, ffx_hook_context, ffx_hook_desc);
        const auto parsed =
            ce::ffx_api::ParseFrameGenerationConfigureState(reinterpret_cast<const ce::ffx_api::ApiHeader*>(ffx_hook_desc));
        static std::atomic<int> s_vulkanConfigureBypassLogCount{0};
        const int logCount = s_vulkanConfigureBypassLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (logCount <= 20 || (logCount % 600) == 0) {
            HookLogImportant(
                "FFX Hook: Vulkan ffxConfigure forwarded without DXGI/DX12 interop "
                "(context=%p type=0x%llx frameGeneration=%d enabled=%d frameID=%llu result=%u log=%d)",
                contextHandle, static_cast<unsigned long long>(ffx_hook_desc ? ffx_hook_desc->type : 0), parsed.recognized ? 1 : 0,
                parsed.enabled ? 1 : 0, static_cast<unsigned long long>(parsed.frameId), result, logCount);
        }
        return result;
    }

    // During the Streamline startup window, skip CE-side processing to avoid
    // accessing DX12 swapchain state (HDR, callback bridges) while SL's
    // critical initialization is still in progress.  Just forward the call.
    if (DXGIShared::IsStreamlineStartupTransitionWindowActive()) {
        return CallFfxConfigureOriginalGuarded(originalConfigure, ffx_hook_context, ffx_hook_desc);
    }

    ce::ffx_api::ConfigureDescFrameGeneration localConfig = {};
    // Backing storage for a substituted RegisterUiResource forward (CE swaps in its own full-size UI texture
    // when the game registers a degenerate 1x1 placeholder). Function-scoped so it outlives the synchronous
    // forward at CallFfxConfigureOriginalGuarded below.
    ce::ffx_api::ConfigureDescFrameGenerationSwapChainRegisterUiResource localUiConfig = {};
    DX12FFXUiOverlayTargetPreparation uiTargetPreparation = {};
    bool uiTargetPrepared = false;
    bool uiTargetSubstituted = false;
    const ffxConfigureDescHeader* descToCall = ffx_hook_desc;
    const auto* parsedDesc = reinterpret_cast<const ce::ffx_api::ApiHeader*>(ffx_hook_desc);
    const bool recognizedFGConfigure = parsedDesc && parsedDesc->type == ce::ffx_api::kConfigureDescTypeFrameGeneration;
    ce::ffx_api::PresentCallback bridgedOriginalCallback = nullptr;
    void* bridgedOriginalUserContext = nullptr;
    bool usingDefaultPresentCallback = false;
    bool installedPresentCallbackBridge = false;
    bool retainedExistingBridgeForDisabledConfigure = false;
    bool retainedAlreadyBridgedPresentCallback = false;
    bool retainedBridgeForDisabledConfigure = false;
    bool retainedBridgeForNullCallbackToggle = false;
    bool disabledStartupArmingConfigure = false;
    bool appPresentCallbackProvided = false;
    bool alreadyBridgedPresentCallbackProvided = false;
    if (recognizedFGConfigure) {
        localConfig = *reinterpret_cast<const ce::ffx_api::ConfigureDescFrameGeneration*>(ffx_hook_desc);
        alreadyBridgedPresentCallbackProvided = DX12_IsFFXPresentCallbackBridgeCallback(localConfig.presentCallback);
        appPresentCallbackProvided = localConfig.presentCallback && !alreadyBridgedPresentCallbackProvided;
        disabledStartupArmingConfigure = ce::dx12_overlay_policy::ShouldTreatNativeFSRDisabledConfigureAsStartupArming(
            true, localConfig.frameGenerationEnabled != 0, DX12_IsNativeFSRStartupConfigureArmingPending(),
            DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration(), g_FGCompat.IsFSRFGApiActive(),
            g_FGCompat.HasDirectFFXApiConfirmation());
        DX12_TryCacheRuntimeOwnedCallbackHDRStateFromSwapchain(localConfig.swapChain);
        void* bridgeKey = nullptr;
        if (ce::dx12_overlay_policy::ShouldInstallFFXPresentCallbackBridgeForConfigure(
                true, localConfig.frameGenerationEnabled != 0,
                appPresentCallbackProvided || alreadyBridgedPresentCallbackProvided)) {
            bridgeKey = GetOrCreatePresentCallbackBridgeKey(contextHandle);
            if (alreadyBridgedPresentCallbackProvided) {
                retainedAlreadyBridgedPresentCallback = true;
                if (!localConfig.presentCallbackUserContext) {
                    localConfig.presentCallbackUserContext = bridgeKey;
                    descToCall = reinterpret_cast<const ffxConfigureDescHeader*>(&localConfig);
                }
                if (!DX12_HasFFXPresentCallbackBridge(bridgeKey)) {
                    DX12_SetFFXPresentCallbackBridge(bridgeKey, nullptr, nullptr);
                }
            } else {
                bridgedOriginalCallback =
                    localConfig.presentCallback ? localConfig.presentCallback : ffx_hook_g_DefaultPresentCallback;
                bridgedOriginalUserContext =
                    localConfig.presentCallback ? localConfig.presentCallbackUserContext : nullptr;
                usingDefaultPresentCallback = !localConfig.presentCallback && bridgedOriginalCallback;
                DX12_SetFFXPresentCallbackBridge(bridgeKey, bridgedOriginalCallback, bridgedOriginalUserContext);
            }
            localConfig.presentCallback = &DX12_RenderOverlayViaFFXPresentCallback;
            localConfig.presentCallbackUserContext = bridgeKey;
            descToCall = reinterpret_cast<const ffxConfigureDescHeader*>(&localConfig);
            installedPresentCallbackBridge = !retainedAlreadyBridgedPresentCallback;
        } else {
            bridgeKey = GetPresentCallbackBridgeKey(contextHandle);
            // App->null-callback toggle while FG stays ENABLED: AMD retains CE's bridge and keeps
            // calling it. Do NOT clear the bridge's retained original here — keep CE's bridge installed
            // and delegating to that retained app callback, so the composition is done correctly
            // instead of CE self-composing (which wedges AMD's presenter; session 20260615_021242).
            const bool retainBridgeForNullCallbackToggle =
                ce::dx12_overlay_policy::ShouldRetainFFXPresentCallbackBridgeForEnabledNullCallbackToggle(
                    true, localConfig.frameGenerationEnabled != 0, appPresentCallbackProvided,
                    HasTrackedPresentCallbackBridgeKey(bridgeKey) &&
                        DX12_HasFFXPresentCallbackBridgeWithOriginal(bridgeKey));
            if (localConfig.frameGenerationEnabled && !appPresentCallbackProvided &&
                !retainBridgeForNullCallbackToggle) {
                DX12_ClearFFXPresentCallbackBridge(bridgeKey);
            }
            if (retainBridgeForNullCallbackToggle) {
                localConfig.presentCallback = &DX12_RenderOverlayViaFFXPresentCallback;
                localConfig.presentCallbackUserContext = bridgeKey;
                descToCall = reinterpret_cast<const ffxConfigureDescHeader*>(&localConfig);
                retainedBridgeForNullCallbackToggle = true;
            }
            retainedExistingBridgeForDisabledConfigure =
                HasTrackedPresentCallbackBridgeKey(bridgeKey) && DX12_HasFFXPresentCallbackBridge(bridgeKey);
            if (ce::dx12_overlay_policy::ShouldRetainFFXPresentCallbackBridgeForDisabledConfigure(
                    true, localConfig.frameGenerationEnabled != 0, retainedExistingBridgeForDisabledConfigure,
                    disabledStartupArmingConfigure)) {
                localConfig.presentCallback = &DX12_RenderOverlayViaFFXPresentCallback;
                localConfig.presentCallbackUserContext = bridgeKey;
                descToCall = reinterpret_cast<const ffxConfigureDescHeader*>(&localConfig);
                retainedBridgeForDisabledConfigure = true;
            }
        }
    }

    // No-app-callback native FSR FG: the game registers its HUD as a UI resource EVERY frame
    // (type=0x00030002 on the swapchain context). AMD composites that UI resource onto BOTH real and
    // generated frames POST-interpolation on its own queue. Cache the UI texture (or substitute CE's own
    // backbuffer-sized texture for a degenerate game placeholder) so the per-present composite
    // can draw CE's overlay onto it on the target-compatible owner queue before proxy Present. AMD then
    // composites the overlay post-interpolation. Forwarded unchanged unless a substitute texture was swapped in.
    if (parsedDesc &&
        parsedDesc->type == ce::ffx_api::kConfigureDescTypeFrameGenerationSwapChainRegisterUiResourceDX12 &&
        (DX12_ShouldCacheFFXUiResourceForBundle() ||
         !ffx_hook_g_ffxConfigureVehPermanentlyDisarmed.load(std::memory_order_acquire))) {
        const auto* uiDesc =
            reinterpret_cast<const ce::ffx_api::ConfigureDescFrameGenerationSwapChainRegisterUiResource*>(ffx_hook_desc);
        if (uiDesc->uiResource.resource) {
            // Decide composite-onto-game-texture vs substitute-CE-full-size-texture and update the composite's
            // cached target. GTA leaves UI composition enabled but registers a 1x1 placeholder, so CE substitutes
            // its own backbuffer-sized texture and forwards THAT (so AMD composites the overlay onto every real +
            // generated frame). The test app / games that register a usable full-size UI texture forward unchanged
            // and CE blends the overlay onto the game's own texture via the per-present composite.
            ce::ffx_api::Resource ceSubstitute = {};
            uiTargetSubstituted =
                DX12_PrepareFFXUiOverlayTarget(uiDesc->uiResource, uiDesc->flags, &ceSubstitute, &uiTargetPreparation);
            uiTargetPrepared = uiTargetPreparation.target != nullptr;
            if (uiTargetSubstituted) {
                localUiConfig = *uiDesc;
                localUiConfig.uiResource = ceSubstitute;
                descToCall = reinterpret_cast<const ffxConfigureDescHeader*>(&localUiConfig);
            }
        } else {
            static std::atomic<int> s_emptyUiResLogCount{0};
            if (s_emptyUiResLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
                HookLogImportant(
                    "FFX Hook: native-FSR no-callback UI-resource register had empty uiResource (frame skipped)");
            }
        }
    }


    // Stamp QPC + frame counter for freeze-diagnosis timeline correlation (composite vs configure-forward).
    DX12_NoteFfxConfigureForward(parsedDesc ? parsedDesc->type : 0);

    const ffxReturnCode_t result = CallFfxConfigureOriginalGuarded(originalConfigure, ffx_hook_context, descToCall);
    if (uiTargetPrepared) {
        if (result == ffx_hook_FFX_API_RETURN_OK) {
            DX12_CommitFFXUiOverlayTarget(&uiTargetPreparation);
            if (uiTargetSubstituted) {
                // Publish re-registration only after AMD accepted the substitute. A failed configure must keep
                // the prior known-good target/descriptor intact.
                StoreSubstituteUiReRegistration(ffx_hook_context, originalConfigure, localUiConfig);
            } else {
                ClearSubstituteUiReRegistrationForContext(contextHandle);
            }
        } else {
            HookLogImportant(
                "FFX Hook: RegisterUiResource rejected (result=%d substitute=%d); preserving prior overlay target",
                static_cast<int>(result), uiTargetSubstituted ? 1 : 0);
            DX12_DiscardFFXUiOverlayTarget(&uiTargetPreparation);
        }
    }
    if (result != ffx_hook_FFX_API_RETURN_OK || !ffx_hook_desc) {
        return result;
    }

    const auto parsed =
        ce::ffx_api::ParseFrameGenerationConfigureState(reinterpret_cast<const ce::ffx_api::ApiHeader*>(ffx_hook_desc));
    if (!parsed.recognized) {
        return result;
    }

    // Capture the game-facing FFX FrameInterpolation PROXY swapchain and install the game-thread composite
    // driver (proxy Present prework). GTA passes the proxy in ffxConfigureDescFrameGeneration.swapChain of
    // the startup-arming AND enabled configures the one-shot VEH intercepts, so the hook is in place before
    // the first interpolated present. Idempotent + module-validated (only patches a Present entry that
    // resolves into the FFX runtime module); originalConfigure anchors that module check.
    if (recognizedFGConfigure && localConfig.swapChain) {
        // A protected inner DXGI create proves that this ffxCreateContext was already in flight as CE routed
        // cached export pointers. Its queue is FFX's internal presentQueue, not the descriptor gameQueue; recover
        // the retained pre-FSR original game/producer queue before the proxy hook becomes reachable. A primary
        // descriptor binding always wins.
        DX12_TryRecoverNativeFSRSwapchainPresentationQueue(contextHandle, localConfig.swapChain);
        DX12_TryInstallFFXProxyPresentHook(localConfig.swapChain, reinterpret_cast<void*>(originalConfigure),
                                           "ffxConfigure(FrameGeneration)");
    }

    if (installedPresentCallbackBridge) {
        static std::atomic<int> s_installedPresentCallbackBridgeLogCount{0};
        const int logCount = s_installedPresentCallbackBridgeLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0 || disabledStartupArmingConfigure) {
            const auto* originalDesc = reinterpret_cast<const ce::ffx_api::ConfigureDescFrameGeneration*>(ffx_hook_desc);
            HookLogImportant(
                "FFX Hook: Installed DX12 overlay present-callback bridge for context=%p frameID=%llu enabled=%d "
                "startupArming=%d originalPresent=%p resolvedPresent=%p usedDefaultPresent=%d log=%d",
                ffx_hook_context, static_cast<unsigned long long>(originalDesc->frameID),
                originalDesc->frameGenerationEnabled ? 1 : 0, disabledStartupArmingConfigure ? 1 : 0,
                reinterpret_cast<void*>(originalDesc->presentCallback),
                reinterpret_cast<void*>(bridgedOriginalCallback), usingDefaultPresentCallback ? 1 : 0, logCount + 1);
        }
    } else if (retainedAlreadyBridgedPresentCallback) {
        static std::atomic<int> s_retainedAlreadyBridgedPresentCallbackLogCount{0};
        const int logCount = s_retainedAlreadyBridgedPresentCallbackLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            const auto* originalDesc = reinterpret_cast<const ce::ffx_api::ConfigureDescFrameGeneration*>(ffx_hook_desc);
            HookLogImportant(
                "FFX Hook: Retained existing DX12 overlay present-callback bridge for already-bridged configure "
                "(context=%p frameID=%llu enabled=%d startupArming=%d originalPresent=%p originalUserCtx=%p log=%d)",
                ffx_hook_context, static_cast<unsigned long long>(originalDesc->frameID),
                originalDesc->frameGenerationEnabled ? 1 : 0, disabledStartupArmingConfigure ? 1 : 0,
                reinterpret_cast<void*>(originalDesc->presentCallback), originalDesc->presentCallbackUserContext,
                logCount + 1);
        }
    } else if (retainedBridgeForDisabledConfigure) {
        static std::atomic<int> s_retainedDisabledPresentCallbackBridgeLogCount{0};
        const int logCount = s_retainedDisabledPresentCallbackBridgeLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            const auto* originalDesc = reinterpret_cast<const ce::ffx_api::ConfigureDescFrameGeneration*>(ffx_hook_desc);
            HookLogImportant(
                "FFX Hook: Retained existing DX12 overlay present-callback bridge for disabled native-FSR configure "
                "(context=%p frameID=%llu originalPresent=%p bridgeUserCtx=%p log=%d)",
                ffx_hook_context, static_cast<unsigned long long>(originalDesc->frameID),
                reinterpret_cast<void*>(originalDesc->presentCallback), localConfig.presentCallbackUserContext,
                logCount + 1);
        }
    } else if (retainedBridgeForNullCallbackToggle) {
        static std::atomic<int> s_retainedNullCallbackToggleBridgeLogCount{0};
        const int logCount = s_retainedNullCallbackToggleBridgeLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            const auto* originalDesc = reinterpret_cast<const ce::ffx_api::ConfigureDescFrameGeneration*>(ffx_hook_desc);
            HookLogImportant(
                "FFX Hook: Retained DX12 overlay present-callback bridge across enabled app->null-callback toggle "
                "(AMD keeps calling CE's bridge; delegating to retained original instead of self-compose to avoid "
                "the ffxQuery wedge) (context=%p frameID=%llu appNull=1 bridgeUserCtx=%p log=%d)",
                ffx_hook_context, static_cast<unsigned long long>(originalDesc->frameID), localConfig.presentCallbackUserContext,
                logCount + 1);
        }
    } else if (disabledStartupArmingConfigure) {
        static std::atomic<int> s_disabledStartupArmingNoBridgeLogCount{0};
        const int logCount = s_disabledStartupArmingNoBridgeLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            const auto* originalDesc = reinterpret_cast<const ce::ffx_api::ConfigureDescFrameGeneration*>(ffx_hook_desc);
            HookLogImportant(
                "FFX Hook: Native FSR disabled startup-arming configure forwarded without CE present-callback bridge "
                "(context=%p frameID=%llu retainedBridge=%d originalPresent=%p log=%d)",
                ffx_hook_context, static_cast<unsigned long long>(originalDesc->frameID),
                retainedExistingBridgeForDisabledConfigure ? 1 : 0,
                reinterpret_cast<void*>(originalDesc->presentCallback), logCount + 1);
        }
    } else if (recognizedFGConfigure && parsed.enabled && !appPresentCallbackProvided) {
        static std::atomic<int> s_enabledNoPresentCallbackLogCount{0};
        const int logCount = s_enabledNoPresentCallbackLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            const auto* originalDesc = reinterpret_cast<const ce::ffx_api::ConfigureDescFrameGeneration*>(ffx_hook_desc);
            HookLogImportant(
                "FFX Hook: Native FSR enabled with no app present callback; preserving AMD internal "
                "no-callback composition and using normal DX12 overlay route "
                "(context=%p frameID=%llu originalPresent=%p log=%d)",
                ffx_hook_context, static_cast<unsigned long long>(originalDesc->frameID),
                reinterpret_cast<void*>(originalDesc->presentCallback), logCount + 1);
        }
    } else if (recognizedFGConfigure) {
        static std::atomic<int> s_configureNoBridgeLogCount{0};
        const int logCount = s_configureNoBridgeLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            const auto* originalDesc = reinterpret_cast<const ce::ffx_api::ConfigureDescFrameGeneration*>(ffx_hook_desc);
            HookLogImportant(
                "FFX Hook: Native FSR configure without DX12 present-callback bridge "
                "(context=%p frameID=%llu enabled=%d retainedBridge=%d originalPresent=%p log=%d)",
                ffx_hook_context, static_cast<unsigned long long>(originalDesc->frameID),
                originalDesc->frameGenerationEnabled ? 1 : 0, retainedExistingBridgeForDisabledConfigure ? 1 : 0,
                reinterpret_cast<void*>(originalDesc->presentCallback), logCount + 1);
        }
    }

    if (!parsed.enabled && disabledStartupArmingConfigure) {
        static std::atomic<int> s_disabledStartupArmingPreserveLogCount{0};
        const int logCount = s_disabledStartupArmingPreserveLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "FFX Hook: Native FSR disabled configure used for startup arming; preserving authoritative FSR state "
                "until direct enabled configure arrives (context=%p frameID=%llu runtimeOwned=%d directFFX=%d log=%d)",
                ffx_hook_context, static_cast<unsigned long long>(parsed.frameId),
                DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration() ? 1 : 0,
                g_FGCompat.HasDirectFFXApiConfirmation() ? 1 : 0, logCount + 1);
        }
        return result;
    }

    // Keep per-context dedupe and its global routing/session publications ordered even when a runtime emits
    // configure packets concurrently. The provider call itself remains outside this lock.
    std::lock_guard<std::mutex> transitionLock(ffx_hook_g_FrameGenerationRoutingTransitionMutex);
    const bool bridgeActiveForConfigure =
        installedPresentCallbackBridge || retainedAlreadyBridgedPresentCallback || retainedBridgeForDisabledConfigure;
    bool enabledStateChanged = parsed.enabled;
    bool routingStateChanged = true;
    {
        std::lock_guard<std::mutex> lock(ffx_hook_g_ContextMapMutex);
        const auto existing = ffx_hook_g_FrameGenerationRoutingByContext.find(contextHandle);
        if (existing != ffx_hook_g_FrameGenerationRoutingByContext.end()) {
            enabledStateChanged = existing->second.enabled != parsed.enabled;
            routingStateChanged = enabledStateChanged || existing->second.bridgeActive != bridgeActiveForConfigure ||
                                  existing->second.appCallbackProvided != appPresentCallbackProvided;
            existing->second = {parsed.enabled, bridgeActiveForConfigure, appPresentCallbackProvided};
        } else {
            // A first observed disabled configure has no live state to tear down. The first enabled configure is
            // a real transition and must still finalize protected FFX startup.
            enabledStateChanged = parsed.enabled;
            ffx_hook_g_FrameGenerationRoutingByContext.emplace(
                contextHandle,
                FrameGenerationRoutingState{parsed.enabled, bridgeActiveForConfigure, appPresentCallbackProvided});
        }
    }

    if (enabledStateChanged) {
        HookLogImportant("FFX Hook: Frame Generation configure transition %s (context=%p frameID=%llu type=0x%llx)",
                         parsed.enabled ? "ENABLED" : "DISABLED", ffx_hook_context,
                         static_cast<unsigned long long>(parsed.frameId), static_cast<unsigned long long>(ffx_hook_desc->type));
    } else {
        static std::atomic<int> s_unchangedConfigureLogCount{0};
        const int logCount = s_unchangedConfigureLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (logCount <= 10 || (logCount % 600) == 0) {
            HookLog(
                "FFX Hook: Frame Generation configure unchanged (%s context=%p frameID=%llu routingChanged=%d "
                "log=%d)",
                parsed.enabled ? "enabled" : "disabled", ffx_hook_context, static_cast<unsigned long long>(parsed.frameId),
                routingStateChanged ? 1 : 0, logCount);
        }
    }

    // Native FSR can keep its context alive while toggling FG on/off via
    // ffxConfigure. Trust that runtime signal over context lifetime.
    if (parsed.enabled && enabledStateChanged) {
        // MarkDirectFFXApiConfirmation intentionally requires the current FSR
        // activation to be live. Latch the API state before notifying DX12 so
        // the first enabled configure can finalize protected startup without an
        // extra hook re-arm pass on the runtime thread.
        g_FGCompat.SetFSRFGActive(true);
        const bool hadConfirmation = g_FGCompat.HasDirectFFXApiConfirmation();
        g_FGCompat.MarkDirectFFXApiConfirmation();
        if (!hadConfirmation && g_FGCompat.HasDirectFFXApiConfirmation()) {
            HookLogImportant(
                "FFX Hook: Direct FFX API confirmation established from ffxConfigure ENABLED "
                "(context=%p frameID=%llu)",
                ffx_hook_context, static_cast<unsigned long long>(parsed.frameId));
        }
    }
    const bool retainedBridgeForConfigure =
        !parsed.enabled && (retainedExistingBridgeForDisabledConfigure || retainedAlreadyBridgedPresentCallback ||
                            retainedBridgeForDisabledConfigure);
    if (routingStateChanged) {
        DX12_OnNativeFSRPresentCallbackRoutingConfigured(parsed.enabled, bridgeActiveForConfigure,
                                                         appPresentCallbackProvided);
    }
    if (enabledStateChanged) {
        DX12_OnNativeFSRFrameGenerationConfigured(parsed.enabled, retainedBridgeForConfigure);
        g_FGCompat.SetFSRFGActive(parsed.enabled);
        ce::fg_session::EmitFGEvent(
            parsed.enabled ? ce::fg_session::FGEventKind::kNativeFSRConfigureOn
                           : ce::fg_session::FGEventKind::kNativeFSRConfigureOff,
            "FFXHook::Hooked_ffxConfigure", reinterpret_cast<void*>(ffx_hook_context), nullptr,
            parsed.enabled ? ce::fg_runtime::RuntimeMode::kFSRFG : ce::fg_runtime::RuntimeMode::kOff, parsed.enabled,
            true);
    }
    return result;
}

inline bool IsFFXDynamicHookOwnerModule(const char* moduleBaseName, HMODULE module) {
    if (moduleBaseName && ce::overlay_compat::IsFFXFrameGenerationModulePath(moduleBaseName)) {
        return true;
    }

    char modulePath[MAX_PATH] = {};
    if (module && GetModuleFileNameA(module, modulePath, sizeof(modulePath))) {
        return ce::overlay_compat::IsFFXFrameGenerationModulePath(modulePath);
    }

    return false;
}

inline void RegisterDynamicHooksOnce() {
    std::call_once(ffx_hook_g_DynamicHookRegistrationOnce, [] {
        IATHook::RegisterDynamicHookFiltered("ffxCreateContext", reinterpret_cast<void*>(Hooked_ffxCreateContext),
                                             reinterpret_cast<void**>(&ffx_hook_g_Original_ffxCreateContext),
                                             IsFFXDynamicHookOwnerModule);
        IATHook::RegisterDynamicHookFiltered("ffxDestroyContext", reinterpret_cast<void*>(Hooked_ffxDestroyContext),
                                             reinterpret_cast<void**>(&ffx_hook_g_Original_ffxDestroyContext),
                                             IsFFXDynamicHookOwnerModule);
        IATHook::RegisterDynamicHookFiltered("ffxConfigure", reinterpret_cast<void*>(Hooked_ffxConfigure),
                                             reinterpret_cast<void**>(&ffx_hook_g_Original_ffxConfigure),
                                             IsFFXDynamicHookOwnerModule);
        HookLogImportant("FFX Hook: Registered module-filtered dynamic hooks for FFX exports");
    });
}

inline bool InstallHooksForModule(HMODULE hModule, const char* ffx_hook_moduleName) {
    if (!hModule)
        return false;

    const bool firstSeenModule = ffx_hook_g_HookedModule != hModule;
    if (firstSeenModule) {
        HookLog("FFX Hook: Installing hooks for module %s (%p)", ffx_hook_moduleName, hModule);
    }
    g_FGCompat.SetFSRFGSupportPresent(true);

    // Get the original functions
    PfnFfxCreateContext createCtx = (PfnFfxCreateContext)GetProcAddress(hModule, "ffxCreateContext");
    PfnFfxDestroyContext destroyCtx = (PfnFfxDestroyContext)GetProcAddress(hModule, "ffxDestroyContext");
    PfnFfxConfigure configureCtx = (PfnFfxConfigure)GetProcAddress(hModule, "ffxConfigure");

    if (!createCtx && !destroyCtx && !configureCtx) {
        static std::atomic<int> s_noExportLogCount{0};
        const int logCount = s_noExportLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (logCount <= 20 || (logCount % 300) == 0) {
            HookLog("FFX Hook: No supported FFX exports found in %s - skipping (log=%d)", ffx_hook_moduleName, logCount);
        }
        return false;
    }

    // Do not retire a live durable route merely because a related FFX DLL without ffxConfigure was observed.
    // Only a genuinely different callable configure export requires a new breakpoint/routing epoch.
    const void* installedConfigureTarget = ffx_hook_g_ffxConfigureTarget.load(std::memory_order_acquire);
    if (firstSeenModule && ffx_hook_g_HookedModule && configureCtx && installedConfigureTarget &&
        installedConfigureTarget != reinterpret_cast<void*>(configureCtx)) {
        ffx_hook_g_DurableCachedConfigureRouteActive.store(false, std::memory_order_release);
        ffx_hook_g_ffxConfigureVehPermanentlyDisarmed.store(false, std::memory_order_release);
    }

    const bool allowInlineHooks = ce::ffx_api::ShouldInlineHookFFXExportsForModule(ffx_hook_moduleName);
    const bool allowIATHooks = ce::ffx_api::ShouldPatchFFXImportsForModule(ffx_hook_moduleName);

    const auto resolvedDefaultPresentCallback =
        reinterpret_cast<ce::ffx_api::PresentCallback>(GetProcAddress(hModule, "ffxFrameInterpolationUiComposition"));
    if (resolvedDefaultPresentCallback && resolvedDefaultPresentCallback != ffx_hook_g_DefaultPresentCallback) {
        ffx_hook_g_DefaultPresentCallback = resolvedDefaultPresentCallback;
        if (ffx_hook_g_DefaultPresentCallback) {
            HookLogImportant("FFX Hook: Resolved default frame-interpolation present callback at %p",
                             reinterpret_cast<void*>(ffx_hook_g_DefaultPresentCallback));
        }
    }

    // Direct-export originals must follow protected/dynamic FFX module reloads.
    // GTA can unload amd_fidelityfx_dx12.dll and map it again at a new base when
    // a save enables native FSR; keeping the old ffxConfigure pointer turns the
    // next VEH hook call into a DEP fault on the unmapped image.
    RefreshDirectOriginalForModuleReload(ffx_hook_g_Original_ffxCreateContext, createCtx, ffx_hook_g_ffxCreateContextInlineHooked,
                                         ffx_hook_g_ffxCreateContextTarget, "ffxCreateContext", ffx_hook_moduleName);
    RefreshDirectOriginalForModuleReload(ffx_hook_g_Original_ffxDestroyContext, destroyCtx, ffx_hook_g_ffxDestroyContextInlineHooked,
                                         ffx_hook_g_ffxDestroyContextTarget, "ffxDestroyContext", ffx_hook_moduleName);
    RefreshDirectOriginalForModuleReload(ffx_hook_g_Original_ffxConfigure, configureCtx, ffx_hook_g_ffxConfigureInlineHooked,
                                         ffx_hook_g_ffxConfigureTarget, "ffxConfigure", ffx_hook_moduleName);
    ffx_hook_g_HookedModule = hModule;

    RegisterDynamicHooksOnce();

    const bool armProtectedConfigureBreakpoint =
        !allowInlineHooks && ce::ffx_api::ShouldArmProtectedOfficialFFXConfigureBreakpoint(ffx_hook_moduleName);

    if (!allowInlineHooks && allowIATHooks && ce::ffx_api::IsOfficialAMDFFXRuntimeModuleName(ffx_hook_moduleName) &&
        firstSeenModule) {
        HookLogImportant(
            "FFX Hook: Using IAT/dynamic hooks for protected official FFX module %s; inline export JMP patching "
            "skipped; guarded ffxConfigure VEH fallback %s for SDK dispatch-table/intra-module calls",
            ffx_hook_moduleName, armProtectedConfigureBreakpoint ? "enabled" : "not eligible");
    } else if (!allowInlineHooks && !allowIATHooks && firstSeenModule) {
        HookLogImportant(
            "FFX Hook: Using GetProcAddress-only hooks for protected official FFX module %s; inline export patching "
            "and IAT import patching skipped to avoid startup fail-fast; code bytes left unmodified; waiting for "
            "a real ffxConfigure call to arm the native FSR present-callback bridge",
            ffx_hook_moduleName);
    }

    // Install IAT hooks in loaded non-system/non-overlay modules to intercept calls to FFX functions.
    // Official AMD runtime DLLs are intentionally not inline-patched. Import-table routing is allowed because it
    // changes caller thunks instead of the AMD runtime code page and lets statically importing games expose the real
    // ffxConfigure packet needed for the present-callback bridge.

    void* dummy = nullptr;
    bool routedAnything = false;
    bool inlineHookedAnything = false;
    bool iatPatchedAnything = false;
    bool vehHookedAnything = false;
    if (createCtx) {
        routedAnything = true;
        if (allowInlineHooks) {
            inlineHookedAnything |=
                InstallInlineHookOnce(reinterpret_cast<void*>(createCtx),
                                      reinterpret_cast<void*>(Hooked_ffxCreateContext), ffx_hook_g_Original_ffxCreateContext,
                                      ffx_hook_g_ffxCreateContextInlineHooked, ffx_hook_g_ffxCreateContextTarget, "ffxCreateContext");
        }
        HookLog("FFX Hook: ffxCreateContext found at %p, hooking via %s (inline=%d)", createCtx,
                allowIATHooks ? "IAT/dynamic" : "dynamic-only", allowInlineHooks ? 1 : 0);
        if (allowIATHooks) {
            iatPatchedAnything |=
                IATHook::PatchIATAllModules(ffx_hook_moduleName, "ffxCreateContext", (void*)Hooked_ffxCreateContext, &dummy);
        }
    }

    if (destroyCtx) {
        routedAnything = true;
        if (allowInlineHooks) {
            inlineHookedAnything |=
                InstallInlineHookOnce(reinterpret_cast<void*>(destroyCtx),
                                      reinterpret_cast<void*>(Hooked_ffxDestroyContext), ffx_hook_g_Original_ffxDestroyContext,
                                      ffx_hook_g_ffxDestroyContextInlineHooked, ffx_hook_g_ffxDestroyContextTarget, "ffxDestroyContext");
        }
        HookLog("FFX Hook: ffxDestroyContext found at %p, hooking via %s (inline=%d)", destroyCtx,
                allowIATHooks ? "IAT/dynamic" : "dynamic-only", allowInlineHooks ? 1 : 0);
        if (allowIATHooks) {
            iatPatchedAnything |=
                IATHook::PatchIATAllModules(ffx_hook_moduleName, "ffxDestroyContext", (void*)Hooked_ffxDestroyContext, &dummy);
        }
    }

    if (configureCtx) {
        routedAnything = true;
        if (allowInlineHooks) {
            inlineHookedAnything |= InstallInlineHookOnce(
                reinterpret_cast<void*>(configureCtx), reinterpret_cast<void*>(Hooked_ffxConfigure),
                ffx_hook_g_Original_ffxConfigure, ffx_hook_g_ffxConfigureInlineHooked, ffx_hook_g_ffxConfigureTarget, "ffxConfigure");
        } else if (!allowIATHooks) {
            static std::atomic<int> s_protectedConfigureUnpatchedLogCount{0};
            const int logCount = s_protectedConfigureUnpatchedLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logCount <= 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "FFX Hook: Protected official FFX ffxConfigure export left unpatched "
                    "(module=%s target=%p log=%d); relying on GetProcAddress-visible API routing",
                    ffx_hook_moduleName ? ffx_hook_moduleName : "FFX", reinterpret_cast<void*>(configureCtx), logCount);
            }
        }
        HookLog("FFX Hook: ffxConfigure found at %p, hooking via %s (inline=%d veh=%d)", configureCtx,
                allowIATHooks ? (armProtectedConfigureBreakpoint ? "IAT/dynamic+VEH" : "IAT/dynamic")
                              : (armProtectedConfigureBreakpoint ? "dynamic+VEH" : "dynamic-only"),
                allowInlineHooks ? 1 : 0, armProtectedConfigureBreakpoint ? 1 : 0);
        if (allowIATHooks) {
            iatPatchedAnything |=
                IATHook::PatchIATAllModules(ffx_hook_moduleName, "ffxConfigure", (void*)Hooked_ffxConfigure, &dummy);
        }
        if (armProtectedConfigureBreakpoint) {
            // Protected official AMD module: install a re-arming VEH hook to
            // intercept SDK dispatch-table or intra-module ffxConfigure calls
            // that bypass GetProcAddress and caller import thunks. The handler
            // restores the byte before forwarding to the real function, then
            // re-arms after the call returns.
            vehHookedAnything |= InstallFfxConfigureBreakpointHook(configureCtx, ffx_hook_moduleName);
            if (!vehHookedAnything) {
                if (ffx_hook_g_ffxConfigureVehPermanentlyDisarmed.load(std::memory_order_acquire)) {
                    // Expected no-op: the one-shot VEH already detected no-callback mode and permanently
                    // disarmed for this FG-on window (re-armed on FG-off). The periodic module rescan used
                    // to log this as a failure every second (session 20260701_213656) — log once instead.
                    static std::atomic<int> s_disarmedRearmSkipLogCount{0};
                    if (s_disarmedRearmSkipLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
                        HookLog(
                            "FFX Hook: ffxConfigure VEH re-arm skipped — one-shot breakpoint permanently "
                            "disarmed for this FG-on window (module=%s target=%p)",
                            ffx_hook_moduleName ? ffx_hook_moduleName : "FFX", reinterpret_cast<void*>(configureCtx));
                    }
                } else {
                    static std::atomic<int> s_protectedConfigureVehFailureLogCount{0};
                    const int logCount =
                        s_protectedConfigureVehFailureLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
                    if (logCount <= 20 || (logCount % 300) == 0) {
                        HookLogImportant(
                            "FFX Hook: Failed to arm guarded ffxConfigure VEH fallback "
                            "(module=%s target=%p log=%d); relying on IAT/dynamic routing only",
                            ffx_hook_moduleName ? ffx_hook_moduleName : "FFX", reinterpret_cast<void*>(configureCtx), logCount);
                    }
                }
            }
        }
    }

    if (routedAnything) {
        static std::atomic<int> s_hooksInstalledLogCount{0};
        const int logCount = s_hooksInstalledLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (firstSeenModule || logCount <= 20 || (logCount % 300) == 0) {
            HookLog(
                "FFX Hook: Hooks installed successfully for %s (inline=%d iat=%d veh=%d dynamic=1 protected=%d "
                "log=%d)",
                ffx_hook_moduleName, inlineHookedAnything ? 1 : 0, iatPatchedAnything ? 1 : 0, vehHookedAnything ? 1 : 0,
                !allowInlineHooks ? 1 : 0, logCount);
        }
    }

    // A game can resolve FFX exports before CE's GetProcAddress hook wins startup initialization, then keep
    // those addresses in writable SDK dispatch/global slots forever. Route those already-resolved client slots
    // after the original exports and protected VEH fallback are fully initialized. This keeps
    // create/configure/destroy observable (exact gameQueue capture and immediate suspend/resume state) without
    // patching AMD's executable code or sustaining the contended entry breakpoint.
    const ce::ffx_cached_pointer_router::Route cachedRoutes[] = {
        {"ffxCreateContext", reinterpret_cast<void*>(createCtx), reinterpret_cast<void*>(Hooked_ffxCreateContext)},
        {"ffxDestroyContext", reinterpret_cast<void*>(destroyCtx), reinterpret_cast<void*>(Hooked_ffxDestroyContext)},
        {"ffxConfigure", reinterpret_cast<void*>(configureCtx), reinterpret_cast<void*>(Hooked_ffxConfigure)},
    };
    const auto cachedRouteResult =
        ce::ffx_cached_pointer_router::Refresh(hModule, cachedRoutes, _countof(cachedRoutes));
    constexpr std::uint64_t kConfigureRouteBit = std::uint64_t{1} << 2;
    if ((cachedRouteResult.routedRouteMask & kConfigureRouteBit) != 0) {
        // Once a durable client-owned ffxConfigure pointer routes through Hooked_ffxConfigure, the protected
        // entry breakpoint is redundant. Retire it immediately. A caller that fetched the original before the
        // atomic slot replacement still reaches the armed VEH; a caller that fetched the replacement reaches
        // Hooked_ffxConfigure. Setting the permanent latch before restoring the byte also prevents an in-flight
        // guarded forward from re-arming after it returns.
        const bool durableRouteWasActive =
            ffx_hook_g_DurableCachedConfigureRouteActive.exchange(true, std::memory_order_acq_rel);
        const bool breakpointWasArmed = ffx_hook_g_ffxConfigureVehArmed.load(std::memory_order_acquire);
        ffx_hook_g_ffxConfigureVehPermanentlyDisarmed.store(true, std::memory_order_release);
        RestoreFfxConfigureBreakpointIfCurrent(reinterpret_cast<void*>(configureCtx),
                                               "durable cached ffxConfigure pointer route installed");
        ffx_hook_g_FfxConfigureDeferredRearm.store(false, std::memory_order_release);
        ffx_hook_g_FfxConfigureDeferredRearmTarget.store(nullptr, std::memory_order_release);
        if (!durableRouteWasActive || breakpointWasArmed) {
            HookLogImportant(
                "FFX Hook: Retired protected ffxConfigure VEH breakpoint after installing a durable cached-pointer "
                "route — all later enable/disable transitions remain observable without AMD code-page mutation");
        }
    }
    if (cachedRouteResult.pointerSlotsPatched != 0) {
        HookLogImportant(
            "FFX Hook: Routed %zu pre-resolved FFX export pointer slot(s) across %zu client module(s) "
            "(%zu writable non-executable sections) — startup-cached create/configure/destroy calls now retain "
            "exact queue and transition visibility without modifying AMD code",
            cachedRouteResult.pointerSlotsPatched, cachedRouteResult.modulesScanned,
            cachedRouteResult.writableSectionsScanned);
    }
    return true;
}

inline void RestoreFfxConfigureBreakpointIfCurrent(void* target, const char* ffx_hook_reason) {
    std::lock_guard<std::recursive_mutex> lock(ffx_hook_g_FfxConfigureBreakpointMutex);
    if (!target || !ffx_hook_g_ffxConfigureVehArmed.load(std::memory_order_acquire) ||
        ffx_hook_g_ffxConfigureTarget.load(std::memory_order_acquire) != target) {
        return;
    }

    if (!IsCommittedReadableCodeAddress(target)) {
        ffx_hook_g_ffxConfigureVehArmed.store(false, std::memory_order_release);
        HookLogImportant("FFX Hook: Dropping stale VEH breakpoint state for unloaded ffxConfigure target %p (%s)",
                         target, ffx_hook_reason && ffx_hook_reason[0] ? ffx_hook_reason : "target changed");
        return;
    }

    auto* targetByte = static_cast<uint8_t*>(target);
    if (*targetByte != 0xCC) {
        ffx_hook_g_ffxConfigureVehArmed.store(false, std::memory_order_release);
        return;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        HookLogImportant("FFX Hook: Failed to restore stale VEH breakpoint at %p before retargeting (err=%lu)", target,
                         GetLastError());
        return;
    }
    *targetByte = ffx_hook_g_ffxConfigureOriginalFirstByte;
    FlushInstructionCache(GetCurrentProcess(), targetByte, 1);
    VirtualProtect(target, 1, PAGE_EXECUTE_READ, &oldProtect);
    ffx_hook_g_ffxConfigureVehArmed.store(false, std::memory_order_release);
    HookLogImportant("FFX Hook: Restored stale VEH breakpoint at %p before retargeting (%s)", target,
                     ffx_hook_reason && ffx_hook_reason[0] ? ffx_hook_reason : "target changed");
}

inline bool ArmFfxConfigureBreakpoint(PfnFfxConfigure target, const char* ffx_hook_moduleName, const char* ffx_hook_reason) {
    std::lock_guard<std::recursive_mutex> lock(ffx_hook_g_FfxConfigureBreakpointMutex);
    if (!target) {
        return false;
    }

    // One-shot detection: if the VEH was permanently disarmed (after the first enabled no-callback
    // ffxConfigure), do NOT re-arm. The byte stays as the original — ffxConfigure runs natively.
    if (ffx_hook_g_ffxConfigureVehPermanentlyDisarmed.load(std::memory_order_acquire)) {
        return false;
    }

    if (ffx_hook_g_FfxConfigureOriginalForwardDepth.load(std::memory_order_acquire) > 0) {
        ffx_hook_g_FfxConfigureDeferredRearmTarget.store(reinterpret_cast<void*>(target), std::memory_order_release);
        ffx_hook_g_FfxConfigureDeferredRearm.store(true, std::memory_order_release);
        if (!ffx_hook_g_ffxConfigureInlineHooked.load(std::memory_order_acquire) && ffx_hook_g_Original_ffxConfigure != target) {
            HookLogImportant(
                "FFX Hook: Updating protected ffxConfigure original while deferring VEH arm "
                "(old=%p new=%p module=%s reason=%s)",
                reinterpret_cast<void*>(ffx_hook_g_Original_ffxConfigure), reinterpret_cast<void*>(target),
                ffx_hook_moduleName ? ffx_hook_moduleName : "FFX", ffx_hook_reason ? ffx_hook_reason : "unknown");
            ffx_hook_g_Original_ffxConfigure = target;
        }
        static std::atomic<int> s_deferredArmLogCount{0};
        const int logCount = s_deferredArmLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (logCount <= 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "FFX Hook: Deferring ffxConfigure VEH breakpoint arm while original forwarding is active "
                "(target=%p module=%s reason=%s depth=%d log=%d)",
                reinterpret_cast<void*>(target), ffx_hook_moduleName ? ffx_hook_moduleName : "FFX", ffx_hook_reason ? ffx_hook_reason : "unknown",
                ffx_hook_g_FfxConfigureOriginalForwardDepth.load(std::memory_order_acquire), logCount);
        }
        return true;
    }

    if (!IsCommittedReadableCodeAddress(reinterpret_cast<void*>(target))) {
        HookLogImportant("FFX Hook: Refusing to arm VEH breakpoint for unreadable ffxConfigure target %p (%s)",
                         reinterpret_cast<void*>(target), ffx_hook_moduleName ? ffx_hook_moduleName : "FFX");
        return false;
    }

    void* previousTarget = ffx_hook_g_ffxConfigureTarget.load(std::memory_order_acquire);
    if (previousTarget && previousTarget != reinterpret_cast<void*>(target)) {
        RestoreFfxConfigureBreakpointIfCurrent(previousTarget, "ffxConfigure target changed");
    }

    auto* targetByte = reinterpret_cast<uint8_t*>(target);
    const uint8_t currentByte = *targetByte;
    const bool alreadyArmed = ffx_hook_g_ffxConfigureVehArmed.load(std::memory_order_acquire) &&
                              ffx_hook_g_ffxConfigureTarget.load(std::memory_order_acquire) == reinterpret_cast<void*>(target) &&
                              currentByte == 0xCC;
    if (alreadyArmed) {
        return true;
    }

    if (currentByte != 0xCC) {
        ffx_hook_g_ffxConfigureOriginalFirstByte = currentByte;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(target), 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    *targetByte = 0xCC;
    FlushInstructionCache(GetCurrentProcess(), targetByte, 1);
    VirtualProtect(reinterpret_cast<LPVOID>(target), 1, PAGE_EXECUTE_READ, &oldProtect);

    ffx_hook_g_ffxConfigureTarget.store(reinterpret_cast<void*>(target), std::memory_order_release);
    ffx_hook_g_ffxConfigureVehArmed.store(true, std::memory_order_release);
    if (!ffx_hook_g_ffxConfigureInlineHooked.load(std::memory_order_acquire) && ffx_hook_g_Original_ffxConfigure != target) {
        HookLogImportant("FFX Hook: Updating protected ffxConfigure original for VEH target (old=%p new=%p)",
                         reinterpret_cast<void*>(ffx_hook_g_Original_ffxConfigure), reinterpret_cast<void*>(target));
        ffx_hook_g_Original_ffxConfigure = target;
    }

    const bool postCallRearm = ffx_hook_reason && std::strcmp(ffx_hook_reason, "post-call rearm") == 0;
    bool shouldLogArm = true;
    int rearmLogCount = 0;
    if (postCallRearm) {
        static std::atomic<int> s_postCallRearmLogCount{0};
        rearmLogCount = s_postCallRearmLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        shouldLogArm = rearmLogCount <= 20 || (rearmLogCount % 300) == 0;
    }
    if (shouldLogArm) {
        if (postCallRearm) {
            HookLogImportant("FFX Hook: %s VEH breakpoint for %s!ffxConfigure at %p (%s #%d)",
                             currentByte == 0xCC ? "Confirmed" : "Armed", ffx_hook_moduleName ? ffx_hook_moduleName : "FFX",
                             reinterpret_cast<void*>(target), ffx_hook_reason, rearmLogCount);
        } else {
            HookLogImportant("FFX Hook: %s VEH breakpoint for %s!ffxConfigure at %p (%s)",
                             currentByte == 0xCC ? "Confirmed" : "Armed", ffx_hook_moduleName ? ffx_hook_moduleName : "FFX",
                             reinterpret_cast<void*>(target), ffx_hook_reason ? ffx_hook_reason : "init");
        }
    }
    return true;
}

inline ffxReturnCode_t CallFfxConfigureOriginalGuarded(PfnFfxConfigure originalConfigure, ffxContext* ffx_hook_context,
                                                       const ffxConfigureDescHeader* ffx_hook_desc) {
    if (!originalConfigure) {
        return 1;
    }

    ffx_hook_g_FfxConfigureOriginalForwardDepth.fetch_add(1, std::memory_order_acq_rel);
    bool pausedBreakpoint = false;
    {
        std::lock_guard<std::recursive_mutex> lock(ffx_hook_g_FfxConfigureBreakpointMutex);
        void* target = reinterpret_cast<void*>(originalConfigure);
        if (!ffx_hook_g_ffxConfigureInlineHooked.load(std::memory_order_acquire) &&
            ffx_hook_g_ffxConfigureVehArmed.load(std::memory_order_acquire) &&
            ffx_hook_g_ffxConfigureTarget.load(std::memory_order_acquire) == target && IsCommittedReadableCodeAddress(target)) {
            auto* targetByte = static_cast<uint8_t*>(target);
            if (*targetByte == 0xCC) {

                DWORD oldProtect = 0;
                if (VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                    *targetByte = ffx_hook_g_ffxConfigureOriginalFirstByte;
                    FlushInstructionCache(GetCurrentProcess(), targetByte, 1);
                    VirtualProtect(target, 1, PAGE_EXECUTE_READ, &oldProtect);
                    ffx_hook_g_ffxConfigureVehArmed.store(false, std::memory_order_release);
                    pausedBreakpoint = true;

                    static std::atomic<int> s_forwardPauseLogCount{0};
                    const int logCount = s_forwardPauseLogCount.fetch_add(1, std::memory_order_relaxed);
                    if (logCount < 20 || (logCount % 300) == 0) {
                        HookLogImportant(
                            "FFX Hook: Temporarily paused protected ffxConfigure VEH breakpoint for guarded "
                            "original forwarding (target=%p log=%d)",
                            target, logCount + 1);
                    }
                } else {
                    HookLogImportant(
                        "FFX Hook: Failed to pause protected ffxConfigure VEH breakpoint before forwarding "
                        "(target=%p err=%lu)",
                        target, GetLastError());
                }
            }
        }
    }

    const ffxReturnCode_t result = originalConfigure(ffx_hook_context, ffx_hook_desc);

    if (pausedBreakpoint) {
        ffx_hook_g_FfxConfigureDeferredRearmTarget.store(reinterpret_cast<void*>(originalConfigure), std::memory_order_release);
        ffx_hook_g_FfxConfigureDeferredRearm.store(true, std::memory_order_release);
    }

    const int remainingDepth = ffx_hook_g_FfxConfigureOriginalForwardDepth.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remainingDepth == 0 && ffx_hook_g_FfxConfigureDeferredRearm.exchange(false, std::memory_order_acq_rel)) {
        void* deferredTarget = ffx_hook_g_FfxConfigureDeferredRearmTarget.exchange(nullptr, std::memory_order_acq_rel);
        if (!deferredTarget) {
            deferredTarget = reinterpret_cast<void*>(originalConfigure);
        }
        ArmFfxConfigureBreakpoint(reinterpret_cast<PfnFfxConfigure>(deferredTarget), "protected official FFX runtime",
                                  "forward-call rearm");
    }
    return result;
}

inline LONG WINAPI FfxConfigureBreakpointVEH(EXCEPTION_POINTERS* ep) {
    if (!ep) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    auto* ctx = ep->ContextRecord;
    auto* rec = ep->ExceptionRecord;

    void* target = ffx_hook_g_ffxConfigureTarget.load(std::memory_order_acquire);
    const uintptr_t instructionPointer =
#ifdef _WIN64
        ctx ? static_cast<uintptr_t>(ctx->Rip) : 0;
#else
        ctx ? static_cast<uintptr_t>(ctx->Eip) : 0;
#endif
    if (!ctx || !rec || rec->ExceptionCode != STATUS_BREAKPOINT || !target ||
        !FFXHook::detail::IsEntryBreakpointHit(rec->ExceptionAddress, instructionPointer, target) ||
        !ffx_hook_g_ffxConfigureVehArmed.load(std::memory_order_acquire)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(target), 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    *static_cast<uint8_t*>(reinterpret_cast<LPVOID>(target)) = ffx_hook_g_ffxConfigureOriginalFirstByte;
    FlushInstructionCache(GetCurrentProcess(), target, 1);
    VirtualProtect(reinterpret_cast<LPVOID>(target), 1, PAGE_EXECUTE_READ, &oldProtect);
    ffx_hook_g_ffxConfigureVehArmed.store(false, std::memory_order_release);

    auto contextPtr = reinterpret_cast<ffxContext*>(
#ifdef _WIN64
        ctx->Rcx
#else
        ctx->Ecx
#endif
    );
    auto* ffx_hook_desc = reinterpret_cast<const ffxConfigureDescHeader*>(
#ifdef _WIN64
        ctx->Rdx
#else
        ctx->Edx
#endif
    );
    PfnFfxConfigure previousOverride = ffx_hook_t_FfxConfigureOriginalOverride;
    ffx_hook_t_FfxConfigureOriginalOverride = reinterpret_cast<PfnFfxConfigure>(target);
    ffxReturnCode_t result = Hooked_ffxConfigure(contextPtr, ffx_hook_desc);
    ffx_hook_t_FfxConfigureOriginalOverride = previousOverride;

    static std::atomic<int> s_vehHitLogCount{0};
    const int hitCount = s_vehHitLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (hitCount <= 20 || (hitCount % 300) == 0) {
        const auto* parsedDesc = reinterpret_cast<const ce::ffx_api::ApiHeader*>(ffx_hook_desc);
        HookLogImportant(
            "FFX Hook: VEH ffxConfigure breakpoint handled call #%d (context=%p desc=%p type=0x%llx result=%u)",
            hitCount, contextPtr, ffx_hook_desc, parsedDesc ? static_cast<unsigned long long>(parsedDesc->type) : 0ULL,
            static_cast<unsigned>(result));
    }

    // One-shot VEH detection: disarm only after BOTH (1) the no-callback flag is set AND (2) the UI
    // texture has been cached from a RegisterUiResource call. If we disarm before the cache is populated,
    // an otherwise-unrouted caller runs natively and the cache is never filled → the bundle never fires.
    // The RegisterUiResource (type=0x30002) may arrive before or after the enabled configure (type=0x20002)
    // that sets the no-callback flag, so we must wait for both conditions.
    if (ffx_hook_desc && DX12_IsNativeFSRInternalNoCallbackCompositionActive() && DX12_IsFFXUiResourceCachedForBundle() &&
        !ffx_hook_g_ffxConfigureVehPermanentlyDisarmed.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant(
            "FFX Hook: VEH permanently disarmed after one-shot no-callback detection (context=%p) — "
            "AMD's code page stays native; IAT/GetProcAddress/cached-pointer routes remain observable without "
            "multi-threaded 0xCC contention that desyncs ffxQuery pacing",
            contextPtr);
    } else {
        // Re-arm after the real ffxConfigure returns. This keeps protected official
        // runtimes on a breakpoint hook without installing a permanent JMP, and
        // prevents early non-FG/disabled configures from consuming the only chance
        // to catch a later save-load FG enable.
        ArmFfxConfigureBreakpoint(reinterpret_cast<PfnFfxConfigure>(target), "protected official FFX runtime",
                                  "post-call rearm");
    }

    // Return directly to the caller: pop the return address from the stack
    // and skip the patched ffxConfigure body entirely (Hooked_ffxConfigure
    // already called g_Original_ffxConfigure, so re-executing would double-call).
#ifdef _WIN64
    ctx->Rax = result;
    ctx->Rip = *reinterpret_cast<ULONG64*>(ctx->Rsp);
    ctx->Rsp += 8;
#else
    ctx->Eax = result;
    ctx->Eip = *reinterpret_cast<DWORD*>(ctx->Esp);
    ctx->Esp += 4;
#endif
    return EXCEPTION_CONTINUE_EXECUTION;
}

inline bool InstallFfxConfigureBreakpointHook(PfnFfxConfigure target, const char* ffx_hook_moduleName) {
    if (!ffx_hook_g_ffxConfigureVehHandle) {
        ffx_hook_g_ffxConfigureVehHandle = AddVectoredExceptionHandler(1, FfxConfigureBreakpointVEH);
        if (!ffx_hook_g_ffxConfigureVehHandle) {
            return false;
        }
    }

    const bool alreadyInstalledAndArmed =
        ffx_hook_g_ffxConfigureVehInstalled && ffx_hook_g_ffxConfigureVehArmed.load(std::memory_order_acquire) &&
        ffx_hook_g_ffxConfigureTarget.load(std::memory_order_acquire) == reinterpret_cast<void*>(target);
    if (!ArmFfxConfigureBreakpoint(target, ffx_hook_moduleName, "init")) {
        return false;
    }
    ffx_hook_g_ffxConfigureVehInstalled = true;
    if (!alreadyInstalledAndArmed) {
        HookLogImportant("FFX Hook: Installed re-arming VEH hook for %s!ffxConfigure at %p", ffx_hook_moduleName,
                         reinterpret_cast<void*>(target));
    }
    return true;
}

// Retroactive ffxConfigure call: when FSR FG activates through Streamline's
// authoritative takeover (no direct ffxConfigure intercepted), CE calls
// g_Original_ffxConfigure on all tracked FG contexts to install the present
// callback bridge.  This is necessary because ffxConfigure is called during
// AMD module init, before CE can intercept it.
inline bool InstallBridgeOnTrackedContextsImpl(void* swapChain) {
    (void)swapChain;
    HookLogImportant(
        "FFX Hook: Skipping retroactive present-callback bridge install because synthetic partial ffxConfigure "
        "packets are unsafe for the official FSR runtime; waiting for a real enabled ffxConfigure instead");
    return false;
}
