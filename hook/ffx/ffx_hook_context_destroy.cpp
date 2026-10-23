#include "ffx_hook_internal.h"
#include "hook/metrics/performance_metrics.h"

ffxReturnCode_t Hooked_ffxDestroyContext(ffxContext* ffx_hook_context, const ffxAllocationCallbacks* memCb) {
    const auto originalDestroy = ffx_hook_t_FfxDestroyContextOriginalOverride
                                     ? ffx_hook_t_FfxDestroyContextOriginalOverride
                                     : ffx_hook_g_Original_ffxDestroyContext;
    if (!originalDestroy) {
        HookLog("FFX Hook: ffxDestroyContext called but original not set!");
        return 1;  // Error
    }
    if (HookIsShuttingDown() || ffx_hook_DelegatesToForeignObserver(reinterpret_cast<void*>(originalDestroy)))
        return originalDestroy(ffx_hook_context, memCb);

    const ffxContext contextHandle = ffx_hook_context ? *ffx_hook_context : nullptr;

    // Inspect before forwarding but commit no bookkeeping changes until AMD confirms destruction succeeded.
    // This preserves callback delegation and queue ownership if the provider rejects the destroy.
    bool isFGContext = false;
    bool isFGSwapchainContext = false;
    bool isKnownContext = false;
    bool isVulkanContext = false;
    bool isVulkanFGContext = false;
    {
        std::lock_guard<std::mutex> lock(ffx_hook_g_ContextMapMutex);
        auto it = ffx_hook_g_ContextTypeMap.find(contextHandle);
        if (it != ffx_hook_g_ContextTypeMap.end()) {
            isKnownContext = true;
            uint32_t effectId = it->second;
            isVulkanContext = ffx_hook_g_VulkanContextSet.find(contextHandle) != ffx_hook_g_VulkanContextSet.end();
            isVulkanFGContext = isVulkanContext && ce::ffx_api::IsFrameGenerationEffectType(effectId);
            isFGSwapchainContext = !isVulkanContext && effectId == ffx_hook_FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN;
            isFGContext = !isVulkanContext && (effectId == ffx_hook_FFX_API_EFFECT_ID_FRAMEGENERATION ||
                                               effectId == ffx_hook_FFX_API_EFFECT_ID_FRAMEGENERATIONSWAPCHAIN);
        }
    }

    // Call original
    ffxReturnCode_t result = originalDestroy(ffx_hook_context, memCb);

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
            // A known effect context (frame generation, upscaling) presents nothing: AMD keeps presenting what its
            // swapchain context scheduled, and games destroy the FG context first. Tearing the presentation down
            // here left the last FSR FG output without the topmost overlay (FG flow test FlowSwitch GTA-style);
            // the swapchain context's destroy, or the last context's (dx12_hook_ffx.cpp), ends it.
            if (isFGSwapchainContext || !isKnownContext) {
                DX12_UnregisterNativeFSRSwapchainPresentationQueue(contextHandle, "FFX swapchain context destroyed");
            } else {
                HookLog("FFX Hook: effect context %p destroyed; its swapchain's presentation stays registered",
                        contextHandle);
            }
            ClearSubstituteUiReRegistrationForContext(contextHandle);
            if (isFGSwapchainContext) {
                // The protected-startup latch belongs to this swapchain context, not to the process-wide
                // FG context count: the sibling FRAMEGENERATION context survives FG toggles, so the
                // count-based teardown below never runs and the latch would quiesce CE forever.
                DX12_RetireProtectedOfficialFFXStartupForDestroyedFFXSwapchainContext(
                    "FFX frame-generation swapchain context destroyed");
            }
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
            if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
                perf->NotifyFSRFrameGenerationTransition(false, "ffxContextDestroy");
            }
            ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kFFXContextDestroy,
                                        "FFXHook::Hooked_ffxDestroyContext", reinterpret_cast<void*>(ffx_hook_context),
                                        nullptr, ce::fg_runtime::RuntimeMode::kOff, false, true);
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
