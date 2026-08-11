#include "nvngx_hook_internal.h"

namespace {

bool IsTrackedUpscalerFeature(int featureID) {
    return featureID == 1 || featureID == nvngx_hook_NVSDK_NGX_Feature_RayReconstruction;
}

void PublishEvaluatedFeature(int featureID, bool firstEvaluation) {
    if (!g_IPC || !g_IPC->GetSharedMem())
        return;

    auto& state = g_IPC->GetSharedMem()->dlssState;
    if (featureID == nvngx_hook_NVSDK_NGX_Feature_RayReconstruction) {
        state.rrActive.store(true, std::memory_order_release);
        state.srActive.store(false, std::memory_order_release);
        if (firstEvaluation) {
            static std::atomic<uint32_t> rrEvaluationLogs{0};
            if (rrEvaluationLogs.fetch_add(1, std::memory_order_relaxed) < 32) {
                HookLogImportant(
                    "NVNGX RR: Feature 13 evaluation succeeded; Ray Reconstruction is rendering "
                    "(forced UE policy confirmed at NGX execution)");
            }
        }
    } else if (featureID == 1) {
        state.srActive.store(true, std::memory_order_release);
        state.rrActive.store(false, std::memory_order_release);
        if (firstEvaluation && GetActiveGraphicsConfig().forceRayReconstruction) {
            static std::atomic<uint32_t> forcedSrLogs{0};
            if (forcedSrLogs.fetch_add(1, std::memory_order_relaxed) < 16) {
                HookLogImportant(
                    "NVNGX RR: Feature 1 (ordinary DLSS SR) evaluation succeeded while force policy is enabled; "
                    "the UE plugin did not select RR or fell back after RR creation failed");
            }
        } else if (firstEvaluation && g_IPC->GetSharedMem()->GetDebugLogging()) {
            NVNGXLog("NVNGX: Feature 1 evaluation confirmed DLSS SR rendering");
        }
    }
}

void RepublishFeatureStateAfterRelease(int releasedFeature) {
    if (!g_IPC || !g_IPC->GetSharedMem())
        return;

    auto& state = g_IPC->GetSharedMem()->dlssState;
    const bool rrPublished = state.rrActive.load(std::memory_order_acquire);
    const bool srPublished = state.srActive.load(std::memory_order_acquire);
    const bool rrRemains =
        nvngx_hook_g_FeatureRegistry.HasEvaluatedFeature(nvngx_hook_NVSDK_NGX_Feature_RayReconstruction);
    const bool srRemains = nvngx_hook_g_FeatureRegistry.HasEvaluatedFeature(1);

    // Releasing an old, inactive handle must not displace the feature that most
    // recently evaluated. Only fall back once the published feature has no
    // evaluated handle left; this also avoids an old RR handle winning merely
    // because RR has the larger feature ID.
    if (releasedFeature == nvngx_hook_NVSDK_NGX_Feature_RayReconstruction && rrPublished && !rrRemains) {
        state.rrActive.store(false, std::memory_order_release);
        state.srActive.store(srRemains, std::memory_order_release);
    } else if (releasedFeature == 1 && srPublished && !srRemains) {
        state.srActive.store(false, std::memory_order_release);
        state.rrActive.store(rrRemains, std::memory_order_release);
    }
}

NVSDK_NGX_Result ProcessEvaluateFeature(PFN_NVSDK_NGX_EvaluateFeature original, void* ctx, const void* handle,
                                        const NVSDK_NGX_Parameter* params, void* callback, const char* api) {
    if (!original)
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    if (HookIsShuttingDown())
        return original(ctx, handle, params, callback);

    const int expectedFeature = nvngx_hook_g_FeatureRegistry.FindFeature(const_cast<void*>(handle));
    const NVSDK_NGX_Result result = original(ctx, handle, params, callback);
    if (ce::ngx_lifecycle::IsSuccessfulResult(static_cast<uint32_t>(result))) {
        const auto evaluation = nvngx_hook_g_FeatureRegistry.MarkEvaluated(const_cast<void*>(handle));
        if (evaluation.found) {
            PublishEvaluatedFeature(evaluation.feature, evaluation.firstEvaluation);
        } else {
            static std::atomic<uint32_t> untrackedLogs{0};
            const uint32_t logIndex = untrackedLogs.fetch_add(1, std::memory_order_relaxed);
            if (logIndex < 8 && GetActiveGraphicsConfig().forceRayReconstruction) {
                HookLogImportant("NVNGX RR: %s evaluate succeeded for untracked handle=%p; CreateFeature hook was missed",
                                 api, handle);
            }
        }
    } else if (expectedFeature == nvngx_hook_NVSDK_NGX_Feature_RayReconstruction) {
        static std::atomic<uint32_t> failedEvaluationLogs{0};
        const uint32_t logIndex = failedEvaluationLogs.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 8 && GetActiveGraphicsConfig().forceRayReconstruction) {
            HookLogImportant("NVNGX RR: %s Feature 13 evaluation failed (result=0x%08X handle=%p)", api,
                             static_cast<unsigned int>(result), handle);
        }
    }
    return result;
}

NVSDK_NGX_Result ProcessReleaseFeature(PFN_NVSDK_NGX_ReleaseFeature original, void* handle, const char* api) {
    if (!original)
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    if (HookIsShuttingDown())
        return original(handle);
    const NVSDK_NGX_Result result = original(handle);
    if (!ce::ngx_lifecycle::IsSuccessfulResult(static_cast<uint32_t>(result)))
        return result;

    const auto removal = nvngx_hook_g_FeatureRegistry.Remove(handle);
    if (removal.found && removal.wasEvaluated) {
        if (IsTrackedUpscalerFeature(removal.feature))
            RepublishFeatureStateAfterRelease(removal.feature);
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
            NVNGXLog("NVNGX: %s released evaluated feature ID %d handle=%p", api, removal.feature, handle);
        }
    }
    return result;
}

}  // namespace

void TrackNgxFeatureCreation(int featureID, NVSDK_NGX_Result result, void** handle) {
    const bool upscalerFeature = IsTrackedUpscalerFeature(featureID);
    const bool forceRR = upscalerFeature && GetActiveGraphicsConfig().forceRayReconstruction;
    if (!ce::ngx_lifecycle::IsSuccessfulResult(static_cast<uint32_t>(result)) || !handle || !*handle) {
        if (featureID == nvngx_hook_NVSDK_NGX_Feature_RayReconstruction && forceRR) {
            static std::atomic<uint32_t> rrCreateFailureLogs{0};
            if (rrCreateFailureLogs.fetch_add(1, std::memory_order_relaxed) < 16) {
                HookLogImportant(
                    "NVNGX RR: Feature 13 creation failed or returned no handle (result=0x%08X); "
                    "ordinary SR fallback remains unblocked to avoid a missing frame output",
                    static_cast<unsigned int>(result));
            }
        }
        return;
    }

    // Record every NGX handle so a later evaluation is "untracked" only when
    // CreateFeature interception was genuinely missed, not merely because the
    // handle belongs to FG or another feature we do not publish as an upscaler.
    const auto recorded = nvngx_hook_g_FeatureRegistry.RecordCreated(*handle, featureID);
    if (recorded == ce::ngx_lifecycle::RecordResult::kFull) {
        static std::atomic<bool> fullLogged{false};
        if (!fullLogged.exchange(true, std::memory_order_acq_rel)) {
            HookLogImportant("NVNGX RR: feature-handle registry is full; later evaluation evidence may be incomplete");
        }
        return;
    }
    if (!upscalerFeature)
        return;

    if (featureID == nvngx_hook_NVSDK_NGX_Feature_RayReconstruction) {
        static std::atomic<uint32_t> rrCreateLogs{0};
        if (rrCreateLogs.fetch_add(1, std::memory_order_relaxed) < 32) {
            HookLogImportant("NVNGX RR: Feature 13 created successfully (handle=%p); awaiting first evaluation",
                             *handle);
        }
    } else if (forceRR) {
        static std::atomic<uint32_t> forcedSrCreateLogs{0};
        if (forcedSrCreateLogs.fetch_add(1, std::memory_order_relaxed) < 16) {
            HookLogImportant(
                "NVNGX RR: ordinary SR Feature 1 created while force policy is enabled (handle=%p); "
                "evaluation will determine whether this is an active fallback",
                *handle);
        }
    }
}

NVSDK_NGX_Result __cdecl Hooked_EvaluateFeature_D3D11(void* ctx, const void* handle,
                                                       const NVSDK_NGX_Parameter* params, void* callback) {
    return ProcessEvaluateFeature(nvngx_hook_oEvaluateFeature_D3D11, ctx, handle, params, callback, "D3D11");
}

NVSDK_NGX_Result __cdecl Hooked_EvaluateFeature_D3D11_C(void* ctx, const void* handle,
                                                         const NVSDK_NGX_Parameter* params, void* callback) {
    return ProcessEvaluateFeature(nvngx_hook_oEvaluateFeature_D3D11_C, ctx, handle, params, callback, "D3D11_C");
}

NVSDK_NGX_Result __cdecl Hooked_EvaluateFeature_D3D12(void* ctx, const void* handle,
                                                       const NVSDK_NGX_Parameter* params, void* callback) {
    return ProcessEvaluateFeature(nvngx_hook_oEvaluateFeature_D3D12, ctx, handle, params, callback, "D3D12");
}

NVSDK_NGX_Result __cdecl Hooked_EvaluateFeature_D3D12_C(void* ctx, const void* handle,
                                                         const NVSDK_NGX_Parameter* params, void* callback) {
    return ProcessEvaluateFeature(nvngx_hook_oEvaluateFeature_D3D12_C, ctx, handle, params, callback, "D3D12_C");
}

NVSDK_NGX_Result __cdecl Hooked_EvaluateFeature_VULKAN(void* ctx, const void* handle,
                                                        const NVSDK_NGX_Parameter* params, void* callback) {
    return ProcessEvaluateFeature(nvngx_hook_oEvaluateFeature_VULKAN, ctx, handle, params, callback, "VULKAN");
}

NVSDK_NGX_Result __cdecl Hooked_EvaluateFeature_VULKAN_C(void* ctx, const void* handle,
                                                          const NVSDK_NGX_Parameter* params, void* callback) {
    return ProcessEvaluateFeature(nvngx_hook_oEvaluateFeature_VULKAN_C, ctx, handle, params, callback, "VULKAN_C");
}

NVSDK_NGX_Result __cdecl Hooked_ReleaseFeature_D3D11(void* handle) {
    return ProcessReleaseFeature(nvngx_hook_oReleaseFeature_D3D11, handle, "D3D11");
}

NVSDK_NGX_Result __cdecl Hooked_ReleaseFeature_D3D12(void* handle) {
    return ProcessReleaseFeature(nvngx_hook_oReleaseFeature_D3D12, handle, "D3D12");
}

NVSDK_NGX_Result __cdecl Hooked_ReleaseFeature_VULKAN(void* handle) {
    return ProcessReleaseFeature(nvngx_hook_oReleaseFeature_VULKAN, handle, "VULKAN");
}
