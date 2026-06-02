#include "ffx_hook.h"
#include <psapi.h>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include "../common/dx12_overlay_policy.h"
#include "../common/dxgi_shared.h"
#include "../common/ffx_api_parsing.h"
#include "../common/fg_detection.h"
#include "../common/fg_session_state.h"
#include "../common/hook_common.h"
#include "../common/overlay_compat.h"
#include "../wrappers/iat_hook.h"
#include "../wrappers/inline_hook.h"
#include "dx12_hook.h"
#include "ffx_hook.h"

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
std::atomic<int> g_ActiveFGContextCount{0};
std::atomic<bool> g_NoModulesLogged{false};
std::once_flag g_DynamicHookRegistrationOnce;

// CRITICAL FIX: Track context types to know if destroyed context is FG
std::mutex g_ContextMapMutex;
std::unordered_map<ffxContext, uint32_t> g_ContextTypeMap;
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
std::recursive_mutex g_FfxConfigureBreakpointMutex;
static void* g_ffxConfigureVehHandle = nullptr;
static uint8_t g_ffxConfigureOriginalFirstByte = 0;
static std::atomic<bool> g_ffxConfigureVehArmed{false};
static std::atomic<int> g_FfxConfigureOriginalForwardDepth{0};
static std::atomic<bool> g_FfxConfigureDeferredRearm{false};
static std::atomic<void*> g_FfxConfigureDeferredRearmTarget{nullptr};

static bool ArmFfxConfigureBreakpoint(PfnFfxConfigure target, const char* moduleName, const char* reason);
static ffxReturnCode_t CallFfxConfigureOriginalGuarded(PfnFfxConfigure originalConfigure, ffxContext* context,
                                                       const ffxConfigureDescHeader* desc);

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

    // During the Streamline startup window, skip CE-side processing to avoid
    // interfering with SL's critical initialization (creating a temporary
    // COMPUTE queue or accessing SL state during DllMain can crash SL with
    // a null pointer call).
    if (DXGIShared::IsStreamlineStartupTransitionWindowActive()) {
        return g_Original_ffxCreateContext(context, desc, memCb);
    }

    // Call original first
    ffxReturnCode_t result = g_Original_ffxCreateContext(context, desc, memCb);

    if (result == FFX_API_RETURN_OK && desc) {
        uint32_t effectId = GetEffectId(desc->type);

        // CRITICAL FIX: Track context type for proper cleanup
        {
            std::lock_guard<std::mutex> lock(g_ContextMapMutex);
            g_ContextTypeMap[context] = effectId;
        }

        // Check if this is a Frame Generation context
        if (effectId == FFX_API_EFFECT_ID_FRAMEGENERATION || effectId == FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN) {
            int prevCount = g_ActiveFGContextCount.fetch_add(1, std::memory_order_acq_rel);
            HookLog(
                "FFX Hook: Frame Generation context CREATED (type=0x%llx, "
                "effectId=0x%x, activeContexts=%d)",
                (unsigned long long)desc->type, effectId, prevCount + 1);

            // Signal FG activation to the detection system
            // Only on first context (0 -> 1 transition)
            if (prevCount == 0) {
                HookLog("FFX Hook: FSR Frame Generation ACTIVATED (first context created)");
                g_FGCompat.SetFSRFGActive(true);
            }
        }
    }

    return result;
}

ffxReturnCode_t Hooked_ffxDestroyContext(ffxContext* context, const ffxAllocationCallbacks* memCb) {
    if (!g_Original_ffxDestroyContext) {
        HookLog("FFX Hook: ffxDestroyContext called but original not set!");
        return 1;  // Error
    }

    // During the Streamline startup window, skip CE-side processing (same as
    // ffxCreateContext guard) to avoid interfering with SL's initialization.
    if (DXGIShared::IsStreamlineStartupTransitionWindowActive()) {
        return g_Original_ffxDestroyContext(context, memCb);
    }

    // CRITICAL FIX: Check if this is an FG context before decrementing
    bool isFGContext = false;
    {
        std::lock_guard<std::mutex> lock(g_ContextMapMutex);
        auto it = g_ContextTypeMap.find(context);
        if (it != g_ContextTypeMap.end()) {
            uint32_t effectId = it->second;
            isFGContext = (effectId == FFX_API_EFFECT_ID_FRAMEGENERATION ||
                           effectId == FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN);
            g_ContextTypeMap.erase(it);
        }
    }

    {
        std::lock_guard<std::mutex> lock(g_PresentCallbackBridgeMutex);
        g_PresentCallbackBridgeKeys.erase(reinterpret_cast<void*>(context));
    }
    DX12_ClearFFXPresentCallbackBridge(reinterpret_cast<void*>(context));

    // Call original
    ffxReturnCode_t result = g_Original_ffxDestroyContext(context, memCb);

    // Only decrement if this was actually an FG context
    if (result == FFX_API_RETURN_OK && isFGContext) {
        int newCount = g_ActiveFGContextCount.fetch_sub(1, std::memory_order_acq_rel) - 1;
        if (newCount < 0) {
            g_ActiveFGContextCount.store(0, std::memory_order_release);
            newCount = 0;
        }

        HookLog("FFX Hook: FG Context destroyed (activeContexts=%d)", newCount);

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
                                        "FFXHook::Hooked_ffxDestroyContext", context, nullptr,
                                        ce::fg_runtime::RuntimeMode::kOff, false, true);
        }
    } else if (result == FFX_API_RETURN_OK && !isFGContext) {
        HookLog("FFX Hook: Non-FG Context destroyed");
    }

    return result;
}

ffxReturnCode_t Hooked_ffxConfigure(ffxContext* context, const ffxConfigureDescHeader* desc) {
    PfnFfxConfigure originalConfigure =
        t_FfxConfigureOriginalOverride ? t_FfxConfigureOriginalOverride : g_Original_ffxConfigure;
    if (!originalConfigure) {
        HookLog("FFX Hook: ffxConfigure called but original not set!");
        return 1;  // Error
    }

    // During the Streamline startup window, skip CE-side processing to avoid
    // accessing DX12 swapchain state (HDR, callback bridges) while SL's
    // critical initialization is still in progress.  Just forward the call.
    if (DXGIShared::IsStreamlineStartupTransitionWindowActive()) {
        return CallFfxConfigureOriginalGuarded(originalConfigure, context, desc);
    }

    ce::ffx_api::ConfigureDescFrameGeneration localConfig = {};
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
            bridgeKey = GetOrCreatePresentCallbackBridgeKey(context);
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
            bridgeKey = GetPresentCallbackBridgeKey(context);
            if (localConfig.frameGenerationEnabled && !appPresentCallbackProvided) {
                DX12_ClearFFXPresentCallbackBridge(bridgeKey);
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

    const ffxReturnCode_t result = CallFfxConfigureOriginalGuarded(originalConfigure, context, descToCall);
    if (result != FFX_API_RETURN_OK || !desc) {
        return result;
    }

    const auto parsed =
        ce::ffx_api::ParseFrameGenerationConfigureState(reinterpret_cast<const ce::ffx_api::ApiHeader*>(desc));
    if (!parsed.recognized) {
        return result;
    }

    if (installedPresentCallbackBridge) {
        static std::atomic<int> s_installedPresentCallbackBridgeLogCount{0};
        const int logCount = s_installedPresentCallbackBridgeLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0 || disabledStartupArmingConfigure) {
            const auto* originalDesc = reinterpret_cast<const ce::ffx_api::ConfigureDescFrameGeneration*>(desc);
            HookLogImportant(
                "FFX Hook: Installed DX12 overlay present-callback bridge for context=%p frameID=%llu enabled=%d "
                "startupArming=%d originalPresent=%p resolvedPresent=%p usedDefaultPresent=%d log=%d",
                context, static_cast<unsigned long long>(originalDesc->frameID),
                originalDesc->frameGenerationEnabled ? 1 : 0, disabledStartupArmingConfigure ? 1 : 0,
                reinterpret_cast<void*>(originalDesc->presentCallback),
                reinterpret_cast<void*>(bridgedOriginalCallback), usingDefaultPresentCallback ? 1 : 0, logCount + 1);
        }
    } else if (retainedAlreadyBridgedPresentCallback) {
        static std::atomic<int> s_retainedAlreadyBridgedPresentCallbackLogCount{0};
        const int logCount = s_retainedAlreadyBridgedPresentCallbackLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            const auto* originalDesc = reinterpret_cast<const ce::ffx_api::ConfigureDescFrameGeneration*>(desc);
            HookLogImportant(
                "FFX Hook: Retained existing DX12 overlay present-callback bridge for already-bridged configure "
                "(context=%p frameID=%llu enabled=%d startupArming=%d originalPresent=%p originalUserCtx=%p log=%d)",
                context, static_cast<unsigned long long>(originalDesc->frameID),
                originalDesc->frameGenerationEnabled ? 1 : 0, disabledStartupArmingConfigure ? 1 : 0,
                reinterpret_cast<void*>(originalDesc->presentCallback), originalDesc->presentCallbackUserContext,
                logCount + 1);
        }
    } else if (retainedBridgeForDisabledConfigure) {
        static std::atomic<int> s_retainedDisabledPresentCallbackBridgeLogCount{0};
        const int logCount =
            s_retainedDisabledPresentCallbackBridgeLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            const auto* originalDesc = reinterpret_cast<const ce::ffx_api::ConfigureDescFrameGeneration*>(desc);
            HookLogImportant(
                "FFX Hook: Retained existing DX12 overlay present-callback bridge for disabled native-FSR configure "
                "(context=%p frameID=%llu originalPresent=%p bridgeUserCtx=%p log=%d)",
                context, static_cast<unsigned long long>(originalDesc->frameID),
                reinterpret_cast<void*>(originalDesc->presentCallback), localConfig.presentCallbackUserContext,
                logCount + 1);
        }
    } else if (disabledStartupArmingConfigure) {
        static std::atomic<int> s_disabledStartupArmingNoBridgeLogCount{0};
        const int logCount = s_disabledStartupArmingNoBridgeLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            const auto* originalDesc = reinterpret_cast<const ce::ffx_api::ConfigureDescFrameGeneration*>(desc);
            HookLogImportant(
                "FFX Hook: Native FSR disabled startup-arming configure forwarded without CE present-callback bridge "
                "(context=%p frameID=%llu retainedBridge=%d originalPresent=%p log=%d)",
                context, static_cast<unsigned long long>(originalDesc->frameID),
                retainedExistingBridgeForDisabledConfigure ? 1 : 0,
                reinterpret_cast<void*>(originalDesc->presentCallback), logCount + 1);
        }
    } else if (recognizedFGConfigure && parsed.enabled && !appPresentCallbackProvided) {
        static std::atomic<int> s_enabledNoPresentCallbackLogCount{0};
        const int logCount = s_enabledNoPresentCallbackLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            const auto* originalDesc = reinterpret_cast<const ce::ffx_api::ConfigureDescFrameGeneration*>(desc);
            HookLogImportant(
                "FFX Hook: Native FSR enabled with no app present callback; preserving AMD internal "
                "no-callback composition and using normal DX12 overlay route "
                "(context=%p frameID=%llu originalPresent=%p log=%d)",
                context, static_cast<unsigned long long>(originalDesc->frameID),
                reinterpret_cast<void*>(originalDesc->presentCallback), logCount + 1);
        }
    } else if (recognizedFGConfigure) {
        static std::atomic<int> s_configureNoBridgeLogCount{0};
        const int logCount = s_configureNoBridgeLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            const auto* originalDesc = reinterpret_cast<const ce::ffx_api::ConfigureDescFrameGeneration*>(desc);
            HookLogImportant(
                "FFX Hook: Native FSR configure without DX12 present-callback bridge "
                "(context=%p frameID=%llu enabled=%d retainedBridge=%d originalPresent=%p log=%d)",
                context, static_cast<unsigned long long>(originalDesc->frameID),
                originalDesc->frameGenerationEnabled ? 1 : 0,
                retainedExistingBridgeForDisabledConfigure ? 1 : 0,
                reinterpret_cast<void*>(originalDesc->presentCallback), logCount + 1);
        }
    }

    HookLog("FFX Hook: Frame Generation configure %s (context=%p, frameID=%llu, type=0x%llx)",
            parsed.enabled ? "ENABLED" : "DISABLED", context, (unsigned long long)parsed.frameId,
            (unsigned long long)desc->type);

    if (!parsed.enabled && disabledStartupArmingConfigure) {
        static std::atomic<int> s_disabledStartupArmingPreserveLogCount{0};
        const int logCount = s_disabledStartupArmingPreserveLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "FFX Hook: Native FSR disabled configure used for startup arming; preserving authoritative FSR state "
                "until direct enabled configure arrives (context=%p frameID=%llu runtimeOwned=%d directFFX=%d log=%d)",
                context, static_cast<unsigned long long>(parsed.frameId),
                DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration() ? 1 : 0,
                g_FGCompat.HasDirectFFXApiConfirmation() ? 1 : 0, logCount + 1);
        }
        return result;
    }

    // Native FSR can keep its context alive while toggling FG on/off via
    // ffxConfigure. Trust that runtime signal over context lifetime.
    if (parsed.enabled) {
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
                context, static_cast<unsigned long long>(parsed.frameId));
        }
    }
    const bool retainedBridgeForConfigure = !parsed.enabled && (retainedExistingBridgeForDisabledConfigure ||
                                                                retainedAlreadyBridgedPresentCallback ||
                                                                retainedBridgeForDisabledConfigure);
    DX12_OnNativeFSRPresentCallbackRoutingConfigured(
        parsed.enabled,
        installedPresentCallbackBridge || retainedAlreadyBridgedPresentCallback || retainedBridgeForDisabledConfigure,
        appPresentCallbackProvided);
    DX12_OnNativeFSRFrameGenerationConfigured(parsed.enabled, retainedBridgeForConfigure);
    g_FGCompat.SetFSRFGActive(parsed.enabled);
    ce::fg_session::EmitFGEvent(
        parsed.enabled ? ce::fg_session::FGEventKind::kNativeFSRConfigureOn
                       : ce::fg_session::FGEventKind::kNativeFSRConfigureOff,
        "FFXHook::Hooked_ffxConfigure", context, nullptr,
        parsed.enabled ? ce::fg_runtime::RuntimeMode::kFSRFG : ce::fg_runtime::RuntimeMode::kOff, parsed.enabled, true);
    return result;
}

// ============================================================================
// Hook Installation via GetProcAddress Detour
// ============================================================================

bool IsFFXDynamicHookOwnerModule(const char* moduleBaseName, HMODULE module) {
    if (moduleBaseName && ce::overlay_compat::IsFFXFrameGenerationModulePath(moduleBaseName)) {
        return true;
    }

    char modulePath[MAX_PATH] = {};
    if (module && GetModuleFileNameA(module, modulePath, sizeof(modulePath))) {
        return ce::overlay_compat::IsFFXFrameGenerationModulePath(modulePath);
    }

    return false;
}

void RegisterDynamicHooksOnce() {
    std::call_once(g_DynamicHookRegistrationOnce, [] {
        IATHook::RegisterDynamicHookFiltered("ffxCreateContext", reinterpret_cast<void*>(Hooked_ffxCreateContext),
                                             reinterpret_cast<void**>(&g_Original_ffxCreateContext),
                                             IsFFXDynamicHookOwnerModule);
        IATHook::RegisterDynamicHookFiltered("ffxDestroyContext", reinterpret_cast<void*>(Hooked_ffxDestroyContext),
                                             reinterpret_cast<void**>(&g_Original_ffxDestroyContext),
                                             IsFFXDynamicHookOwnerModule);
        IATHook::RegisterDynamicHookFiltered("ffxConfigure", reinterpret_cast<void*>(Hooked_ffxConfigure),
                                             reinterpret_cast<void**>(&g_Original_ffxConfigure),
                                             IsFFXDynamicHookOwnerModule);
        HookLogImportant("FFX Hook: Registered module-filtered dynamic hooks for FFX exports");
    });
}

bool InstallHooksForModule(HMODULE hModule, const char* moduleName) {
    if (!hModule)
        return false;

    const bool firstSeenModule = g_HookedModule != hModule;
    if (firstSeenModule) {
        HookLog("FFX Hook: Installing hooks for module %s (%p)", moduleName, hModule);
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
            HookLog("FFX Hook: No supported FFX exports found in %s - skipping (log=%d)", moduleName, logCount);
        }
        return false;
    }

    const bool allowInlineHooks = ce::ffx_api::ShouldInlineHookFFXExportsForModule(moduleName);
    const bool allowIATHooks = ce::ffx_api::ShouldPatchFFXImportsForModule(moduleName);

    const auto resolvedDefaultPresentCallback =
        reinterpret_cast<ce::ffx_api::PresentCallback>(GetProcAddress(hModule, "ffxFrameInterpolationUiComposition"));
    if (resolvedDefaultPresentCallback && resolvedDefaultPresentCallback != g_DefaultPresentCallback) {
        g_DefaultPresentCallback = resolvedDefaultPresentCallback;
        if (g_DefaultPresentCallback) {
            HookLogImportant("FFX Hook: Resolved default frame-interpolation present callback at %p",
                             reinterpret_cast<void*>(g_DefaultPresentCallback));
        }
    }

    // Direct-export originals must follow protected/dynamic FFX module reloads.
    // GTA can unload amd_fidelityfx_dx12.dll and map it again at a new base when
    // a save enables native FSR; keeping the old ffxConfigure pointer turns the
    // next VEH hook call into a DEP fault on the unmapped image.
    RefreshDirectOriginalForModuleReload(g_Original_ffxCreateContext, createCtx, g_ffxCreateContextInlineHooked,
                                         g_ffxCreateContextTarget, "ffxCreateContext", moduleName);
    RefreshDirectOriginalForModuleReload(g_Original_ffxDestroyContext, destroyCtx, g_ffxDestroyContextInlineHooked,
                                         g_ffxDestroyContextTarget, "ffxDestroyContext", moduleName);
    RefreshDirectOriginalForModuleReload(g_Original_ffxConfigure, configureCtx, g_ffxConfigureInlineHooked,
                                         g_ffxConfigureTarget, "ffxConfigure", moduleName);
    g_HookedModule = hModule;

    RegisterDynamicHooksOnce();

    const bool armProtectedConfigureBreakpoint =
        !allowInlineHooks && ce::ffx_api::ShouldArmProtectedOfficialFFXConfigureBreakpoint(moduleName);

    if (!allowInlineHooks && allowIATHooks && ce::ffx_api::IsOfficialAMDFFXRuntimeModuleName(moduleName) &&
        firstSeenModule) {
        HookLogImportant(
            "FFX Hook: Using IAT/dynamic hooks for protected official FFX module %s; inline export JMP patching "
            "skipped; guarded ffxConfigure VEH fallback %s for SDK dispatch-table/intra-module calls",
            moduleName, armProtectedConfigureBreakpoint ? "enabled" : "not eligible");
    } else if (!allowInlineHooks && !allowIATHooks && firstSeenModule) {
        HookLogImportant(
            "FFX Hook: Using GetProcAddress-only hooks for protected official FFX module %s; inline export patching "
            "and IAT import patching skipped to avoid startup fail-fast; code bytes left unmodified; waiting for "
            "a real ffxConfigure call to arm the native FSR present-callback bridge",
            moduleName);
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
                                      reinterpret_cast<void*>(Hooked_ffxCreateContext), g_Original_ffxCreateContext,
                                      g_ffxCreateContextInlineHooked, g_ffxCreateContextTarget, "ffxCreateContext");
        }
        HookLog("FFX Hook: ffxCreateContext found at %p, hooking via %s (inline=%d)", createCtx,
                allowIATHooks ? "IAT/dynamic" : "dynamic-only", allowInlineHooks ? 1 : 0);
        if (allowIATHooks) {
            iatPatchedAnything |=
                IATHook::PatchIATAllModules(moduleName, "ffxCreateContext", (void*)Hooked_ffxCreateContext, &dummy);
        }
    }

    if (destroyCtx) {
        routedAnything = true;
        if (allowInlineHooks) {
            inlineHookedAnything |=
                InstallInlineHookOnce(reinterpret_cast<void*>(destroyCtx),
                                      reinterpret_cast<void*>(Hooked_ffxDestroyContext), g_Original_ffxDestroyContext,
                                      g_ffxDestroyContextInlineHooked, g_ffxDestroyContextTarget, "ffxDestroyContext");
        }
        HookLog("FFX Hook: ffxDestroyContext found at %p, hooking via %s (inline=%d)", destroyCtx,
                allowIATHooks ? "IAT/dynamic" : "dynamic-only", allowInlineHooks ? 1 : 0);
        if (allowIATHooks) {
            iatPatchedAnything |=
                IATHook::PatchIATAllModules(moduleName, "ffxDestroyContext", (void*)Hooked_ffxDestroyContext, &dummy);
        }
    }

    if (configureCtx) {
        routedAnything = true;
        if (allowInlineHooks) {
            inlineHookedAnything |= InstallInlineHookOnce(
                reinterpret_cast<void*>(configureCtx), reinterpret_cast<void*>(Hooked_ffxConfigure),
                g_Original_ffxConfigure, g_ffxConfigureInlineHooked, g_ffxConfigureTarget, "ffxConfigure");
        } else if (!allowIATHooks) {
            static std::atomic<int> s_protectedConfigureUnpatchedLogCount{0};
            const int logCount = s_protectedConfigureUnpatchedLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logCount <= 20 || (logCount % 300) == 0) {
                HookLogImportant(
                    "FFX Hook: Protected official FFX ffxConfigure export left unpatched "
                    "(module=%s target=%p log=%d); relying on GetProcAddress-visible API routing",
                    moduleName ? moduleName : "FFX", reinterpret_cast<void*>(configureCtx), logCount);
            }
        }
        HookLog("FFX Hook: ffxConfigure found at %p, hooking via %s (inline=%d veh=%d)", configureCtx,
                allowIATHooks ? (armProtectedConfigureBreakpoint ? "IAT/dynamic+VEH" : "IAT/dynamic")
                              : (armProtectedConfigureBreakpoint ? "dynamic+VEH" : "dynamic-only"),
                allowInlineHooks ? 1 : 0, armProtectedConfigureBreakpoint ? 1 : 0);
        if (allowIATHooks) {
            iatPatchedAnything |=
                IATHook::PatchIATAllModules(moduleName, "ffxConfigure", (void*)Hooked_ffxConfigure, &dummy);
        }
        if (armProtectedConfigureBreakpoint) {
            // Protected official AMD module: install a re-arming VEH hook to
            // intercept SDK dispatch-table or intra-module ffxConfigure calls
            // that bypass GetProcAddress and caller import thunks. The handler
            // restores the byte before forwarding to the real function, then
            // re-arms after the call returns.
            vehHookedAnything |= InstallFfxConfigureBreakpointHook(configureCtx, moduleName);
            if (!vehHookedAnything) {
                static std::atomic<int> s_protectedConfigureVehFailureLogCount{0};
                const int logCount =
                    s_protectedConfigureVehFailureLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
                if (logCount <= 20 || (logCount % 300) == 0) {
                    HookLogImportant(
                        "FFX Hook: Failed to arm guarded ffxConfigure VEH fallback "
                        "(module=%s target=%p log=%d); relying on IAT/dynamic routing only",
                        moduleName ? moduleName : "FFX", reinterpret_cast<void*>(configureCtx), logCount);
                }
            }
        }
    }

    if (routedAnything) {
        static std::atomic<int> s_hooksInstalledLogCount{0};
        const int logCount = s_hooksInstalledLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (firstSeenModule || logCount <= 20 || (logCount % 300) == 0) {
            HookLog("FFX Hook: Hooks installed successfully for %s (inline=%d iat=%d veh=%d dynamic=1 protected=%d "
                    "log=%d)",
                    moduleName, inlineHookedAnything ? 1 : 0, iatPatchedAnything ? 1 : 0,
                    vehHookedAnything ? 1 : 0,
                    !allowInlineHooks ? 1 : 0, logCount);
        }
    }
    return true;
}

// ============================================================================
// VEH breakpoint hook for ffxConfigure on protected official AMD modules.
// The official AMD runtime (amd_fidelityfx_dx12.dll) fails fast (0xC0000409)
// when CE installs a standard inline hook on ffxExport.  Instead, we patch
// the first byte with 0xCC (int3) and catch it in a VEH handler.  This
// bypasses CFG validation because int3 is a breakpoint, not an indirect call.
// The handler restores the byte, invokes Hooked_ffxConfigure to install the
// present callback bridge, then re-arms the breakpoint after the real call
// returns so later save-load enable packets are still visible.
// ============================================================================

static void RestoreFfxConfigureBreakpointIfCurrent(void* target, const char* reason) {
    std::lock_guard<std::recursive_mutex> lock(g_FfxConfigureBreakpointMutex);
    if (!target || !g_ffxConfigureVehArmed.load(std::memory_order_acquire) ||
        g_ffxConfigureTarget.load(std::memory_order_acquire) != target) {
        return;
    }

    if (!IsCommittedReadableCodeAddress(target)) {
        g_ffxConfigureVehArmed.store(false, std::memory_order_release);
        HookLogImportant("FFX Hook: Dropping stale VEH breakpoint state for unloaded ffxConfigure target %p (%s)",
                         target, reason && reason[0] ? reason : "target changed");
        return;
    }

    auto* targetByte = static_cast<uint8_t*>(target);
    if (*targetByte != 0xCC) {
        g_ffxConfigureVehArmed.store(false, std::memory_order_release);
        return;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        HookLogImportant("FFX Hook: Failed to restore stale VEH breakpoint at %p before retargeting (err=%lu)", target,
                         GetLastError());
        return;
    }
    *targetByte = g_ffxConfigureOriginalFirstByte;
    FlushInstructionCache(GetCurrentProcess(), targetByte, 1);
    VirtualProtect(target, 1, oldProtect, &oldProtect);
    g_ffxConfigureVehArmed.store(false, std::memory_order_release);
    HookLogImportant("FFX Hook: Restored stale VEH breakpoint at %p before retargeting (%s)", target,
                     reason && reason[0] ? reason : "target changed");
}

static bool ArmFfxConfigureBreakpoint(PfnFfxConfigure target, const char* moduleName, const char* reason) {
    std::lock_guard<std::recursive_mutex> lock(g_FfxConfigureBreakpointMutex);
    if (!target) {
        return false;
    }

    if (g_FfxConfigureOriginalForwardDepth.load(std::memory_order_acquire) > 0) {
        g_FfxConfigureDeferredRearmTarget.store(reinterpret_cast<void*>(target), std::memory_order_release);
        g_FfxConfigureDeferredRearm.store(true, std::memory_order_release);
        if (!g_ffxConfigureInlineHooked.load(std::memory_order_acquire) && g_Original_ffxConfigure != target) {
            HookLogImportant(
                "FFX Hook: Updating protected ffxConfigure original while deferring VEH arm "
                "(old=%p new=%p module=%s reason=%s)",
                reinterpret_cast<void*>(g_Original_ffxConfigure), reinterpret_cast<void*>(target),
                moduleName ? moduleName : "FFX", reason ? reason : "unknown");
            g_Original_ffxConfigure = target;
        }
        static std::atomic<int> s_deferredArmLogCount{0};
        const int logCount = s_deferredArmLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (logCount <= 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "FFX Hook: Deferring ffxConfigure VEH breakpoint arm while original forwarding is active "
                "(target=%p module=%s reason=%s depth=%d log=%d)",
                reinterpret_cast<void*>(target), moduleName ? moduleName : "FFX", reason ? reason : "unknown",
                g_FfxConfigureOriginalForwardDepth.load(std::memory_order_acquire), logCount);
        }
        return true;
    }

    if (!IsCommittedReadableCodeAddress(reinterpret_cast<void*>(target))) {
        HookLogImportant("FFX Hook: Refusing to arm VEH breakpoint for unreadable ffxConfigure target %p (%s)",
                         reinterpret_cast<void*>(target), moduleName ? moduleName : "FFX");
        return false;
    }

    void* previousTarget = g_ffxConfigureTarget.load(std::memory_order_acquire);
    if (previousTarget && previousTarget != reinterpret_cast<void*>(target)) {
        RestoreFfxConfigureBreakpointIfCurrent(previousTarget, "ffxConfigure target changed");
    }

    auto* targetByte = reinterpret_cast<uint8_t*>(reinterpret_cast<LPVOID>(target));
    const uint8_t currentByte = *targetByte;
    const bool alreadyArmed = g_ffxConfigureVehArmed.load(std::memory_order_acquire) &&
                              g_ffxConfigureTarget.load(std::memory_order_acquire) == reinterpret_cast<void*>(target) &&
                              currentByte == 0xCC;
    if (alreadyArmed) {
        return true;
    }

    if (currentByte != 0xCC) {
        g_ffxConfigureOriginalFirstByte = currentByte;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(target), 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return false;
    }
    *targetByte = 0xCC;
    FlushInstructionCache(GetCurrentProcess(), targetByte, 1);
    VirtualProtect(reinterpret_cast<LPVOID>(target), 1, oldProtect, &oldProtect);

    g_ffxConfigureTarget.store(reinterpret_cast<void*>(target), std::memory_order_release);
    g_ffxConfigureVehArmed.store(true, std::memory_order_release);
    if (!g_ffxConfigureInlineHooked.load(std::memory_order_acquire) && g_Original_ffxConfigure != target) {
        HookLogImportant("FFX Hook: Updating protected ffxConfigure original for VEH target (old=%p new=%p)",
                         reinterpret_cast<void*>(g_Original_ffxConfigure), reinterpret_cast<void*>(target));
        g_Original_ffxConfigure = target;
    }

    const bool postCallRearm = reason && std::strcmp(reason, "post-call rearm") == 0;
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
                             currentByte == 0xCC ? "Confirmed" : "Armed", moduleName ? moduleName : "FFX",
                             reinterpret_cast<void*>(target), reason, rearmLogCount);
        } else {
            HookLogImportant("FFX Hook: %s VEH breakpoint for %s!ffxConfigure at %p (%s)",
                             currentByte == 0xCC ? "Confirmed" : "Armed", moduleName ? moduleName : "FFX",
                             reinterpret_cast<void*>(target), reason ? reason : "init");
        }
    }
    return true;
}

static ffxReturnCode_t CallFfxConfigureOriginalGuarded(PfnFfxConfigure originalConfigure, ffxContext* context,
                                                       const ffxConfigureDescHeader* desc) {
    if (!originalConfigure) {
        return 1;
    }

    g_FfxConfigureOriginalForwardDepth.fetch_add(1, std::memory_order_acq_rel);
    bool pausedBreakpoint = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_FfxConfigureBreakpointMutex);
        void* target = reinterpret_cast<void*>(originalConfigure);
        if (!g_ffxConfigureInlineHooked.load(std::memory_order_acquire) &&
            g_ffxConfigureVehArmed.load(std::memory_order_acquire) &&
            g_ffxConfigureTarget.load(std::memory_order_acquire) == target && IsCommittedReadableCodeAddress(target)) {
            auto* targetByte = static_cast<uint8_t*>(target);
            if (*targetByte == 0xCC) {
                DWORD oldProtect = 0;
                if (VirtualProtect(target, 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
                    *targetByte = g_ffxConfigureOriginalFirstByte;
                    FlushInstructionCache(GetCurrentProcess(), targetByte, 1);
                    VirtualProtect(target, 1, oldProtect, &oldProtect);
                    g_ffxConfigureVehArmed.store(false, std::memory_order_release);
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

    const ffxReturnCode_t result = originalConfigure(context, desc);

    if (pausedBreakpoint) {
        g_FfxConfigureDeferredRearmTarget.store(reinterpret_cast<void*>(originalConfigure), std::memory_order_release);
        g_FfxConfigureDeferredRearm.store(true, std::memory_order_release);
    }

    const int remainingDepth = g_FfxConfigureOriginalForwardDepth.fetch_sub(1, std::memory_order_acq_rel) - 1;
    if (remainingDepth == 0 && g_FfxConfigureDeferredRearm.exchange(false, std::memory_order_acq_rel)) {
        void* deferredTarget = g_FfxConfigureDeferredRearmTarget.exchange(nullptr, std::memory_order_acq_rel);
        if (!deferredTarget) {
            deferredTarget = reinterpret_cast<void*>(originalConfigure);
        }
        ArmFfxConfigureBreakpoint(reinterpret_cast<PfnFfxConfigure>(deferredTarget), "protected official FFX runtime",
                                  "forward-call rearm");
    }
    return result;
}

static LONG WINAPI FfxConfigureBreakpointVEH(EXCEPTION_POINTERS* ep) {
    if (!ep) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    auto* ctx = ep->ContextRecord;
    auto* rec = ep->ExceptionRecord;

    void* target = g_ffxConfigureTarget.load(std::memory_order_acquire);
    const uintptr_t instructionPointer =
#ifdef _WIN64
        ctx ? static_cast<uintptr_t>(ctx->Rip) : 0;
#else
        ctx ? static_cast<uintptr_t>(ctx->Eip) : 0;
#endif
    if (!ctx || !rec || rec->ExceptionCode != STATUS_BREAKPOINT || !target ||
        !FFXHook::detail::IsEntryBreakpointHit(rec->ExceptionAddress, instructionPointer, target) ||
        !g_ffxConfigureVehArmed.load(std::memory_order_acquire)) {
        return EXCEPTION_CONTINUE_SEARCH;
    }

    DWORD oldProtect = 0;
    if (!VirtualProtect(reinterpret_cast<LPVOID>(target), 1, PAGE_EXECUTE_READWRITE, &oldProtect)) {
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    *static_cast<uint8_t*>(reinterpret_cast<LPVOID>(target)) = g_ffxConfigureOriginalFirstByte;
    FlushInstructionCache(GetCurrentProcess(), target, 1);
    VirtualProtect(reinterpret_cast<LPVOID>(target), 1, oldProtect, &oldProtect);
    g_ffxConfigureVehArmed.store(false, std::memory_order_release);

    auto contextPtr = reinterpret_cast<ffxContext*>(
#ifdef _WIN64
        ctx->Rcx
#else
        ctx->Ecx
#endif
    );
    auto* desc = reinterpret_cast<const ffxConfigureDescHeader*>(
#ifdef _WIN64
        ctx->Rdx
#else
        ctx->Edx
#endif
    );
    PfnFfxConfigure previousOverride = t_FfxConfigureOriginalOverride;
    t_FfxConfigureOriginalOverride = reinterpret_cast<PfnFfxConfigure>(target);
    ffxReturnCode_t result = Hooked_ffxConfigure(contextPtr, desc);
    t_FfxConfigureOriginalOverride = previousOverride;

    static std::atomic<int> s_vehHitLogCount{0};
    const int hitCount = s_vehHitLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (hitCount <= 20 || (hitCount % 300) == 0) {
        const auto* parsedDesc = reinterpret_cast<const ce::ffx_api::ApiHeader*>(desc);
        HookLogImportant(
            "FFX Hook: VEH ffxConfigure breakpoint handled call #%d (context=%p desc=%p type=0x%llx result=%u)",
            hitCount, contextPtr, desc, parsedDesc ? static_cast<unsigned long long>(parsedDesc->type) : 0ULL,
            static_cast<unsigned>(result));
    }

    // Re-arm after the real ffxConfigure returns. This keeps protected official
    // runtimes on a breakpoint hook without installing a permanent JMP, and
    // prevents early non-FG/disabled configures from consuming the only chance
    // to catch a later save-load FG enable.
    ArmFfxConfigureBreakpoint(reinterpret_cast<PfnFfxConfigure>(target), "protected official FFX runtime",
                              "post-call rearm");

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

static bool InstallFfxConfigureBreakpointHook(PfnFfxConfigure target, const char* moduleName) {
    if (!g_ffxConfigureVehHandle) {
        g_ffxConfigureVehHandle = AddVectoredExceptionHandler(1, FfxConfigureBreakpointVEH);
        if (!g_ffxConfigureVehHandle) {
            return false;
        }
    }

    const bool alreadyInstalledAndArmed =
        g_ffxConfigureVehInstalled && g_ffxConfigureVehArmed.load(std::memory_order_acquire) &&
        g_ffxConfigureTarget.load(std::memory_order_acquire) == reinterpret_cast<void*>(target);
    if (!ArmFfxConfigureBreakpoint(target, moduleName, "init")) {
        return false;
    }
    g_ffxConfigureVehInstalled = true;
    if (!alreadyInstalledAndArmed) {
        HookLogImportant("FFX Hook: Installed re-arming VEH hook for %s!ffxConfigure at %p", moduleName,
                         reinterpret_cast<void*>(target));
    }
    return true;
}

// Retroactive ffxConfigure call: when FSR FG activates through Streamline's
// authoritative takeover (no direct ffxConfigure intercepted), CE calls
// g_Original_ffxConfigure on all tracked FG contexts to install the present
// callback bridge.  This is necessary because ffxConfigure is called during
// AMD module init, before CE can intercept it.
static bool InstallBridgeOnTrackedContextsImpl(void* swapChain) {
    (void)swapChain;
    HookLogImportant(
        "FFX Hook: Skipping retroactive present-callback bridge install because synthetic partial ffxConfigure "
        "packets are unsafe for the official FSR runtime; waiting for a real enabled ffxConfigure instead");
    return false;
}

}  // anonymous namespace

// ============================================================================
// Public API
// ============================================================================

namespace FFXHook {

void* GetPresentCallbackBridgeKey(void* context) {
    return GetOrCreatePresentCallbackBridgeKey(context);
}

void RegisterDynamicHooks() {
    RegisterDynamicHooksOnce();
}

bool InstallBridgeOnTrackedContexts(void* swapChain) {
    return InstallBridgeOnTrackedContextsImpl(swapChain);
}

void Init() {
    std::lock_guard<std::mutex> lock(g_InitMutex);

    static int s_initCallCount = 0;
    ++s_initCallCount;

    if (!g_Initialized.load(std::memory_order_acquire) && !g_NoModulesLogged.load(std::memory_order_acquire)) {
        HookLog("FFX Hook: Initializing...");
    }

    RegisterDynamicHooksOnce();

    // Try to find FFX modules.
    // These cover both the older explicit FG DLL names and newer generic
    // FidelityFX runtime DLL names observed in GTA V Enhanced.
    // Also includes dlssg-to-fsr3 mod DLLs that redirect DLSS FG to FSR FG.
    const wchar_t* ffxModules[] = {
        // FSR 4 / FSR 3.1 DLLs (UE5 native integration) - check first.
        L"amd_fidelityfx_framegeneration_dx12.dll",
        L"amd_fidelityfx_framegeneration_vk.dll",
        // GTA V Enhanced can load the generic FidelityFX runtime DLL name while
        // still routing native frame generation through the FFX API exports.
        L"amd_fidelityfx_dx12.dll",
        L"amd_fidelityfx_vk.dll",
        // Standard AMD FSR FG DLLs.
        L"amd_fidelityfx_fg.dll",
        L"ffx_frameinterpolation_x64.dll",
        L"amd_fidelityfx_framegeneration.dll",
        L"ffx_framegeneration.dll",
        // dlssg-to-fsr3 mod - uses nvngx_dlssg.dll as a proxy that calls FFX API.
        L"nvngx_dlssg.dll",
        // FSR3 FG mod common names.
        L"fsr3fg.dll",
        L"fsr3mod.dll",
    };

    const char* ffxModuleNames[] = {
        "amd_fidelityfx_framegeneration_dx12.dll",
        "amd_fidelityfx_framegeneration_vk.dll",
        "amd_fidelityfx_dx12.dll",
        "amd_fidelityfx_vk.dll",
        "amd_fidelityfx_fg.dll",
        "ffx_frameinterpolation_x64.dll",
        "amd_fidelityfx_framegeneration.dll",
        "ffx_framegeneration.dll",
        "nvngx_dlssg.dll",
        "fsr3fg.dll",
        "fsr3mod.dll",
    };

    bool foundSupportedModule = false;
    for (size_t i = 0; i < _countof(ffxModules); ++i) {
        HMODULE hMod = GetModuleHandleW(ffxModules[i]);
        if (hMod) {
            if (!g_Initialized.load(std::memory_order_acquire) || g_HookedModule != hMod) {
                HookLog("FFX Hook: Found module %s at %p", ffxModuleNames[i], hMod);
            }
            g_FGCompat.SetFSRFGSupportPresent(true);
            if (InstallHooksForModule(hMod, ffxModuleNames[i])) {
                foundSupportedModule = true;
                continue;
            }
            // Module exists but has no FFX exports (e.g. real nvngx_dlssg.dll)
            static std::atomic<int> s_moduleWithoutExportsLogCount{0};
            const int logCount = s_moduleWithoutExportsLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
            if (logCount <= 20 || (logCount % 300) == 0) {
                HookLog("FFX Hook: Module %s has no FFX exports, continuing search (log=%d)", ffxModuleNames[i],
                        logCount);
            }
        }
    }

    if (foundSupportedModule) {
        g_Initialized.store(true, std::memory_order_release);
        g_NoModulesLogged.store(false, std::memory_order_release);
        return;
    }

    g_Initialized.store(false, std::memory_order_release);
    g_HookedModule = nullptr;
    g_DefaultPresentCallback = nullptr;

    if (!g_NoModulesLogged.exchange(true, std::memory_order_acq_rel)) {
        HookLog("FFX Hook: No FFX modules found, hooks not installed");
    }

    // One-time diagnostic at 30th retry: enumerate loaded modules for debugging
    if (s_initCallCount == 30) {
        HookLogImportant("FFX Hook: Module enumeration diagnostic (call #%d):", s_initCallCount);
        HMODULE hMods[1024];
        DWORD cbNeeded;
        if (EnumProcessModules(GetCurrentProcess(), hMods, sizeof(hMods), &cbNeeded)) {
            int found = 0;
            for (DWORD i = 0; i < cbNeeded / sizeof(HMODULE); i++) {
                wchar_t modName[MAX_PATH];
                if (GetModuleFileNameW(hMods[i], modName, MAX_PATH)) {
                    std::wstring lower(modName);
                    for (auto& c : lower)
                        c = towlower(c);
                    if (lower.find(L"fidelity") != std::wstring::npos || lower.find(L"ffx") != std::wstring::npos ||
                        lower.find(L"framegen") != std::wstring::npos || lower.find(L"fsr") != std::wstring::npos ||
                        lower.find(L"amd_") != std::wstring::npos) {
                        char narrowName[MAX_PATH];
                        WideCharToMultiByte(CP_UTF8, 0, modName, -1, narrowName, MAX_PATH, NULL, NULL);
                        HookLogImportant("FFX Hook:   Loaded: %s", narrowName);
                        found++;
                    }
                }
            }
            if (found == 0) {
                HookLogImportant("FFX Hook:   No AMD/FFX/FSR modules among %d loaded",
                                 (int)(cbNeeded / sizeof(HMODULE)));
            }
        }
    }
}

bool IsInitialized() {
    return g_Initialized.load(std::memory_order_acquire);
}

void Shutdown() {
    std::lock_guard<std::mutex> lock(g_InitMutex);

    if (!g_Initialized.load(std::memory_order_acquire)) {
        return;
    }

    HookLog("FFX Hook: Shutting down...");

    // Remove IAT hooks
    if (g_HookedModule) {
        // Note: IATHook::RemoveHook would need to be implemented
        // For now, we just clear the state - hooks will naturally be cleaned up
        // when the DLL unloads
    }

    // Cleanup VEH breakpoint hook
    if (g_ffxConfigureVehHandle) {
        RemoveVectoredExceptionHandler(g_ffxConfigureVehHandle);
        g_ffxConfigureVehHandle = nullptr;
    }
    RestoreFfxConfigureBreakpointIfCurrent(g_ffxConfigureTarget.load(std::memory_order_acquire), "FFX hook shutdown");
    g_ffxConfigureVehInstalled = false;
    g_ffxConfigureVehArmed.store(false, std::memory_order_release);
    g_ffxConfigureTarget.store(nullptr, std::memory_order_release);

    g_Original_ffxCreateContext = nullptr;
    g_Original_ffxDestroyContext = nullptr;
    g_Original_ffxConfigure = nullptr;
    g_HookedModule = nullptr;
    g_DefaultPresentCallback = nullptr;
    g_ActiveFGContextCount.store(0, std::memory_order_release);
    DX12_ClearNativeFSRStartupConfigureArming("FFX hook shutdown");
    g_FGCompat.SetFSRFGActive(false);
    g_FGCompat.SetFSRFGSupportPresent(false);
    g_NoModulesLogged.store(false, std::memory_order_release);
    g_Initialized.store(false, std::memory_order_release);

    HookLog("FFX Hook: Shutdown complete");
}

}  // namespace FFXHook
