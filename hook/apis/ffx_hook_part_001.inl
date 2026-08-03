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
extern void DX12_ClearNativeFSRRuntimeOwnedTeardown(const char* reason);

// ============================================================================
// FFX API Type Definitions (from FFX SDK)
// We don't include the SDK headers directly to avoid dependency issues
// ============================================================================

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

// FFX Effect IDs - match values from ffx_api.h
constexpr uint32_t FFX_API_EFFECT_MASK = 0x00ff0000u;
constexpr uint32_t FFX_API_EFFECT_ID_FRAMEGENERATION = 0x00020000u;
constexpr uint32_t FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN = 0x00030000u;

// Return codes
constexpr ffxReturnCode_t FFX_API_RETURN_OK = 0;

// Function signatures
typedef ffxReturnCode_t (*PfnFfxCreateContext)(ffxContext* context, ffxCreateContextDescHeader* desc,
                                               const ffxAllocationCallbacks* memCb);
typedef ffxReturnCode_t (*PfnFfxDestroyContext)(ffxContext* context, const ffxAllocationCallbacks* memCb);
typedef ffxReturnCode_t (*PfnFfxConfigure)(ffxContext* context, const ffxConfigureDescHeader* desc);

// ============================================================================
// Hook State
// ============================================================================

namespace {

std::mutex g_InitMutex;
std::atomic<bool> g_Initialized{false};
std::atomic<int> g_FGContextCount{0};
std::atomic<bool> g_NoModulesLogged{false};
std::once_flag g_DynamicHookRegistrationOnce;

// CRITICAL FIX: Track context types to know if destroyed context is FG
std::mutex g_ContextMapMutex;
std::unordered_map<ffxContext, uint32_t> g_ContextTypeMap;
std::unordered_set<ffxContext> g_VulkanContextSet;
// Serializes post-provider transition publication with successful context destruction. The FFX provider may invoke
// configure from multiple threads; CE's global routing/session mutations must never overlap or race context erasure.
std::mutex g_FrameGenerationRoutingTransitionMutex;
struct FrameGenerationRoutingState {
    bool enabled = false;
    bool bridgeActive = false;
    bool appCallbackProvided = false;
};
std::unordered_map<ffxContext, FrameGenerationRoutingState> g_FrameGenerationRoutingByContext;
std::mutex g_PresentCallbackBridgeMutex;
std::unordered_set<void*> g_PresentCallbackBridgeKeys;

// Original function pointers
PfnFfxCreateContext g_Original_ffxCreateContext = nullptr;
PfnFfxDestroyContext g_Original_ffxDestroyContext = nullptr;
PfnFfxConfigure g_Original_ffxConfigure = nullptr;
thread_local PfnFfxConfigure t_FfxConfigureOriginalOverride = nullptr;

std::atomic<bool> g_ffxCreateContextInlineHooked{false};
std::atomic<bool> g_ffxDestroyContextInlineHooked{false};
std::atomic<bool> g_ffxConfigureInlineHooked{false};
std::atomic<void*> g_ffxCreateContextTarget{nullptr};
std::atomic<void*> g_ffxDestroyContextTarget{nullptr};
std::atomic<void*> g_ffxConfigureTarget{nullptr};
    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
std::recursive_mutex g_FfxConfigureBreakpointMutex;
static void* g_ffxConfigureVehHandle = nullptr;
static uint8_t g_ffxConfigureOriginalFirstByte = 0;
static std::atomic<bool> g_ffxConfigureVehArmed{false};
// One-shot VEH detection: after CE processes the first ENABLED no-callback ffxConfigure, the breakpoint is
// permanently disarmed (byte restored, no re-arm). Calls already routed through IAT/GetProcAddress or the
// cached-pointer router remain observable without touching AMD's code page. This eliminates the multi-threaded
// 0xCC contention that desyncs AMD's QPC-timed pacing (ffxQuery+0x225fe). GTA's ffxConfigure calls come from
// 6-7 threads; the 0xCC byte mutation
// on a code page those threads call into creates timing irregularities. The synthetic test app (single-
// threaded VEH hits) is stable; GTA (multi-threaded) wedges in ~8-10 frames. The latch resets on FG-off only
// when no durable caller-owned pointer route is available for the next on-transition.
static std::atomic<bool> g_ffxConfigureVehPermanentlyDisarmed{false};
// A stable client-owned ffxConfigure pointer route survives context destruction. While it is active, resetting
// FG state must not re-arm AMD's code-page breakpoint: the pointer route observes the next enable and every
// suspend/resume transition without executable-page mutation.
static std::atomic<bool> g_DurableCachedConfigureRouteActive{false};
static std::atomic<int> g_FfxConfigureOriginalForwardDepth{0};
static std::atomic<bool> g_FfxConfigureDeferredRearm{false};
static std::atomic<void*> g_FfxConfigureDeferredRearmTarget{nullptr};

static bool ArmFfxConfigureBreakpoint(PfnFfxConfigure target, const char* moduleName, const char* reason);
static void RestoreFfxConfigureBreakpointIfCurrent(void* target, const char* reason);
static ffxReturnCode_t CallFfxConfigureOriginalGuarded(PfnFfxConfigure originalConfigure, ffxContext* context,
                                                       const ffxConfigureDescHeader* desc);
static void ClearSubstituteUiReRegistrationForContext(ffxContext context);

bool IsCommittedReadableCodeAddress(void* address) {
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

template <typename T>
void RefreshDirectOriginalForModuleReload(T& original, T resolved, std::atomic<bool>& inlineHooked,
                                          std::atomic<void*>& targetSlot, const char* hookName,
                                          const char* moduleName) {
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
                         moduleName && moduleName[0] ? moduleName : "unknown");
    }

    original = resolved;
}

template <typename T>
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
HMODULE g_HookedModule = nullptr;
ce::ffx_api::PresentCallback g_DefaultPresentCallback = nullptr;

// Flag: VEH breakpoint hook installed for protected official AMD ffxConfigure
// (defined after Hooked_ffxConfigure below)
static bool g_ffxConfigureVehInstalled = false;
static bool InstallFfxConfigureBreakpointHook(PfnFfxConfigure target, const char* moduleName);

void* GetOrCreatePresentCallbackBridgeKey(ffxContext context) {
    if (!context) {
        return nullptr;
    }

    // This value is passed to the official FSR runtime as an opaque callback
    // user context. Keep it aligned and real; some runtime paths are stricter
    // than the SDK smoke tests and can reject or touch tagged pointers.
    void* key = reinterpret_cast<void*>(context);
    std::lock_guard<std::mutex> lock(g_PresentCallbackBridgeMutex);
    g_PresentCallbackBridgeKeys.insert(key);
    return key;
}

void* GetPresentCallbackBridgeKey(ffxContext context) {
    if (!context) {
        return nullptr;
    }

    return reinterpret_cast<void*>(context);
}

bool HasTrackedPresentCallbackBridgeKey(void* key) {
    if (!key) {
        return false;
    }

    std::lock_guard<std::mutex> lock(g_PresentCallbackBridgeMutex);
    return g_PresentCallbackBridgeKeys.find(key) != g_PresentCallbackBridgeKeys.end();
}

// Extract effect ID from structure type
inline uint32_t GetEffectId(ffxStructType_t type) {
    return static_cast<uint32_t>(type) & FFX_API_EFFECT_MASK;
}

// ============================================================================
// Hooked Functions
// ============================================================================

ffxReturnCode_t Hooked_ffxCreateContext(ffxContext* context, ffxCreateContextDescHeader* desc,
                                        const ffxAllocationCallbacks* memCb) {
    if (!g_Original_ffxCreateContext) {
        HookLog("FFX Hook: ffxCreateContext called but original not set!");
        return 1;  // Error
    }

    // Parse the swapchain creation descriptor before forwarding: the output pointer is populated by AMD, while
    // the exact game/presentation queue is an input. Direct proxy-backbuffer work is legal only on this queue.
    const auto parsedSwapChainCreate =
        ce::ffx_api::ParseFrameGenerationSwapChainCreateState(reinterpret_cast<const ce::ffx_api::ApiHeader*>(desc));
    const auto contextBackend =
        ce::ffx_api::ParseCreateContextBackend(reinterpret_cast<const ce::ffx_api::ApiHeader*>(desc));
    const bool duringStreamlineStartup = DXGIShared::IsStreamlineStartupTransitionWindowActive();

    // Call original first
    ffxReturnCode_t result = g_Original_ffxCreateContext(context, desc, memCb);

    if (result == FFX_API_RETURN_OK && desc) {
        uint32_t effectId = GetEffectId(desc->type);

        if (parsedSwapChainCreate.recognized && context && *context && parsedSwapChainCreate.swapChainOutput &&
            *parsedSwapChainCreate.swapChainOutput && parsedSwapChainCreate.gameQueue) {
            DX12_RegisterNativeFSRSwapchainPresentationQueue(
                *context, *parsedSwapChainCreate.swapChainOutput,
                static_cast<ID3D12CommandQueue*>(parsedSwapChainCreate.gameQueue));

            // ffxCreateContext has already returned the game-facing proxy and
            // its exact producer queue. Install the same module-validated
            // proxy Present prework used by ffxConfigure now, before the first
            // passthrough Present can race the later enabled configure. The
            // protected inner real swapchain remains fully quiesced; only the
            // proxy backbuffer receives CE work on the descriptor game queue.
            void* runtimeAnchor = g_ffxCreateContextTarget.load(std::memory_order_acquire);
            if (!runtimeAnchor) {
                runtimeAnchor = reinterpret_cast<void*>(g_Original_ffxCreateContext);
            }
            DX12_TryInstallFFXProxyPresentHook(*parsedSwapChainCreate.swapChainOutput, runtimeAnchor,
                                               "ffxCreateContext(FrameGenerationSwapChain)");
        }

        // Track successful context creation even during Streamline startup. This is passive bookkeeping only;
        // skipping it leaks the context count and queue binding when the matching destroy arrives later.
        bool newlyTrackedContext = false;
        {
            std::lock_guard<std::mutex> lock(g_ContextMapMutex);
            const auto [it, inserted] = g_ContextTypeMap.emplace(context ? *context : nullptr, effectId);
            newlyTrackedContext = inserted;
            if (!inserted) {
                it->second = effectId;
            }
            if (!ce::ffx_api::ShouldUseDX12FrameGenerationInterop(contextBackend)) {
                g_VulkanContextSet.insert(context ? *context : nullptr);
            } else {
                g_VulkanContextSet.erase(context ? *context : nullptr);
            }
        }

        // Check if this is a Frame Generation context
        if (newlyTrackedContext && !ce::ffx_api::ShouldUseDX12FrameGenerationInterop(contextBackend) &&
            ce::ffx_api::IsFrameGenerationEffectType(desc->type)) {
            HookLogImportant(
                "FFX Hook: Vulkan Frame Generation context CREATED; DXGI/DX12 interop intentionally bypassed "
                "(context=%p type=0x%llx effectId=0x%x)",
                context ? *context : nullptr, (unsigned long long)desc->type, effectId);
        } else if (newlyTrackedContext && (effectId == FFX_API_EFFECT_ID_FRAMEGENERATION ||
                                           effectId == FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN)) {
            int prevCount = g_FGContextCount.fetch_add(1, std::memory_order_acq_rel);
            HookLog(
                "FFX Hook: Frame Generation context CREATED (type=0x%llx, "
                "effectId=0x%x, liveContexts=%d, streamlineStartup=%d)",
                (unsigned long long)desc->type, effectId, prevCount + 1, duringStreamlineStartup ? 1 : 0);
            // Context creation proves support/lifetime only. Many titles pre-create the FSR contexts while FG
            // remains disabled; activation begins only after a successful enabled ffxConfigure/callback proof.
            (void)prevCount;
        }
    }

    return result;
}

ffxReturnCode_t Hooked_ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks* memCb) {
    if (!g_Original_ffxDestroyContext) {
        HookLog("FFX Hook: ffxDestroyContext called but original not set!");
        return 1;  // Error
    }

    const ffxContext contextHandle = context ? *context : nullptr;

    // Inspect before forwarding but commit no bookkeeping changes until AMD confirms destruction succeeded.
    // This preserves callback delegation and queue ownership if the provider rejects the destroy.
    bool isFGContext = false;
    bool isVulkanContext = false;
    bool isVulkanFGContext = false;
    {
        std::lock_guard<std::mutex> lock(g_ContextMapMutex);
        auto it = g_ContextTypeMap.find(contextHandle);
        if (it != g_ContextTypeMap.end()) {
            uint32_t effectId = it->second;
            isVulkanContext = g_VulkanContextSet.find(contextHandle) != g_VulkanContextSet.end();
            isVulkanFGContext = isVulkanContext && ce::ffx_api::IsFrameGenerationEffectType(effectId);
            isFGContext = !isVulkanContext && (effectId == FFX_API_EFFECT_ID_FRAMEGENERATION ||
                                               effectId == FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN);
        }
    }

    // Call original
    ffxReturnCode_t result = g_Original_ffxDestroyContext(context, memCb);

    std::unique_lock<std::mutex> transitionLock;
    if (result == FFX_API_RETURN_OK) {
        transitionLock = std::unique_lock<std::mutex>(g_FrameGenerationRoutingTransitionMutex);
        {
            std::lock_guard<std::mutex> lock(g_ContextMapMutex);
            g_ContextTypeMap.erase(contextHandle);
            g_VulkanContextSet.erase(contextHandle);
            g_FrameGenerationRoutingByContext.erase(contextHandle);
        }
        if (!isVulkanContext) {
            {
                std::lock_guard<std::mutex> lock(g_PresentCallbackBridgeMutex);
                g_PresentCallbackBridgeKeys.erase(contextHandle);
            }
            DX12_ClearFFXPresentCallbackBridge(contextHandle);
            DX12_UnregisterNativeFSRSwapchainPresentationQueue(contextHandle, "FFX swapchain context destroyed");
            ClearSubstituteUiReRegistrationForContext(contextHandle);
        }
    }

    // Only decrement if this was actually an FG context
    if (result == FFX_API_RETURN_OK && isFGContext) {
        int newCount = g_FGContextCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (newCount < 0) {
            g_FGContextCount.store(0, std::memory_order_release);
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
                                        "FFXHook::Hooked_ffxDestroyContext", reinterpret_cast<void*>(context), nullptr,
                                        ce::fg_runtime::RuntimeMode::kOff, false, true);
        }
    } else if (result == FFX_API_RETURN_OK && isVulkanFGContext) {
        HookLogImportant(
            "FFX Hook: Vulkan Frame Generation context destroyed; DXGI/DX12 teardown intentionally bypassed "
            "(context=%p)",
            contextHandle);
    } else if (result == FFX_API_RETURN_OK && !isFGContext) {
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
static std::mutex g_SubstReRegMutex;
static ffxContext g_SubstReRegContext = nullptr;
static PfnFfxConfigure g_SubstReRegConfigure = nullptr;
static ce::ffx_api::ConfigureDescFrameGenerationSwapChainRegisterUiResource g_SubstReRegDesc = {};
static std::atomic<bool> g_SubstReRegActive{false};

static void ClearSubstituteUiReRegistrationForContext(ffxContext context) {
    if (!context) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_SubstReRegMutex);
    if (g_SubstReRegContext != context) {
        return;
    }
    g_SubstReRegActive.store(false, std::memory_order_release);
    g_SubstReRegContext = nullptr;
    g_SubstReRegConfigure = nullptr;
    g_SubstReRegDesc = {};
    HookLogImportant("FFX Hook: Cleared substitute UI re-registration for destroyed context %p", context);
}

static void StoreSubstituteUiReRegistration(
    ffxContext* context, PfnFfxConfigure originalConfigure,
    const ce::ffx_api::ConfigureDescFrameGenerationSwapChainRegisterUiResource& substitutedDesc) {
    std::lock_guard<std::mutex> lock(g_SubstReRegMutex);
    g_SubstReRegContext = context ? *context : nullptr;
    g_SubstReRegConfigure = originalConfigure;
    g_SubstReRegDesc = substitutedDesc;
    g_SubstReRegActive.store(true, std::memory_order_release);
}

ffxReturnCode_t Hooked_ffxConfigure(ffxContext* context, const ffxConfigureDescHeader* desc) {
    PfnFfxConfigure originalConfigure =
        t_FfxConfigureOriginalOverride ? t_FfxConfigureOriginalOverride : g_Original_ffxConfigure;
    if (!originalConfigure) {
        HookLog("FFX Hook: ffxConfigure called but original not set!");
        return 1;  // Error
    }
    const ffxContext contextHandle = context ? *context : nullptr;

    bool isVulkanContext = false;
    {
        std::lock_guard<std::mutex> lock(g_ContextMapMutex);
        isVulkanContext = g_VulkanContextSet.find(contextHandle) != g_VulkanContextSet.end();
    }
    if (isVulkanContext) {
        const ffxReturnCode_t result = CallFfxConfigureOriginalGuarded(originalConfigure, context, desc);
        const auto parsed =
            ce::ffx_api::ParseFrameGenerationConfigureState(reinterpret_cast<const ce::ffx_api::ApiHeader*>(desc));
        static std::atomic<int> s_vulkanConfigureBypassLogCount{0};
        const int logCount = s_vulkanConfigureBypassLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (logCount <= 20 || (logCount % 600) == 0) {
            HookLogImportant(
                "FFX Hook: Vulkan ffxConfigure forwarded without DXGI/DX12 interop "
                "(context=%p type=0x%llx frameGeneration=%d enabled=%d frameID=%llu result=%u log=%d)",
                contextHandle, static_cast<unsigned long long>(desc ? desc->type : 0), parsed.recognized ? 1 : 0,
                parsed.enabled ? 1 : 0, static_cast<unsigned long long>(parsed.frameId), result, logCount);
        }
        return result;
    }

    // During the Streamline startup window, skip CE-side processing to avoid
    // accessing DX12 swapchain state (HDR, callback bridges) while SL's
    // critical initialization is still in progress.  Just forward the call.
    if (DXGIShared::IsStreamlineStartupTransitionWindowActive()) {
        return CallFfxConfigureOriginalGuarded(originalConfigure, context, desc);
    }

    ce::ffx_api::ConfigureDescFrameGeneration localConfig = {};
    // Backing storage for a substituted RegisterUiResource forward (CE swaps in its own full-size UI texture
    // when the game registers a degenerate 1x1 placeholder). Function-scoped so it outlives the synchronous
    // forward at CallFfxConfigureOriginalGuarded below.
    ce::ffx_api::ConfigureDescFrameGenerationSwapChainRegisterUiResource localUiConfig = {};
    DX12FFXUiOverlayTargetPreparation uiTargetPreparation = {};
    bool uiTargetPrepared = false;
    bool uiTargetSubstituted = false;
    const ffxConfigureDescHeader* descToCall = desc;
    const auto* parsedDesc = reinterpret_cast<const ce::ffx_api::ApiHeader*>(desc);
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
        localConfig = *reinterpret_cast<const ce::ffx_api::ConfigureDescFrameGeneration*>(desc);
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
                    localConfig.presentCallback ? localConfig.presentCallback : g_DefaultPresentCallback;
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
         !g_ffxConfigureVehPermanentlyDisarmed.load(std::memory_order_acquire))) {
        const auto* uiDesc =
            reinterpret_cast<const ce::ffx_api::ConfigureDescFrameGenerationSwapChainRegisterUiResource*>(desc);
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
