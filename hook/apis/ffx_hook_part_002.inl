
    // Stamp QPC + frame counter for freeze-diagnosis timeline correlation (composite vs configure-forward).
    DX12_NoteFfxConfigureForward(parsedDesc ? parsedDesc->type : 0);

    const ffxReturnCode_t result = CallFfxConfigureOriginalGuarded(originalConfigure, context, descToCall);
    if (uiTargetPrepared) {
        if (result == FFX_API_RETURN_OK) {
            DX12_CommitFFXUiOverlayTarget(&uiTargetPreparation);
            if (uiTargetSubstituted) {
                // Publish re-registration only after AMD accepted the substitute. A failed configure must keep
                // the prior known-good target/descriptor intact.
                StoreSubstituteUiReRegistration(context, originalConfigure, localUiConfig);
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
    if (result != FFX_API_RETURN_OK || !desc) {
        return result;
    }

    const auto parsed =
        ce::ffx_api::ParseFrameGenerationConfigureState(reinterpret_cast<const ce::ffx_api::ApiHeader*>(desc));
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
        const int logCount = s_retainedDisabledPresentCallbackBridgeLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            const auto* originalDesc = reinterpret_cast<const ce::ffx_api::ConfigureDescFrameGeneration*>(desc);
            HookLogImportant(
                "FFX Hook: Retained existing DX12 overlay present-callback bridge for disabled native-FSR configure "
                "(context=%p frameID=%llu originalPresent=%p bridgeUserCtx=%p log=%d)",
                context, static_cast<unsigned long long>(originalDesc->frameID),
                reinterpret_cast<void*>(originalDesc->presentCallback), localConfig.presentCallbackUserContext,
                logCount + 1);
        }
    } else if (retainedBridgeForNullCallbackToggle) {
        static std::atomic<int> s_retainedNullCallbackToggleBridgeLogCount{0};
        const int logCount = s_retainedNullCallbackToggleBridgeLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            const auto* originalDesc = reinterpret_cast<const ce::ffx_api::ConfigureDescFrameGeneration*>(desc);
            HookLogImportant(
                "FFX Hook: Retained DX12 overlay present-callback bridge across enabled app->null-callback toggle "
                "(AMD keeps calling CE's bridge; delegating to retained original instead of self-compose to avoid "
                "the ffxQuery wedge) (context=%p frameID=%llu appNull=1 bridgeUserCtx=%p log=%d)",
                context, static_cast<unsigned long long>(originalDesc->frameID), localConfig.presentCallbackUserContext,
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
                context, static_cast<unsigned long long>(parsed.frameId),
                DX12_IsRuntimeOwnedSwapchainActiveForFrameGeneration() ? 1 : 0,
                g_FGCompat.HasDirectFFXApiConfirmation() ? 1 : 0, logCount + 1);
        }
        return result;
    }

    // Keep per-context dedupe and its global routing/session publications ordered even when a runtime emits
    // configure packets concurrently. The provider call itself remains outside this lock.
    std::lock_guard<std::mutex> transitionLock(g_FrameGenerationRoutingTransitionMutex);
    const bool bridgeActiveForConfigure =
        installedPresentCallbackBridge || retainedAlreadyBridgedPresentCallback || retainedBridgeForDisabledConfigure;
    bool enabledStateChanged = parsed.enabled;
    bool routingStateChanged = true;
    {
        std::lock_guard<std::mutex> lock(g_ContextMapMutex);
        const auto existing = g_FrameGenerationRoutingByContext.find(contextHandle);
        if (existing != g_FrameGenerationRoutingByContext.end()) {
            enabledStateChanged = existing->second.enabled != parsed.enabled;
            routingStateChanged = enabledStateChanged || existing->second.bridgeActive != bridgeActiveForConfigure ||
                                  existing->second.appCallbackProvided != appPresentCallbackProvided;
            existing->second = {parsed.enabled, bridgeActiveForConfigure, appPresentCallbackProvided};
        } else {
            // A first observed disabled configure has no live state to tear down. The first enabled configure is
            // a real transition and must still finalize protected FFX startup.
            enabledStateChanged = parsed.enabled;
            g_FrameGenerationRoutingByContext.emplace(
                contextHandle,
                FrameGenerationRoutingState{parsed.enabled, bridgeActiveForConfigure, appPresentCallbackProvided});
        }
    }

    if (enabledStateChanged) {
        HookLogImportant("FFX Hook: Frame Generation configure transition %s (context=%p frameID=%llu type=0x%llx)",
                         parsed.enabled ? "ENABLED" : "DISABLED", context,
                         static_cast<unsigned long long>(parsed.frameId), static_cast<unsigned long long>(desc->type));
    } else {
        static std::atomic<int> s_unchangedConfigureLogCount{0};
        const int logCount = s_unchangedConfigureLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (logCount <= 10 || (logCount % 600) == 0) {
            HookLog(
                "FFX Hook: Frame Generation configure unchanged (%s context=%p frameID=%llu routingChanged=%d "
                "log=%d)",
                parsed.enabled ? "enabled" : "disabled", context, static_cast<unsigned long long>(parsed.frameId),
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
                context, static_cast<unsigned long long>(parsed.frameId));
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
            "FFXHook::Hooked_ffxConfigure", reinterpret_cast<void*>(context), nullptr,
            parsed.enabled ? ce::fg_runtime::RuntimeMode::kFSRFG : ce::fg_runtime::RuntimeMode::kOff, parsed.enabled,
            true);
    }
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

    // Do not retire a live durable route merely because a related FFX DLL without ffxConfigure was observed.
    // Only a genuinely different callable configure export requires a new breakpoint/routing epoch.
    const void* installedConfigureTarget = g_ffxConfigureTarget.load(std::memory_order_acquire);
    if (firstSeenModule && g_HookedModule && configureCtx && installedConfigureTarget &&
        installedConfigureTarget != reinterpret_cast<void*>(configureCtx)) {
        g_DurableCachedConfigureRouteActive.store(false, std::memory_order_release);
        g_ffxConfigureVehPermanentlyDisarmed.store(false, std::memory_order_release);
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
                if (g_ffxConfigureVehPermanentlyDisarmed.load(std::memory_order_acquire)) {
                    // Expected no-op: the one-shot VEH already detected no-callback mode and permanently
                    // disarmed for this FG-on window (re-armed on FG-off). The periodic module rescan used
                    // to log this as a failure every second (session 20260701_213656) — log once instead.
                    static std::atomic<int> s_disarmedRearmSkipLogCount{0};
                    if (s_disarmedRearmSkipLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
                        HookLog(
                            "FFX Hook: ffxConfigure VEH re-arm skipped — one-shot breakpoint permanently "
                            "disarmed for this FG-on window (module=%s target=%p)",
                            moduleName ? moduleName : "FFX", reinterpret_cast<void*>(configureCtx));
                    }
                } else {
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
    }

    if (routedAnything) {
        static std::atomic<int> s_hooksInstalledLogCount{0};
        const int logCount = s_hooksInstalledLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (firstSeenModule || logCount <= 20 || (logCount % 300) == 0) {
            HookLog(
                "FFX Hook: Hooks installed successfully for %s (inline=%d iat=%d veh=%d dynamic=1 protected=%d "
                "log=%d)",
                moduleName, inlineHookedAnything ? 1 : 0, iatPatchedAnything ? 1 : 0, vehHookedAnything ? 1 : 0,
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
            g_DurableCachedConfigureRouteActive.exchange(true, std::memory_order_acq_rel);
        const bool breakpointWasArmed = g_ffxConfigureVehArmed.load(std::memory_order_acquire);
        g_ffxConfigureVehPermanentlyDisarmed.store(true, std::memory_order_release);
        RestoreFfxConfigureBreakpointIfCurrent(reinterpret_cast<void*>(configureCtx),
                                               "durable cached ffxConfigure pointer route installed");
        g_FfxConfigureDeferredRearm.store(false, std::memory_order_release);
        g_FfxConfigureDeferredRearmTarget.store(nullptr, std::memory_order_release);
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
    VirtualProtect(target, 1, PAGE_EXECUTE_READ, &oldProtect);
    g_ffxConfigureVehArmed.store(false, std::memory_order_release);
    HookLogImportant("FFX Hook: Restored stale VEH breakpoint at %p before retargeting (%s)", target,
                     reason && reason[0] ? reason : "target changed");
}

static bool ArmFfxConfigureBreakpoint(PfnFfxConfigure target, const char* moduleName, const char* reason) {
    std::lock_guard<std::recursive_mutex> lock(g_FfxConfigureBreakpointMutex);
    if (!target) {
        return false;
    }

    // One-shot detection: if the VEH was permanently disarmed (after the first enabled no-callback
    // ffxConfigure), do NOT re-arm. The byte stays as the original — ffxConfigure runs natively.
    if (g_ffxConfigureVehPermanentlyDisarmed.load(std::memory_order_acquire)) {
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

    auto* targetByte = reinterpret_cast<uint8_t*>(target);
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
    VirtualProtect(reinterpret_cast<LPVOID>(target), 1, PAGE_EXECUTE_READ, &oldProtect);

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
