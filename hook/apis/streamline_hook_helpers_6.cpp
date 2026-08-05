#include "streamline_hook_internal.h"


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
    return originalEvaluateFeature(feature, streamline_hook_frame, inputs, numInputs, streamline_hook_commandBuffer);

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
