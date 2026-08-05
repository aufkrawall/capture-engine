#include "ffx_hook_internal.h"


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

void* GetOrCreatePresentCallbackBridgeKey(ffxContext ffx_hook_context) {


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

void* GetPresentCallbackBridgeKey(ffxContext ffx_hook_context) {


    if (!ffx_hook_context) {
        return nullptr;
    }

    return reinterpret_cast<void*>(ffx_hook_context);

}

bool HasTrackedPresentCallbackBridgeKey(void* key) {


    if (!key) {
        return false;
    }

    std::lock_guard<std::mutex> lock(ffx_hook_g_PresentCallbackBridgeMutex);
    return ffx_hook_g_PresentCallbackBridgeKeys.find(key) != ffx_hook_g_PresentCallbackBridgeKeys.end();

}

uint32_t GetEffectId(ffxStructType_t type) {


    return static_cast<uint32_t>(type) & ffx_hook_FFX_API_EFFECT_MASK;

}

ffxReturnCode_t Hooked_ffxCreateContext(ffxContext* ffx_hook_context,  ffxCreateContextDescHeader* ffx_hook_desc, 
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

ffxReturnCode_t Hooked_ffxDestroyContext(ffxContext* ffx_hook_context,  const ffxAllocationCallbacks* memCb) {


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

void ClearSubstituteUiReRegistrationForContext(ffxContext ffx_hook_context) {


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

void StoreSubstituteUiReRegistration(
    ffxContext* ffx_hook_context,  PfnFfxConfigure originalConfigure, 
    const ce::ffx_api::ConfigureDescFrameGenerationSwapChainRegisterUiResource& substitutedDesc) {


    std::lock_guard<std::mutex> lock(ffx_hook_g_SubstReRegMutex);
    ffx_hook_g_SubstReRegContext = ffx_hook_context ? *ffx_hook_context : nullptr;
    ffx_hook_g_SubstReRegConfigure = originalConfigure;
    ffx_hook_g_SubstReRegDesc = substitutedDesc;
    ffx_hook_g_SubstReRegActive.store(true, std::memory_order_release);

}

ffxReturnCode_t Hooked_ffxConfigure(ffxContext* ffx_hook_context,  const ffxConfigureDescHeader* ffx_hook_desc) {


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

bool IsFFXDynamicHookOwnerModule(const char* moduleBaseName,  HMODULE module) {


    if (moduleBaseName && ce::overlay_compat::IsFFXFrameGenerationModulePath(moduleBaseName)) {
        return true;
    }

    char modulePath[MAX_PATH] = {};
    if (module && GetModuleFileNameA(module, modulePath, sizeof(modulePath))) {
        return ce::overlay_compat::IsFFXFrameGenerationModulePath(modulePath);
    }

    return false;

}

void ffx_hook_RegisterDynamicHooksOnce() {


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
