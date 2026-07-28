
slResult Hooked_slSetD3DDevice(void* d3dDevice) {
    auto originalSetD3DDevice = GetCallableOriginalSetD3DDevice();
    if (!originalSetD3DDevice) {
        return kSlResultErrorInvalidState;
    }

    ID3D12Device* acceptedD3D12Device = nullptr;
    if (d3dDevice) {
        static_cast<IUnknown*>(d3dDevice)->QueryInterface(IID_PPV_ARGS(&acceptedD3D12Device));
    }
    const bool isD3D12 = acceptedD3D12Device != nullptr;

    const slResult result = originalSetD3DDevice(d3dDevice);
    if (result == kSlResultOk) {
        ID3D12Device* previousAcceptedDevice = nullptr;
        {
            std::lock_guard<std::mutex> lock(g_AcceptedD3D12DeviceMutex);
            previousAcceptedDevice = g_AcceptedD3D12Device;
            g_AcceptedD3D12Device = acceptedD3D12Device;
            acceptedD3D12Device = nullptr;
        }
        if (previousAcceptedDevice) {
            previousAcceptedDevice->Release();
        }
        g_StreamlineUsesD3D12.store(isD3D12, std::memory_order_release);
        if (isD3D12 && !ShouldKeepPureObserverOnlyStreamlineBehavior()) {
            // Resource tags are legal immediately after Streamline accepts the device. Some
            // integrations (Talos) publish their reusable UI tag before resolving any DLSS-G
            // feature function, so GetState-pointer delivery is too late to cover that tag.
            ce::dx12_streamline_ui_overlay::BeginPreactivationStandby(2);
            HookLogImportant(
                "Streamline Hook: D3D12 device accepted — official UI preactivation standby ready before tags "
                "(device=%p)",
                d3dDevice);
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

bool StructTypesEqual(const slStructType& lhs, const slStructType& rhs) {
    return lhs.data1 == rhs.data1 && lhs.data2 == rhs.data2 && lhs.data3 == rhs.data3 &&
           std::memcmp(lhs.data4, rhs.data4, sizeof(lhs.data4)) == 0;
}

bool TryRecordOfficialUiResourceTag(const void* frameToken, const slResourceTag& tag, void* commandBuffer) {
    if (ShouldKeepPureObserverOnlyStreamlineBehavior() || !g_StreamlineUsesD3D12.load(std::memory_order_acquire) ||
        !commandBuffer || tag.type != kSLBufferTypeUIColorAndAlpha || !tag.resource || !tag.resource->native ||
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
    request.commandList = static_cast<ID3D12GraphicsCommandList*>(commandBuffer);
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

uint32_t LogOfficialUiTagOpportunity(const char* tagApi, const void* frameToken, uint32_t viewportKey,
                                     const slResourceTag* tags, uint32_t numTags, void* commandBuffer,
                                     uint32_t feature = UINT_MAX, uint32_t numInputs = 0) {
    static std::atomic<uint32_t> s_uiTagOpportunityLogCount{0};
    const uint32_t opportunity = s_uiTagOpportunityLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (opportunity > 12 && (opportunity % 300) != 0) {
        return 0;
    }

    HookLogImportant(
        "Streamline Hook: Official UI tag record opportunity #%u (api=%s feature=%u frame=%p viewport=%u "
        "tags=%p numTags=%u inputs=%u commandBuffer=%p d3d12=%d)",
        opportunity, tagApi ? tagApi : "unknown", feature, frameToken, viewportKey, tags, numTags, numInputs,
        commandBuffer, g_StreamlineUsesD3D12.load(std::memory_order_relaxed) ? 1 : 0);
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

void TryRecordOfficialUiTag(const char* tagApi, const void* frameToken, const slViewportHandle& viewport,
                            const slResourceTag* tags, uint32_t numTags, void* commandBuffer) {
    const bool wantsUiBootstrapRecord = ce::dx12_streamline_ui_overlay::OnFrameTag(frameToken);

    if (wantsUiBootstrapRecord) {
        LogOfficialUiTagOpportunity(tagApi, frameToken, GetViewportKey(viewport), tags, numTags, commandBuffer);
    }

    // DLSS-G consumes UIColorAndAlpha before its first generated output exists, while PostSL can
    // only run after that output has been produced. Record CE's rolling/one-shot overlay into the
    // official UI layer on the app-provided command list. Source frames keep replacing the
    // eValidUntilPresent record until PostSL consumes the bounded output handoff. This introduces
    // no copy, extra submission, queue, or wait and naturally follows Streamline's synchronization.
    if (wantsUiBootstrapRecord && tags) {
        for (uint32_t i = 0; i < numTags; ++i) {
            if (TryRecordOfficialUiResourceTag(frameToken, tags[i], commandBuffer)) {
                break;
            }
        }
    }
}

slResult Hooked_slSetTag(const slViewportHandle& viewport, const slResourceTag* tags, uint32_t numTags,
                         void* commandBuffer) {
    auto originalSetTag = GetCallableOriginalSetTag();
    if (!originalSetTag) {
        return kSlResultErrorInvalidState;
    }

    // Legacy/global resource tagging has no frame token. A monotonically unique opaque identity
    // lets the standby state roll across calls without dereferencing or fabricating an SL object.
    static std::atomic<uintptr_t> s_legacyTagToken{1};
    const uintptr_t tokenValue = s_legacyTagToken.fetch_add(1, std::memory_order_relaxed);
    const void* frameToken = reinterpret_cast<const void*>((tokenValue << 1u) | 1u);
    TryRecordOfficialUiTag("slSetTag", frameToken, viewport, tags, numTags, commandBuffer);

    return originalSetTag(viewport, tags, numTags, commandBuffer);
}

slResult Hooked_slSetTagForFrame(const slBaseStructure& frame, const slViewportHandle& viewport,
                                 const slResourceTag* tags, uint32_t numTags, void* commandBuffer) {
    auto originalSetTagForFrame = GetCallableOriginalSetTagForFrame();
    if (!originalSetTagForFrame) {
        return kSlResultErrorInvalidState;
    }

    TryRecordOfficialUiTag("slSetTagForFrame", &frame, viewport, tags, numTags, commandBuffer);

    // Streamline observes the resource only after CE's commands have been appended. For volatile
    // tags this is essential: any copy Streamline records into the same command list includes CE.
    return originalSetTagForFrame(frame, viewport, tags, numTags, commandBuffer);
}

slResult Hooked_slEvaluateFeature(uint32_t feature, const slBaseStructure& frame, const slBaseStructure** inputs,
                                  uint32_t numInputs, void* commandBuffer) {
    auto originalEvaluateFeature = GetCallableOriginalEvaluateFeature();
    if (!originalEvaluateFeature) {
        return kSlResultErrorInvalidState;
    }

    // Streamline explicitly permits ResourceTag structures as local evaluate inputs; those tags
    // never pass through slSetTag/slSetTagForFrame. Talos uses this route. Keep the steady-state
    // evaluate path to one atomic branch, then inspect only the short activation/standby window.
    const void* frameToken = &frame;
    const bool wantsUiBootstrapRecord = ce::dx12_streamline_ui_overlay::OnFrameTag(frameToken);
    if (wantsUiBootstrapRecord) {
        slViewportHandle viewport;
        uint32_t localTagCount = 0;
        constexpr uint32_t kMaximumInputChainDepth = 16;
        if (inputs) {
            for (uint32_t i = 0; i < numInputs; ++i) {
                const slBaseStructure* input = inputs[i];
                for (uint32_t depth = 0; input && depth < kMaximumInputChainDepth; ++depth) {
                    if (StructTypesEqual(input->structType, kViewportHandleStructType)) {
                        viewport.value = static_cast<const slViewportHandle*>(input)->value;
                    }
                    if (StructTypesEqual(input->structType, kResourceTagStructType)) {
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
                                        localTagCount, commandBuffer, feature, numInputs);
        uint32_t tagIndex = 0;
        bool recorded = false;
        if (inputs) {
            for (uint32_t i = 0; i < numInputs && !recorded; ++i) {
                const slBaseStructure* input = inputs[i];
                for (uint32_t depth = 0; input && depth < kMaximumInputChainDepth; ++depth) {
                    if (StructTypesEqual(input->structType, kResourceTagStructType)) {
                        const auto& tag = *static_cast<const slResourceTag*>(input);
                        if (opportunity != 0 && tagIndex < 12) {
                            HookLogImportant(
                                "Streamline Hook: UI tag opportunity #%u localTag[%u] input=%u depth=%u type=%u "
                                "lifecycle=%d resource=%p extent=(%u,%u %ux%u)",
                                opportunity, tagIndex, i, depth, tag.type, tag.lifecycle, tag.resource, tag.extent.left,
                                tag.extent.top, tag.extent.width, tag.extent.height);
                        }
                        ++tagIndex;
                        recorded = TryRecordOfficialUiResourceTag(frameToken, tag, commandBuffer);
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
    return originalEvaluateFeature(feature, frame, inputs, numInputs, commandBuffer);
}

// Hook for Streamline Reflex sleep. This lets CE observe game-owned Reflex
// pacing without patching NvAPI_D3D_Sleep inside nvapi64.dll.
slResult Hooked_slReflexSleep(const void* frame) {
    auto originalReflexSleep = GetCallableOriginalReflexSleep();
    if (!originalReflexSleep) {
        return kSlResultErrorInvalidState;
    }

    // DLSSG-health evidence only: relaxed atomics + GetTickCount64 (shared-page read). No locks, no
    // logging, no syscalls — the manual Reflex FPS limiter's latency path through this hook is unchanged.
    g_ReflexSleepObservedCount.fetch_add(1, std::memory_order_relaxed);
    g_ReflexSleepLastTickMs.store(GetTickCount64(), std::memory_order_relaxed);

    g_ReflexLimiter.ApplyHybridPacingBeforeNativeSleep();

    const slResult result = originalReflexSleep(frame);
    if (result == kSlResultOk) {
        g_ReflexLimiter.MarkGameSleep("Streamline");
        g_ReflexLimiter.MarkNativePacingSignal();
    } else {
        static std::atomic<int> s_reflexSleepFailLogCount{0};
        const int failCount = s_reflexSleepFailLogCount.fetch_add(1, std::memory_order_relaxed);
        if (failCount < 5) {
            HookLogImportant("Streamline Hook: slReflexSleep forward failed result=%d frame=%p", result, frame);
        }
    }
    return result;
}

// Hook for current Streamline Reflex options — detects low-latency and FPS limiter signals.
slResult Hooked_slReflexSetOptions(const slReflexOptions& options) {
    auto originalReflexSetOptions = GetCallableOriginalReflexSetOptions();
    if (!originalReflexSetOptions) {
        return kSlResultErrorInvalidState;
    }

    slReflexOptions adjustedOptions = options;
    g_ReflexSetOptionsObservedCount.fetch_add(1, std::memory_order_relaxed);
    g_ReflexSetOptionsLastTickMs.store(GetTickCount64(), std::memory_order_relaxed);
    g_ReflexLastForwardedMode.store(options.mode, std::memory_order_relaxed);
    const uint32_t targetIntervalUs = g_ReflexLimiter.GetTargetIntervalUs();
    const auto frameLimitForwarding = ce::streamline_runtime_policy::ResolveStreamlineReflexFrameLimitForwarding(
        options.frameLimitUs, targetIntervalUs);
    adjustedOptions.frameLimitUs = frameLimitForwarding.frameLimitUs;
    HandleStreamlineReflexPacingSignal("slReflexSetOptions", options.mode, options.frameLimitUs,
                                       adjustedOptions.frameLimitUs, targetIntervalUs);

    const slResult result = originalReflexSetOptions(adjustedOptions);
    if (result == kSlResultOk && frameLimitForwarding.overrideApplied) {
        HookLogImportant(
            "Streamline Hook: Overrode Reflex options frameLimitUs %u->%u (mode=%d incomingActive=%d)",
            options.frameLimitUs, adjustedOptions.frameLimitUs, adjustedOptions.mode,
            ce::streamline_runtime_policy::IsStreamlineReflexPacingSignalActive(options.mode, options.frameLimitUs)
                ? 1
                : 0);
    } else if (result != kSlResultOk) {
        static std::atomic<int> s_reflexSetOptionsFailLogCount{0};
        const int failCount = s_reflexSetOptionsFailLogCount.fetch_add(1, std::memory_order_relaxed);
        if (failCount < 5) {
            HookLogImportant("Streamline Hook: slReflexSetOptions forward failed result=%d mode=%d frameLimitUs=%u",
                             result, options.mode, options.frameLimitUs);
        }
    }
    return result;
}

// Hook for legacy slReflexSetConstants — detects when game activates Reflex via Streamline.
slResult Hooked_slReflexSetConstants(const SLReflexConstants& consts) {
    auto originalReflexSetConstants = GetCallableOriginalReflexSetConstants();
    if (!originalReflexSetConstants) {
        return kSlResultErrorInvalidState;
    }

    SLReflexConstants adjustedConsts = consts;
    const uint32_t targetIntervalUs = g_ReflexLimiter.GetTargetIntervalUs();

    // The legacy constants path only receives CE's frame-limit override when Reflex
    // is actually active, preserving the existing native Streamline behavior.
    if (consts.mode >= kSLReflexModeEnabled) {
        const auto frameLimitForwarding = ce::streamline_runtime_policy::ResolveStreamlineReflexFrameLimitForwarding(
            consts.frameLimitUs, targetIntervalUs);
        adjustedConsts.frameLimitUs = frameLimitForwarding.frameLimitUs;
    }
    HandleStreamlineReflexPacingSignal("slReflexSetConstants", consts.mode, consts.frameLimitUs,
                                       adjustedConsts.frameLimitUs, targetIntervalUs);

    // Forward to the real slReflexSetConstants
    const slResult result = originalReflexSetConstants(adjustedConsts);
    if (result == kSlResultOk && adjustedConsts.frameLimitUs != consts.frameLimitUs) {
        HookLogImportant("Streamline Hook: Overrode Reflex constants frameLimitUs %u->%u (mode=%d)",
                         consts.frameLimitUs, adjustedConsts.frameLimitUs, adjustedConsts.mode);
    } else if (result != kSlResultOk) {
        static std::atomic<int> s_reflexSetConstantsFailLogCount{0};
        const int failCount = s_reflexSetConstantsFailLogCount.fetch_add(1, std::memory_order_relaxed);
        if (failCount < 5) {
            HookLogImportant("Streamline Hook: slReflexSetConstants forward failed result=%d mode=%d frameLimitUs=%u",
                             result, consts.mode, consts.frameLimitUs);
        }
    }
    return result;
}

}  // namespace

namespace StreamlineHook {

ExternalOverlayPresentGuard::ExternalOverlayPresentGuard() {
    ++g_ExternalOverlayPresentGuardDepth;
}

ExternalOverlayPresentGuard::~ExternalOverlayPresentGuard() {
    if (g_ExternalOverlayPresentGuardDepth > 0) {
        --g_ExternalOverlayPresentGuardDepth;
    }
}

bool IsExternalOverlayPresentGuardActive() {
    return g_ExternalOverlayPresentGuardDepth > 0;
}

bool IsExternalOverlayPluginLookupGuardReady() {
    return g_SLGetPluginFunctionHooked.load(std::memory_order_acquire);
}

bool IsAcceptedD3D12Device(IUnknown* device) {
    if (!device) {
        return false;
    }

    IUnknown* candidateIdentity = nullptr;
    IUnknown* acceptedIdentity = nullptr;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(&candidateIdentity))) || !candidateIdentity) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(g_AcceptedD3D12DeviceMutex);
        if (g_AcceptedD3D12Device) {
            g_AcceptedD3D12Device->QueryInterface(IID_PPV_ARGS(&acceptedIdentity));
        }
    }
    const bool matches = acceptedIdentity && candidateIdentity == acceptedIdentity;
    if (acceptedIdentity) {
        acceptedIdentity->Release();
    }
    candidateIdentity->Release();
    return matches;
}

bool HasExplicitSetOptionsActivationForCurrentComeback() {
    // Provenance of the current comeback is tracked explicitly. Startup-window
    // OFF churn can temporarily re-arm provisional GetState suppression without
    // changing the fact that the live comeback itself was activated by a fresh
    // OFF->ON SetOptions edge.
    return g_CurrentComebackActivatedViaExplicitSetOptions.load(std::memory_order_acquire);
}

void Init() {
    std::lock_guard<std::mutex> lock(g_InitMutex);
    RegisterDynamicHooksOnce();

    const bool foundModule = ScanLoadedStreamlineModules();

    if (!foundModule) {
        if (!g_NoModulesLogged.exchange(true, std::memory_order_acq_rel)) {
            HookLog("Streamline Hook: No Streamline modules loaded yet; waiting for module load");
        }
    } else {
        g_NoModulesLogged.store(false, std::memory_order_release);
    }
}

void OnModuleUnloaded(const void* moduleBase, size_t moduleSizeBytes, const char* moduleBaseName) {
    if (!moduleBase || moduleSizeBytes == 0 ||
        !ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload(moduleBaseName)) {
        return;
    }

    // Runs under the loader lock: interlocked/atomic writes and lightweight
    // logging only. Do NOT take g_ModuleHookMutex here (InstallHooksForModule
    // holds it across GetProcAddress, which needs the loader lock).
    struct HookSlotView {
        const char* name;
        std::atomic<void*>* target;
        std::atomic<bool>* installed;
        void* volatile* original;
    };
    HookSlotView slots[] = {
        {"slGetFeatureFunction", &g_SLGetFeatureFunctionTarget, &g_SLGetFeatureFunctionHooked,
         reinterpret_cast<void* volatile*>(&g_Original_slGetFeatureFunction)},
        {"slGetPluginFunction", &g_SLGetPluginFunctionTarget, &g_SLGetPluginFunctionHooked,
         reinterpret_cast<void* volatile*>(&g_Original_slGetPluginFunction)},
        {"slSetD3DDevice", &g_SLSetD3DDeviceTarget, &g_SLSetD3DDeviceHooked,
         reinterpret_cast<void* volatile*>(&g_Original_slSetD3DDevice)},
        {"slSetTag", &g_SLSetTagTarget, &g_SLSetTagHooked, reinterpret_cast<void* volatile*>(&g_Original_slSetTag)},
        {"slSetTagForFrame", &g_SLSetTagForFrameTarget, &g_SLSetTagForFrameHooked,
         reinterpret_cast<void* volatile*>(&g_Original_slSetTagForFrame)},
        {"slEvaluateFeature", &g_SLEvaluateFeatureTarget, &g_SLEvaluateFeatureHooked,
         reinterpret_cast<void* volatile*>(&g_Original_slEvaluateFeature)},
        {"slDLSSGSetOptions", &g_DLSSGSetOptionsTarget, &g_DLSSGSetOptionsHooked,
         reinterpret_cast<void* volatile*>(&g_Original_slDLSSGSetOptions)},
        {"slDLSSGGetState", &g_DLSSGGetStateTarget, &g_DLSSGGetStateHooked,
         reinterpret_cast<void* volatile*>(&g_Original_slDLSSGGetState)},
        {"slReflexSleep", &g_ReflexSleepTarget, &g_ReflexSleepHooked,
         reinterpret_cast<void* volatile*>(&g_Original_slReflexSleep)},
        {"slReflexSetOptions", &g_ReflexSetOptionsTarget, &g_ReflexSetOptionsHooked,
         reinterpret_cast<void* volatile*>(&g_Original_slReflexSetOptions)},
        {"slReflexSetConstants", &g_ReflexSetConstantsTarget, &g_ReflexSetConstantsHooked,
         reinterpret_cast<void* volatile*>(&g_Original_slReflexSetConstants)},
    };

    int invalidatedSlots = 0;
    for (HookSlotView& slot : slots) {
        void* target = slot.target->load(std::memory_order_acquire);
        void* original = *slot.original;
        if (!ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(target, original, moduleBase,
                                                                                          moduleSizeBytes)) {
            continue;
        }
        // Clear the original first so detours fail safe (GetCallableOriginal*
        // returns null) before the installed flag re-arms installation.
        InterlockedExchangePointer(slot.original, nullptr);
        slot.target->store(nullptr, std::memory_order_release);
        slot.installed->store(false, std::memory_order_release);
        ++invalidatedSlots;
        HookLogImportant(
            "Streamline Hook: Invalidated %s hook slot for unloaded %s (target=%p original=%p base=%p size=0x%zX)",
            slot.name, moduleBaseName, target, original, moduleBase, moduleSizeBytes);
    }

    std::atomic<void*>* attemptedTargets[] = {
        &g_DLSSGSetOptionsImportFallbackAttemptedTarget,    &g_DLSSGGetStateImportFallbackAttemptedTarget,
        &g_ReflexSleepImportFallbackAttemptedTarget,        &g_ReflexSetOptionsImportFallbackAttemptedTarget,
        &g_ReflexSetConstantsImportFallbackAttemptedTarget,
    };
    for (std::atomic<void*>* attempted : attemptedTargets) {
        void* target = attempted->load(std::memory_order_acquire);
        if (ce::streamline_runtime_policy::IsStreamlineHookSlotInvalidatedByModuleUnload(target, nullptr, moduleBase,
                                                                                         moduleSizeBytes)) {
            attempted->store(nullptr, std::memory_order_release);
        }
    }

    const uint32_t moduleBit = GetModuleMaskBit(moduleBaseName);
    if (moduleBit != 0) {
        g_InstalledModuleMask.fetch_and(~moduleBit, std::memory_order_acq_rel);
        g_IATPatchesMask.fetch_and(~moduleBit, std::memory_order_acq_rel);
    }

    if (invalidatedSlots > 0 || moduleBit != 0) {
        HookLogImportant(
            "Streamline Hook: Module %s unloaded (base=%p size=0x%zX) — invalidated %d stale hook slot(s); the next "
            "load of this name re-installs hooks for the fresh instance",
            moduleBaseName, moduleBase, moduleSizeBytes, invalidatedSlots);
    }
}

void OnModuleLoaded(HMODULE module, const char* moduleNameOrPath) {
    if (!module || !ce::streamline_runtime_policy::ShouldInspectStreamlineModuleOnLoad(moduleNameOrPath)) {
        return;
    }

    g_NoModulesLogged.store(false, std::memory_order_release);
    const bool inspectedModule = InstallHooksForModule(module, moduleNameOrPath);
    bool resolvedDLSSG = false;
    bool resolvedReflex = false;
    const bool deferFeatureLookup =
        ce::streamline_runtime_policy::ShouldDeferStreamlineFeatureLookupDuringModuleLoad(true);
    if (!deferFeatureLookup) {
        resolvedDLSSG = TryResolveDLSSGFeatureHooks();
        resolvedReflex = TryResolveReflexFeatureHooks();
    } else if (inspectedModule) {
        static std::atomic<int> s_deferredLookupLogCount{0};
        const int logCount = s_deferredLookupLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (logCount <= 20 || (logCount % 100) == 0) {
            HookLogImportant(
                "Streamline Hook: Deferred proactive feature-function lookup during module load for %s (%p) "
                "(log=%d); direct exports/IAT hooks installed now, feature lookup will retry after slSetD3DDevice or "
                "app slGetFeatureFunction",
                GetModuleBaseName(moduleNameOrPath), module, logCount);
        }
    }

    if (inspectedModule || resolvedDLSSG || resolvedReflex) {
        HookLogImportant(
            "Streamline Hook: Fresh module load inspected %s (%p) "
            "slGetFeatureFunctionHooked=%d slGetPluginFunctionHooked=%d slSetD3DDeviceHooked=%d "
            "slSetTagHooked=%d slSetTagForFrameHooked=%d slEvaluateFeatureHooked=%d "
            "dlssgSetOptionsHooked=%d "
            "dlssgGetStateHooked=%d reflexSleepHooked=%d reflexSetOptionsHooked=%d reflexSetConstantsHooked=%d",
            GetModuleBaseName(moduleNameOrPath), module,
            g_SLGetFeatureFunctionHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_SLGetPluginFunctionHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_SLSetD3DDeviceHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_SLSetTagHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_SLSetTagForFrameHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_SLEvaluateFeatureHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_DLSSGSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_DLSSGGetStateHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_ReflexSleepHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_ReflexSetOptionsHooked.load(std::memory_order_acquire) ? 1 : 0,
            g_ReflexSetConstantsHooked.load(std::memory_order_acquire) ? 1 : 0);
    }
}

bool IsInitialized() {
    return g_DynamicHooksRegistered.load(std::memory_order_acquire);
}

bool IsDLSSFGRequestedViaStreamline() {
    return DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
}

void OnAuthoritativeFFXTakeover() {
    ce::dx12_streamline_ui_overlay::EndActivation("authoritative FFX takeover");
    size_t resetViewportCount = 0;
    size_t preservedCapabilityCount = 0;
    {
        std::lock_guard<std::mutex> lock(g_StateMutex);
        resetViewportCount = g_ViewportStates.size();
        preservedCapabilityCount = g_ViewportCapabilityMax.size();
        for (auto& [viewportKey, runtimeState] : g_ViewportStates) {
            runtimeState.active = false;
            runtimeState.multiplier = 0;
            runtimeState.generatedFrames = 0;
        }
    }

    {
        std::lock_guard<std::mutex> offLock(g_SuppressedOffMutex);
        if (g_SuppressedSetOptionsOffDuringStartup) {
            HookLogImportant(
                "Streamline Hook: Clearing suppressed slDLSSGSetOptions(OFF) due to authoritative FFX takeover");
            g_SuppressedSetOptionsOffDuringStartup = false;
        }
    }

    g_SuppressNewGetStateActivationUntilMs.store(GetTickCount64() + kAuthoritativeFFXTakeoverGetStateSuppressMs,
                                                 std::memory_order_release);
    g_BlockGetStateOnlyReactivationUntilSafePostFSRBootstrap.store(true, std::memory_order_release);
    g_CurrentComebackActivatedViaExplicitSetOptions.store(false, std::memory_order_release);
    g_AcceptedRuntimeOffAwaitingSetOptions.store(false, std::memory_order_release);
    g_ConfirmedDLSSReflexSuspendPending.store(false, std::memory_order_release);
    g_StartupWindowOffExtensionPending.store(false, std::memory_order_release);
    ResetStartupProtectedOffChurnActiveProof("authoritative FFX takeover");
    // A new FSR takeover resets the entire FG session context; any stale DLSS-only
    // reactivation block from a previous epoch must not outlive the FSR phase.
    const bool hadStaleExplicitSetOptionsBlock =
        g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.exchange(false, std::memory_order_acq_rel);
    HookLogImportant(
        "Streamline Hook: Authoritative FFX takeover reset %zu viewport states and preserved %zu capability caches; "
        "suppressing GetState-only reactivation for %llums and until safe post-FSR bootstrap or explicit enable "
        "(clearedStaleBlock=%d)",
        resetViewportCount, preservedCapabilityCount, (unsigned long long)kAuthoritativeFFXTakeoverGetStateSuppressMs,
        hadStaleExplicitSetOptionsBlock ? 1 : 0);
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kAuthoritativeFFXTakeover,
                                "StreamlineHook::OnAuthoritativeFFXTakeover", nullptr, nullptr,
                                ce::fg_runtime::RuntimeMode::kFSRFG, true, true);
}

void OnAuthoritativeStreamlineStartupHandoff() {
    g_SuppressNewGetStateActivationUntilMs.store(GetTickCount64() + kAuthoritativeFFXTakeoverGetStateSuppressMs,
                                                 std::memory_order_release);
    g_ConfirmedDLSSReflexSuspendPending.store(false, std::memory_order_release);
    g_StartupWindowOffExtensionPending.store(true, std::memory_order_release);
    ResetStartupProtectedOffChurnActiveProof("authoritative Streamline startup handoff");
    HookLogImportant(
        "Streamline Hook: Authoritative Streamline startup handoff observed — suppressing fresh GetState-only "
        "reactivation for %llums until explicit enable or stable startup evidence arrives",
        (unsigned long long)kAuthoritativeFFXTakeoverGetStateSuppressMs);
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kAuthoritativeStreamlineStartupHandoff,
                                "StreamlineHook::OnAuthoritativeStreamlineStartupHandoff", nullptr, nullptr,
                                ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false);
}

void Shutdown() {
    ce::dx12_streamline_ui_overlay::EndActivation("Streamline shutdown");
    std::lock_guard<std::mutex> lock(g_StateMutex);
    g_ViewportStates.clear();
    g_ViewportCapabilityMax.clear();
    g_SuppressNewGetStateActivationUntilMs.store(0, std::memory_order_release);
    g_BlockGetStateOnlyReactivationUntilExplicitSetOptions.store(false, std::memory_order_release);
    g_BlockGetStateOnlyReactivationUntilSafePostFSRBootstrap.store(false, std::memory_order_release);
    g_CurrentComebackActivatedViaExplicitSetOptions.store(false, std::memory_order_release);
    g_AcceptedRuntimeOffAwaitingSetOptions.store(false, std::memory_order_release);
    g_ConfirmedDLSSReflexSuspendPending.store(false, std::memory_order_release);
    g_StartupWindowOffExtensionPending.store(false, std::memory_order_release);
    ResetStartupProtectedOffChurnActiveProof("Streamline shutdown");
    {
        std::lock_guard<std::mutex> offLock(g_SuppressedOffMutex);
        g_SuppressedSetOptionsOffDuringStartup = false;
    }
    g_FGCompat.SetStreamlineFGSignal(false);
    g_FGCompat.SetDLSSFGMultiplier(0);
    g_FGCompat.SetDLSSFGActive(false);
    g_FGCompat.SetStreamlineSupportPresent(false);
    DXGIShared::g_StreamlineFGRunning.store(false, std::memory_order_release);
    g_StreamlineUsesD3D12.store(false, std::memory_order_release);
    ID3D12Device* acceptedDevice = nullptr;
    {
        std::lock_guard<std::mutex> deviceLock(g_AcceptedD3D12DeviceMutex);
        acceptedDevice = g_AcceptedD3D12Device;
        g_AcceptedD3D12Device = nullptr;
    }
    if (acceptedDevice) {
        acceptedDevice->Release();
    }
}

void FlushSuppressedSetOptionsOffIfNeeded() {
    if (ShouldKeepPureObserverOnlyStreamlineBehavior()) {
        return;
    }
