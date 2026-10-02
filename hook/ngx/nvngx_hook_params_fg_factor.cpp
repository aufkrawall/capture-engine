#include "nvngx_hook_internal.h"
#include "remix_hook.h"

namespace {

int ReadIntegerParameter(NVSDK_NGX_Parameter* params, const ParameterVTableOriginals& originals,
                         const char* name) {
    int value = 0;
    if (originals.getI && originals.getI(params, name, &value) == NVSDK_NGX_Result_Success)
        return value;
    unsigned int unsignedValue = 0;
    if (originals.getUI && originals.getUI(params, name, &unsignedValue) == NVSDK_NGX_Result_Success)
        return static_cast<int>(unsignedValue);
    return 0;
}

}  // namespace

int ResolveAndApplyFGFactorForEvaluation(NVSDK_NGX_Parameter* params) {
    if (!params)
        return 0;

    const int configuredMultiplier = GetConfiguredFGMultiplier(GetActiveGraphicsConfig());
    const ParameterVTableOriginals originals = GetParameterOriginals(params);
    int observedGeneratedFrames =
        ReadIntegerParameter(params, originals, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount);
    if (observedGeneratedFrames < 1 || observedGeneratedFrames > 3) {
        observedGeneratedFrames =
            ReadIntegerParameter(params, originals, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount_Unscoped);
    }
    const int observedLegacyMultiplier =
        ReadIntegerParameter(params, originals, NVSDK_NGX_Parameter_FrameGenerationMultiplier);
    const int observedMultiplier = ce::ngx_lifecycle::ResolveNVNGXObservedFrameGenerationMultiplier(
        observedGeneratedFrames, observedLegacyMultiplier);
    const int evaluatedMultiplier = ce::ngx_lifecycle::ResolveNVNGXEvaluatedFrameGenerationMultiplier(
        configuredMultiplier, observedGeneratedFrames, observedLegacyMultiplier);

    if (configuredMultiplier <= 0) {
        if (evaluatedMultiplier > 0) {
            static std::atomic<int> lastObservedMultiplier{0};
            const int previous = lastObservedMultiplier.exchange(evaluatedMultiplier, std::memory_order_acq_rel);
            if (previous != evaluatedMultiplier) {
                HookLogImportant(
                    "NVNGX FG: live EvaluateFeature parameters report %dx output "
                    "(DLSSG.MultiFrameCount=%d FrameGenerationMultiplier=%d)",
                    evaluatedMultiplier, observedGeneratedFrames, observedLegacyMultiplier);
            }
        }
        return evaluatedMultiplier;
    }

    if (!originals.setI && !originals.setUI) {
        static std::atomic<bool> missingSetterLogged{false};
        if (!missingSetterLogged.exchange(true, std::memory_order_acq_rel)) {
            HookLogImportant(
                "NVNGX FG: cannot enforce the configured runtime factor because the tracked parameter vtable "
                "has no captured setter; reporting the observed live factor instead");
        }
        return observedMultiplier;
    }

    const int generatedFrames = static_cast<int>(DLSSFGMultiplierToGeneratedFrames(configuredMultiplier));

    // This is the final parameter boundary before NGX consumes the evaluation.
    // Older Remix helpers can bypass the Set hooks through a captured original
    // vtable. Synchronize the upstream scheduler from the live mismatch too;
    // the Remix policy edge-deduplicates the normal setter-hook path.
    if (observedGeneratedFrames > 0) {
        RemixHook::ReassertFrameGenerationScheduleFromNgx(
            NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, static_cast<uint32_t>(observedGeneratedFrames),
            static_cast<uint32_t>(generatedFrames));
    }

    if (originals.setI) {
        originals.setI(params, NVSDK_NGX_Parameter_FrameGenerationMultiplier, configuredMultiplier);
        originals.setI(params, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount, generatedFrames);
        originals.setI(params, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount_Unscoped, generatedFrames);
    }
    if (originals.setUI) {
        originals.setUI(params, NVSDK_NGX_Parameter_FrameGenerationMultiplier,
                        static_cast<unsigned int>(configuredMultiplier));
        originals.setUI(params, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount,
                        static_cast<unsigned int>(generatedFrames));
        originals.setUI(params, NVSDK_NGX_DLSSG_Parameter_MultiFrameCount_Unscoped,
                        static_cast<unsigned int>(generatedFrames));
    }

    if (observedMultiplier != 0 && observedMultiplier != configuredMultiplier) {
        static std::atomic<uint32_t> changedValueLogs{0};
        const uint32_t logIndex = changedValueLogs.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 16) {
            HookLogImportant(
                "NVNGX FG: runtime EvaluateFeature factor changed to %dx; reasserted configured %dx "
                "(DLSSG.MultiFrameCount %d -> %d)",
                observedMultiplier, configuredMultiplier, observedGeneratedFrames, generatedFrames);
        }
    }
    return configuredMultiplier;
}
