#include "streamline_hook_internal.h"


slResult SlNullFunctionStub() {


    return streamline_hook_kSlResultOk;

}


void* Hooked_slGetPluginFunction(const char* streamline_hook_functionName) {


    if (StreamlineHook::IsExternalOverlayPresentGuardActive()) {
        static std::atomic<int> s_externalOverlaySuppressedPluginLookupLogCount{0};
        const int logCount = s_externalOverlaySuppressedPluginLookupLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 200) == 0) {
            HookLogImportant(
                "Streamline Hook: Suppressing re-entrant slGetPluginFunction during guarded external overlay "
                "Present (name=%s depth=%d)",
                streamline_hook_functionName ? streamline_hook_functionName : "null", streamline_hook_g_ExternalOverlayPresentGuardDepth);
        }
        return reinterpret_cast<void*>(SlNullFunctionStub);
    }

    auto originalGetPluginFunction = GetCallableOriginalGetPluginFunction();
    if (!originalGetPluginFunction) {
        return streamline_hook_g_Original_slGetPluginFunction ? reinterpret_cast<void*>(SlNullFunctionStub) : nullptr;
    }

    return originalGetPluginFunction(streamline_hook_functionName);

}


slResult Hooked_slGetFeatureFunction(uint32_t feature,  const char* streamline_hook_functionName,  void*& streamline_hook_function) {


    if (StreamlineHook::IsExternalOverlayPresentGuardActive()) {
        static std::atomic<int> s_externalOverlaySuppressedLookupLogCount{0};
        const int logCount = s_externalOverlaySuppressedLookupLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 200) == 0) {
            HookLogImportant(
                "Streamline Hook: Suppressing re-entrant slGetFeatureFunction during guarded external overlay "
                "Present (feature=%u name=%s depth=%d)",
                feature, streamline_hook_functionName ? streamline_hook_functionName : "null", streamline_hook_g_ExternalOverlayPresentGuardDepth);
        }
        streamline_hook_function = reinterpret_cast<void*>(SlNullFunctionStub);
        return streamline_hook_kSlResultErrorInvalidState;
    }

    auto originalGetFeatureFunction = GetCallableOriginalGetFeatureFunction();
    if (!originalGetFeatureFunction) {
        streamline_hook_function = reinterpret_cast<void*>(SlNullFunctionStub);
        return streamline_hook_kSlResultErrorInvalidState;
    }

    const slResult result = originalGetFeatureFunction(feature, streamline_hook_functionName, streamline_hook_function);
    // Safety: if the original returned success but gave us NULL, the caller
    // would call through NULL → RIP=0 crash.  This can happen when third-party
    // overlays (e.g., Steam's OverlayHookD3D3) call slGetFeatureFunction
    // re-entrantly from within Streamline's own code during FG processing.
    // Substitute a safe no-op stub so the caller doesn't crash even if it
    // ignores the error return and uses the function pointer directly.
    if (result == streamline_hook_kSlResultOk && !streamline_hook_function) {
        static std::atomic<int> s_nullFunctionLogCount{0};
        if (s_nullFunctionLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
            HookLogImportant(
                "Streamline Hook: slGetFeatureFunction returned OK with NULL function "
                "(feature=%u name=%s) — substituting safe no-op stub to prevent null call crash",
                feature, streamline_hook_functionName ? streamline_hook_functionName : "null");
        }
        streamline_hook_function = reinterpret_cast<void*>(SlNullFunctionStub);
        return streamline_hook_kSlResultOk;
    }
    if (result != streamline_hook_kSlResultOk || !streamline_hook_functionName || !streamline_hook_function) {
        return result;
    }

    // DLSS Frame Generation feature hooks
    if (feature == streamline_hook_kSLFeatureDLSSG) {
        if (strcmp(streamline_hook_functionName, "slDLSSGSetOptions") == 0) {
            void* originalFunction = streamline_hook_function;
            const bool hookReady = MaybeHookDLSSGSetOptions(streamline_hook_function, true);
            LogFeatureLookupOutcomeOnce(streamline_hook_g_DLSSGSetOptionsLookupLogged, "slDLSSGSetOptions", originalFunction, streamline_hook_function,
                                        hookReady);
        } else if (strcmp(streamline_hook_functionName, "slDLSSGGetState") == 0) {
            void* originalFunction = streamline_hook_function;
            const bool hookReady = MaybeHookDLSSGGetState(streamline_hook_function, true);
            LogFeatureLookupOutcomeOnce(streamline_hook_g_DLSSGGetStateLookupLogged, "slDLSSGGetState", originalFunction, streamline_hook_function,
                                        hookReady);
            // Talos resolves GetState shortly before it starts tagging the activation inputs, but
            // never resolves/calls SetOptions. Arm standby at pointer delivery, before those tags.
            if (!ShouldKeepPureObserverOnlyStreamlineBehavior() &&
                streamline_hook_g_StreamlineUsesD3D12.load(std::memory_order_acquire)) {
                ce::dx12_streamline_ui_overlay::BeginPreactivationStandby(2);
            }
        }
    }
    // Reflex feature hook — detect game activation of native Reflex
    else if (feature == streamline_hook_kSLFeatureReflex) {
        if (strcmp(streamline_hook_functionName, "slReflexSleep") == 0) {
            void* originalFunction = streamline_hook_function;
            const bool hookReady = MaybeHookReflexSleep(streamline_hook_function, true);
            LogFeatureLookupOutcomeOnce(streamline_hook_g_ReflexSleepLookupLogged, "slReflexSleep", originalFunction, streamline_hook_function,
                                        hookReady);
        } else if (strcmp(streamline_hook_functionName, "slReflexSetOptions") == 0) {
            void* originalFunction = streamline_hook_function;
            const bool hookReady = MaybeHookReflexSetOptions(streamline_hook_function, true);
            LogFeatureLookupOutcomeOnce(streamline_hook_g_ReflexSetOptionsLookupLogged, "slReflexSetOptions", originalFunction,
                                        streamline_hook_function, hookReady);
        } else if (strcmp(streamline_hook_functionName, "slReflexSetConstants") == 0) {
            void* originalFunction = streamline_hook_function;
            const bool hookReady = MaybeHookReflexSetConstants(streamline_hook_function, true);
            LogFeatureLookupOutcomeOnce(streamline_hook_g_ReflexSetConstantsLookupLogged, "slReflexSetConstants", originalFunction,
                                        streamline_hook_function, hookReady);
        }
    }

    return result;

}


slResult Hooked_slSetD3DDevice(void* streamline_hook_d3dDevice) {


    auto originalSetD3DDevice = GetCallableOriginalSetD3DDevice();
    if (!originalSetD3DDevice) {
        return streamline_hook_kSlResultErrorInvalidState;
    }

    ID3D12Device* acceptedD3D12Device = nullptr;
    if (streamline_hook_d3dDevice) {
        static_cast<IUnknown*>(streamline_hook_d3dDevice)->QueryInterface(IID_PPV_ARGS(&acceptedD3D12Device));
    }
    const bool isD3D12 = acceptedD3D12Device != nullptr;

    const slResult result = originalSetD3DDevice(streamline_hook_d3dDevice);
    if (result == streamline_hook_kSlResultOk) {
        ID3D12Device* previousAcceptedDevice = nullptr;
        {
            std::lock_guard<std::mutex> lock(streamline_hook_g_AcceptedD3D12DeviceMutex);
            previousAcceptedDevice = streamline_hook_g_AcceptedD3D12Device;
            streamline_hook_g_AcceptedD3D12Device = acceptedD3D12Device;
            acceptedD3D12Device = nullptr;
        }
        if (previousAcceptedDevice) {
            previousAcceptedDevice->Release();
        }
        streamline_hook_g_StreamlineUsesD3D12.store(isD3D12, std::memory_order_release);
        if (isD3D12 && !ShouldKeepPureObserverOnlyStreamlineBehavior()) {
            // Resource tags are legal immediately after Streamline accepts the device. Some
            // integrations (Talos) publish their reusable UI tag before resolving any DLSS-G
            // feature function, so GetState-pointer delivery is too late to cover that tag.
            ce::dx12_streamline_ui_overlay::BeginPreactivationStandby(2);
            HookLogImportant(
                "Streamline Hook: D3D12 device accepted — official UI preactivation standby ready before tags "
                "(device=%p)",
                streamline_hook_d3dDevice);
        } else if (!isD3D12) {
            ce::dx12_streamline_ui_overlay::EndPreactivationStandby("Streamline device is not D3D12");
        }
        TryResolveDLSSGFeatureHooks();
        TryResolveReflexFeatureHooks();
    }
    if (acceptedD3D12Device) {
        acceptedD3D12Device->Release();
    }
    return result;

}


bool StructTypesEqual(const slStructType& lhs,  const slStructType& rhs) {


    return lhs.data1 == rhs.data1 && lhs.data2 == rhs.data2 && lhs.data3 == rhs.data3 &&
           std::memcmp(lhs.data4, rhs.data4, sizeof(lhs.data4)) == 0;

}


bool TryRecordOfficialUiResourceTag(const void* frameToken,  const slResourceTag& tag,  void* streamline_hook_commandBuffer) {


    if (ShouldKeepPureObserverOnlyStreamlineBehavior() || !streamline_hook_g_StreamlineUsesD3D12.load(std::memory_order_acquire) ||
        !streamline_hook_commandBuffer || tag.type != streamline_hook_kSLBufferTypeUIColorAndAlpha || !tag.resource || !tag.resource->native ||
        tag.resource->type != slResourceType::kTexture2D || tag.extent.top != 0 || tag.extent.left != 0) {
        return false;
    }

    auto* uiResource = static_cast<ID3D12Resource*>(tag.resource->native);
    const D3D12_RESOURCE_DESC desc = uiResource->GetDesc();
    const uint32_t width = tag.extent.width != 0
                               ? tag.extent.width
                               : (tag.resource->width != 0 ? tag.resource->width : static_cast<uint32_t>(desc.Width));
    const uint32_t height =
        tag.extent.height != 0 ? tag.extent.height : (tag.resource->height != 0 ? tag.resource->height : desc.Height);
    const DXGI_FORMAT format =
        tag.resource->nativeFormat != 0 ? static_cast<DXGI_FORMAT>(tag.resource->nativeFormat) : desc.Format;
    const bool hdr = DX12_ResolveRuntimeOwnedOverlayTargetHDRState(format);
    ID3D12CommandQueue* initializationQueue = DX12_AcquireOriginalGameQueueForOverlay();
    if (!initializationQueue) {
        return false;
    }

    ce::dx12_streamline_ui_overlay::RecordRequest request;
    request.commandList = static_cast<ID3D12GraphicsCommandList*>(streamline_hook_commandBuffer);
    request.uiResource = uiResource;
    request.initializationQueue = initializationQueue;
    request.resourceState = static_cast<D3D12_RESOURCE_STATES>(tag.resource->state);
    request.format = format;
    request.width = width;
    request.height = height;
    request.hdr = hdr;
    request.frameToken = frameToken;
    const bool recorded = ce::dx12_streamline_ui_overlay::TryRecordBootstrap(request);
    initializationQueue->Release();
    return recorded;

}


uint32_t LogOfficialUiTagOpportunity(const char* tagApi,  const void* frameToken,  uint32_t viewportKey, 
                                     const slResourceTag* tags,  uint32_t numTags,  void* streamline_hook_commandBuffer, 
                                     uint32_t feature,  uint32_t numInputs) {


    static std::atomic<uint32_t> s_uiTagOpportunityLogCount{0};
    const uint32_t opportunity = s_uiTagOpportunityLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (opportunity > 12 && (opportunity % 300) != 0) {
        return 0;
    }

    HookLogImportant(
        "Streamline Hook: Official UI tag record opportunity #%u (api=%s feature=%u frame=%p viewport=%u "
        "tags=%p numTags=%u inputs=%u commandBuffer=%p d3d12=%d)",
        opportunity, tagApi ? tagApi : "unknown", feature, frameToken, viewportKey, tags, numTags, numInputs,
        streamline_hook_commandBuffer, streamline_hook_g_StreamlineUsesD3D12.load(std::memory_order_relaxed) ? 1 : 0);
    const uint32_t loggedTags = tags ? std::min(numTags, 12u) : 0u;
    for (uint32_t i = 0; i < loggedTags; ++i) {
        const slResourceTag& tag = tags[i];
        HookLogImportant(
            "Streamline Hook: UI tag opportunity #%u tag[%u] type=%u lifecycle=%d resource=%p "
            "extent=(%u,%u %ux%u)",
            opportunity, i, tag.type, tag.lifecycle, tag.resource, tag.extent.left, tag.extent.top, tag.extent.width,
            tag.extent.height);
    }
    return opportunity;

}


void TryRecordOfficialUiTag(const char* tagApi,  const void* frameToken,  const slViewportHandle& viewport, 
                            const slResourceTag* tags,  uint32_t numTags,  void* streamline_hook_commandBuffer) {


    const bool wantsUiBootstrapRecord = ce::dx12_streamline_ui_overlay::OnFrameTag(frameToken);

    if (wantsUiBootstrapRecord) {
        LogOfficialUiTagOpportunity(tagApi, frameToken, GetViewportKey(viewport), tags, numTags, streamline_hook_commandBuffer);
    }

    // DLSS-G consumes UIColorAndAlpha before its first generated output exists, while PostSL can
    // only run after that output has been produced. Record CE's rolling/one-shot overlay into the
    // official UI layer on the app-provided command list. Source frames keep replacing the
    // eValidUntilPresent record until PostSL consumes the bounded output handoff. This introduces
    // no copy, extra submission, queue, or wait and naturally follows Streamline's synchronization.
    if (wantsUiBootstrapRecord && tags) {
        for (uint32_t i = 0; i < numTags; ++i) {
            if (TryRecordOfficialUiResourceTag(frameToken, tags[i], streamline_hook_commandBuffer)) {
                break;
            }
        }
    }

}


slResult Hooked_slSetTag(const slViewportHandle& viewport,  const slResourceTag* tags,  uint32_t numTags, 
                         void* streamline_hook_commandBuffer) {


    auto originalSetTag = GetCallableOriginalSetTag();
    if (!originalSetTag) {
        return streamline_hook_kSlResultErrorInvalidState;
    }

    // Legacy/global resource tagging has no frame token. A monotonically unique opaque identity
    // lets the standby state roll across calls without dereferencing or fabricating an SL object.
    static std::atomic<uintptr_t> s_legacyTagToken{1};
    const uintptr_t tokenValue = s_legacyTagToken.fetch_add(1, std::memory_order_relaxed);
    const void* frameToken = reinterpret_cast<const void*>((tokenValue << 1u) | 1u);
    TryRecordOfficialUiTag("slSetTag", frameToken, viewport, tags, numTags, streamline_hook_commandBuffer);

    return originalSetTag(viewport, tags, numTags, streamline_hook_commandBuffer);

}


slResult Hooked_slSetTagForFrame(const slBaseStructure& streamline_hook_frame,  const slViewportHandle& viewport, 
                                 const slResourceTag* tags,  uint32_t numTags,  void* streamline_hook_commandBuffer) {


    auto originalSetTagForFrame = GetCallableOriginalSetTagForFrame();
    if (!originalSetTagForFrame) {
        return streamline_hook_kSlResultErrorInvalidState;
    }

    TryRecordOfficialUiTag("slSetTagForFrame", &streamline_hook_frame, viewport, tags, numTags, streamline_hook_commandBuffer);

    // Streamline observes the resource only after CE's commands have been appended. For volatile
    // tags this is essential: any copy Streamline records into the same command list includes CE.
    return originalSetTagForFrame(streamline_hook_frame, viewport, tags, numTags, streamline_hook_commandBuffer);

}


slResult Hooked_slEvaluateFeature(uint32_t feature,  const slBaseStructure& streamline_hook_frame,  const slBaseStructure** inputs, 
                                  uint32_t numInputs,  void* streamline_hook_commandBuffer) {


    auto originalEvaluateFeature = GetCallableOriginalEvaluateFeature();
    if (!originalEvaluateFeature) {
        return streamline_hook_kSlResultErrorInvalidState;
    }

    // Streamline explicitly permits ResourceTag structures as local evaluate inputs; those tags
    // never pass through slSetTag/slSetTagForFrame. Talos uses this route. Keep the steady-state
    // evaluate path to one atomic branch, then inspect only the short activation/standby window.
    const void* frameToken = &streamline_hook_frame;
    const bool wantsUiBootstrapRecord = ce::dx12_streamline_ui_overlay::OnFrameTag(frameToken);
    if (wantsUiBootstrapRecord) {
        slViewportHandle viewport;
        uint32_t localTagCount = 0;
        constexpr uint32_t kMaximumInputChainDepth = 16;
        if (inputs) {
            for (uint32_t i = 0; i < numInputs; ++i) {
                const slBaseStructure* input = inputs[i];
                for (uint32_t depth = 0; input && depth < kMaximumInputChainDepth; ++depth) {
                    if (StructTypesEqual(input->structType, streamline_hook_kViewportHandleStructType)) {
                        viewport.value = static_cast<const slViewportHandle*>(input)->value;
                    }
                    if (StructTypesEqual(input->structType, streamline_hook_kResourceTagStructType)) {
                        ++localTagCount;
                    }
                    const slBaseStructure* next = input->next;
                    if (next == input) {
                        break;
                    }
                    input = next;
                }
            }
        }

        const uint32_t opportunity =
            LogOfficialUiTagOpportunity("slEvaluateFeature", frameToken, GetViewportKey(viewport), nullptr,
                                        localTagCount, streamline_hook_commandBuffer, feature, numInputs);
        uint32_t tagIndex = 0;
        bool recorded = false;
        if (inputs) {
            for (uint32_t i = 0; i < numInputs && !recorded; ++i) {
                const slBaseStructure* input = inputs[i];
                for (uint32_t depth = 0; input && depth < kMaximumInputChainDepth; ++depth) {
                    if (StructTypesEqual(input->structType, streamline_hook_kResourceTagStructType)) {
                        const auto& tag = *static_cast<const slResourceTag*>(input);
                        if (opportunity != 0 && tagIndex < 12) {
                            HookLogImportant(
                                "Streamline Hook: UI tag opportunity #%u localTag[%u] input=%u depth=%u type=%u "
                                "lifecycle=%d resource=%p extent=(%u,%u %ux%u)",
                                opportunity, tagIndex, i, depth, tag.type, tag.lifecycle, tag.resource, tag.extent.left,
                                tag.extent.top, tag.extent.width, tag.extent.height);
                        }
                        ++tagIndex;
                        recorded = TryRecordOfficialUiResourceTag(frameToken, tag, streamline_hook_commandBuffer);
                    }
                    const slBaseStructure* next = input->next;
                    if (next == input) {
                        break;
                    }
                    input = next;
                }
            }
        }
    }

    // Append CE before Streamline observes/copies any eOnlyValidNow/eValidUntilEvaluate UI tag.
    const slResult result =
        originalEvaluateFeature(feature, streamline_hook_frame, inputs, numInputs, streamline_hook_commandBuffer);
    if (result == streamline_hook_kSlResultOk &&
        (feature == streamline_hook_kFeatureDLSS || feature == streamline_hook_kFeatureDLSSRR)) {
        const uint32_t previous =
            streamline_hook_g_LastUpscalerEvaluation.exchange(feature, std::memory_order_acq_rel);
        if (g_IPC && g_IPC->GetSharedMem()) {
            auto& state = g_IPC->GetSharedMem()->dlssState;
            const bool rayReconstruction = feature == streamline_hook_kFeatureDLSSRR;
            state.rrActive.store(rayReconstruction, std::memory_order_release);
            state.srActive.store(!rayReconstruction, std::memory_order_release);
        }
        if (feature != previous) {
            static std::atomic<uint32_t> transitionLogs{0};
            if (transitionLogs.fetch_add(1, std::memory_order_relaxed) < 16) {
                if (feature == streamline_hook_kFeatureDLSSRR) {
                    HookLogImportant(
                        "Streamline RR: kFeatureDLSS_RR (1001) evaluation succeeded; Ray Reconstruction is rendering");
                } else if (GetActiveGraphicsConfig().forceRayReconstruction) {
                    HookLogImportant(
                        "Streamline RR: ordinary kFeatureDLSS (0) evaluation succeeded while force policy is enabled; "
                        "RR was not selected or has fallen back");
                } else {
                    HookLog("Streamline: ordinary DLSS evaluation confirmed");
                }
            }
        }
    }
    return result;

}


slResult Hooked_slReflexSleep(const void* streamline_hook_frame) {


    auto originalReflexSleep = GetCallableOriginalReflexSleep();
    if (!originalReflexSleep) {
        return streamline_hook_kSlResultErrorInvalidState;
    }

    // DLSSG-health evidence only: relaxed atomics + GetTickCount64 (shared-page read). No locks, no
    // logging, no syscalls — the manual Reflex FPS limiter's latency path through this hook is unchanged.
    streamline_hook_g_ReflexSleepObservedCount.fetch_add(1, std::memory_order_relaxed);
    streamline_hook_g_ReflexSleepLastTickMs.store(GetTickCount64(), std::memory_order_relaxed);

    g_ReflexLimiter.ApplyHybridPacingBeforeNativeSleep();

    const slResult result = originalReflexSleep(streamline_hook_frame);
    if (result == streamline_hook_kSlResultOk) {
        g_ReflexLimiter.MarkGameSleep("Streamline");
        g_ReflexLimiter.MarkNativePacingSignal();
    } else {
        static std::atomic<int> s_reflexSleepFailLogCount{0};
        const int failCount = s_reflexSleepFailLogCount.fetch_add(1, std::memory_order_relaxed);
        if (failCount < 5) {
            HookLogImportant("Streamline Hook: slReflexSleep forward failed result=%d frame=%p", result, streamline_hook_frame);
        }
    }
    return result;

}


slResult Hooked_slReflexSetOptions(const slReflexOptions& streamline_hook_options) {


    auto originalReflexSetOptions = GetCallableOriginalReflexSetOptions();
    if (!originalReflexSetOptions) {
        return streamline_hook_kSlResultErrorInvalidState;
    }

    slReflexOptions adjustedOptions = streamline_hook_options;
    streamline_hook_g_ReflexSetOptionsObservedCount.fetch_add(1, std::memory_order_relaxed);
    streamline_hook_g_ReflexSetOptionsLastTickMs.store(GetTickCount64(), std::memory_order_relaxed);
    streamline_hook_g_ReflexLastForwardedMode.store(streamline_hook_options.mode, std::memory_order_relaxed);
    const uint32_t targetIntervalUs = g_ReflexLimiter.GetTargetIntervalUs();
    const auto frameLimitForwarding = ce::streamline_runtime_policy::ResolveStreamlineReflexFrameLimitForwarding(
        streamline_hook_options.frameLimitUs, targetIntervalUs);
    adjustedOptions.frameLimitUs = frameLimitForwarding.frameLimitUs;
    HandleStreamlineReflexPacingSignal("slReflexSetOptions", streamline_hook_options.mode, streamline_hook_options.frameLimitUs,
                                       adjustedOptions.frameLimitUs, targetIntervalUs);

    const slResult result = originalReflexSetOptions(adjustedOptions);
    if (result == streamline_hook_kSlResultOk && frameLimitForwarding.overrideApplied) {
        HookLogImportant(
            "Streamline Hook: Overrode Reflex options frameLimitUs %u->%u (mode=%d incomingActive=%d)",
            streamline_hook_options.frameLimitUs, adjustedOptions.frameLimitUs, adjustedOptions.mode,
            ce::streamline_runtime_policy::IsStreamlineReflexPacingSignalActive(streamline_hook_options.mode, streamline_hook_options.frameLimitUs)
                ? 1
                : 0);
    } else if (result != streamline_hook_kSlResultOk) {
        static std::atomic<int> s_reflexSetOptionsFailLogCount{0};
        const int failCount = s_reflexSetOptionsFailLogCount.fetch_add(1, std::memory_order_relaxed);
        if (failCount < 5) {
            HookLogImportant("Streamline Hook: slReflexSetOptions forward failed result=%d mode=%d frameLimitUs=%u",
                             result, streamline_hook_options.mode, streamline_hook_options.frameLimitUs);
        }
    }
    return result;

}


slResult Hooked_slReflexSetConstants(const SLReflexConstants& streamline_hook_consts) {


    auto originalReflexSetConstants = GetCallableOriginalReflexSetConstants();
    if (!originalReflexSetConstants) {
        return streamline_hook_kSlResultErrorInvalidState;
    }

    SLReflexConstants adjustedConsts = streamline_hook_consts;
    const uint32_t targetIntervalUs = g_ReflexLimiter.GetTargetIntervalUs();

    // The legacy constants path only receives CE's frame-limit override when Reflex
    // is actually active, preserving the existing native Streamline behavior.
    if (streamline_hook_consts.mode >= streamline_hook_kSLReflexModeEnabled) {
        const auto frameLimitForwarding = ce::streamline_runtime_policy::ResolveStreamlineReflexFrameLimitForwarding(
            streamline_hook_consts.frameLimitUs, targetIntervalUs);
        adjustedConsts.frameLimitUs = frameLimitForwarding.frameLimitUs;
    }
    HandleStreamlineReflexPacingSignal("slReflexSetConstants", streamline_hook_consts.mode, streamline_hook_consts.frameLimitUs,
                                       adjustedConsts.frameLimitUs, targetIntervalUs);

    // Forward to the real slReflexSetConstants
    const slResult result = originalReflexSetConstants(adjustedConsts);
    if (result == streamline_hook_kSlResultOk && adjustedConsts.frameLimitUs != streamline_hook_consts.frameLimitUs) {
        HookLogImportant("Streamline Hook: Overrode Reflex constants frameLimitUs %u->%u (mode=%d)",
                         streamline_hook_consts.frameLimitUs, adjustedConsts.frameLimitUs, adjustedConsts.mode);
    } else if (result != streamline_hook_kSlResultOk) {
        static std::atomic<int> s_reflexSetConstantsFailLogCount{0};
        const int failCount = s_reflexSetConstantsFailLogCount.fetch_add(1, std::memory_order_relaxed);
        if (failCount < 5) {
            HookLogImportant("Streamline Hook: slReflexSetConstants forward failed result=%d mode=%d frameLimitUs=%u",
                             result, streamline_hook_consts.mode, streamline_hook_consts.frameLimitUs);
        }
    }
    return result;

}
