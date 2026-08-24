#include "nvngx_hook_internal.h"

namespace {

bool IsRayReconstructionCapabilityParameter(const char* name) {
    return name &&
           (strcmp(name, NVSDK_NGX_Parameter_SuperSamplingDenoising_Available) == 0 ||
            strcmp(name, NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult) == 0);
}

template <typename T>
void LogObservedRayReconstructionCapability(const char* name, NVSDK_NGX_Result result, const T* value,
                                            const char* getter) {
    if (!IsRayReconstructionCapabilityParameter(name) || !GetActiveGraphicsConfig().forceRayReconstruction)
        return;
    if (result == NVSDK_NGX_Result_Success && value) {
        LogOncePerParam(name, "NVNGX RR: observed real %s via %s = %lld (result=0x%08X; not spoofed)", name,
                        getter, static_cast<long long>(*value), static_cast<unsigned int>(result));
    } else {
        LogOncePerParam(name, "NVNGX RR: real %s query via %s failed (result=0x%08X; not spoofed)", name,
                        getter, static_cast<unsigned int>(result));
    }
}

}  // namespace

void STDMETHODCALLTYPE Hooked_SetI(NVSDK_NGX_Parameter* pThis, const char* InName, int InValue) {
    const PFN_SetI original = GetParameterOriginals(pThis).setI;
    if (!original)
        return;
    if (HookIsShuttingDown()) {
        original(pThis, InName, InValue);
        return;
    }
    if (IsSafeString(InName)) {
        if (strcmp(InName, NVSDK_NGX_Parameter_CreateFlags) == 0) {
            std::string mode = GetActiveGraphicsConfig().dlssAutoExposure;
            if (mode == "on") {
                if (!(InValue & nvngx_hook_NVSDK_NGX_DLSS_Feature_Flags_AutoExposure)) {
                    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                        LogOncePerParam(InName, "NVNGX: Forcing AutoExposure flag ON (was 0x%X)", InValue);
                    InValue |= nvngx_hook_NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
                }
            } else if (mode == "off") {
                if (InValue & nvngx_hook_NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) {
                    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                        LogOncePerParam(InName, "NVNGX: Forcing AutoExposure flag OFF (was 0x%X)", InValue);
                    InValue &= ~nvngx_hook_NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
                }
            }

            // DLSS sharpening is controlled by both a float parameter (Sharpness) AND
            // a feature flag. Setting sharpness to 0 does not necessarily disable
            // sharpening unless this bit is cleared.
            const auto& cfg = GetActiveGraphicsConfig();
            if (cfg.parsed.dlssSharpening > -1.5f) {
                // Treat explicit 0 as "off" (matches user expectation in our config).
                const bool forceDisableSharpening = (cfg.parsed.dlssSharpening < 0.0001f);
                if (forceDisableSharpening) {
                    if (InValue & nvngx_hook_NVSDK_NGX_DLSS_Feature_Flags_DoSharpening) {
                        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                            LogOncePerParam(InName, "NVNGX: Forcing DoSharpening flag OFF (was 0x%X)", InValue);
                        InValue &= ~nvngx_hook_NVSDK_NGX_DLSS_Feature_Flags_DoSharpening;
                    }
                } else {
                    if (!(InValue & nvngx_hook_NVSDK_NGX_DLSS_Feature_Flags_DoSharpening)) {
                        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                            LogOncePerParam(InName, "NVNGX: Forcing DoSharpening flag ON (was 0x%X)", InValue);
                        InValue |= nvngx_hook_NVSDK_NGX_DLSS_Feature_Flags_DoSharpening;
                    }
                }
            }
        } else if (strcmp(InName, NVSDK_NGX_Parameter_AutoExposure) == 0) {
            std::string mode = GetActiveGraphicsConfig().dlssAutoExposure;
            if (mode == "on")
                InValue = 1;
            else if (mode == "off")
                InValue = 0;
            if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                LogOncePerParam(InName, "NVNGX: Forcing AutoExposure to %d", InValue);
        } else {
            const auto& cfg = GetActiveGraphicsConfig();
            if (strcmp(InName, NVSDK_NGX_Parameter_FrameGenerationMultiplier) == 0) {
                const int fgMultiplier = GetConfiguredFGMultiplier(cfg);
                if (fgMultiplier > 0 && InValue != fgMultiplier) {
                    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                        LogOncePerParam(InName, "NVNGX: Overriding FrameGenerationMultiplier via SetI -> %d (was %d)",
                                        fgMultiplier, InValue);
                    InValue = fgMultiplier;
                }
            }

            // Preset Overrides (Super Resolution) via SetI
            bool isDlssPreset = false;
            uint32_t specificOverride = 0;

            if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA) == 0) {
                isDlssPreset = true;
                specificOverride = cfg.parsed.presetDLAA;
            } else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality) == 0) {
                isDlssPreset = true;
                specificOverride = cfg.parsed.presetQuality;
            } else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced) == 0) {
                isDlssPreset = true;
                specificOverride = cfg.parsed.presetBalanced;
            } else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance) == 0) {
                isDlssPreset = true;
                specificOverride = cfg.parsed.presetPerformance;
            } else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance) == 0) {
                isDlssPreset = true;
                specificOverride = cfg.parsed.presetUltraPerformance;
            } else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality) == 0) {
                isDlssPreset = true;
                specificOverride = cfg.parsed.presetUltraQuality;
            }

            if (isDlssPreset) {
                uint32_t val = (specificOverride > 0) ? specificOverride : cfg.parsed.srPreset;
                if (val > 0) {
                    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
                        // Metered diagnostic: the game can re-apply the same
                        // override every frame during startup/activation; log
                        // once per changed (param, value) pair instead of every
                        // call.
                        LogOncePerParam(InName, "NVNGX: SetI: Overriding %s: %d -> %u", InName, InValue, val);
                    }
                    InValue = (int)val;
                    UpdatePresetHint(InName, val, "SetI_Override");
                } else {
                    UpdatePresetHint(InName, (uint32_t)InValue, "SetI_Game");
                }
            } else {
                // Preset Overrides (Ray Reconstruction) via SetI
                bool isRRPreset = false;
                uint32_t specificRROverride = 0;

                if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA) == 0) {
                    isRRPreset = true;
                    specificRROverride = cfg.parsed.rrPresetDLAA;
                } else if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality) == 0) {
                    isRRPreset = true;
                    specificRROverride = cfg.parsed.rrPresetQuality;
                } else if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced) == 0) {
                    isRRPreset = true;
                    specificRROverride = cfg.parsed.rrPresetBalanced;
                } else if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance) == 0) {
                    isRRPreset = true;
                    specificRROverride = cfg.parsed.rrPresetPerformance;
                } else if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance) ==
                           0) {
                    isRRPreset = true;
                    specificRROverride = cfg.parsed.rrPresetUltraPerformance;
                } else if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality) == 0) {
                    isRRPreset = true;
                    specificRROverride = cfg.parsed.rrPresetUltraQuality;
                }

                if (isRRPreset) {
                    uint32_t finalRRPreset = specificRROverride > 0 ? specificRROverride : cfg.parsed.rrPreset;
                    if (finalRRPreset > 0) {
                        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                            LogOncePerParam(InName, "NVNGX: Overriding RR preset (SetI) %s = %u (was %d)", InName,
                                            finalRRPreset, InValue);
                        InValue = (int)finalRRPreset;
                    }
                }
            }
        }

        // Capture Quality Mode for Overlay
        if (strcmp(InName, NVSDK_NGX_Parameter_PerfQualityValue) == 0) {
            std::lock_guard<std::mutex> lock(nvngx_hook_g_ParamMapMutex);
            nvngx_hook_g_ParameterQualityMap[pThis] = InValue;
        }
    }
    original(pThis, InName, InValue);
}

void STDMETHODCALLTYPE Hooked_SetUI(NVSDK_NGX_Parameter* pThis, const char* InName, unsigned int InValue) {
    const PFN_SetUI original = GetParameterOriginals(pThis).setUI;
    if (!original)
        return;
    if (HookIsShuttingDown()) {
        original(pThis, InName, InValue);
        return;
    }
    if (IsSafeString(InName)) {
        if (strcmp(InName, NVSDK_NGX_Parameter_CreateFlags) == 0) {
            // AutoExposure flag override
            std::string mode = GetActiveGraphicsConfig().dlssAutoExposure;
            if (mode == "on") {
                if (!(InValue & nvngx_hook_NVSDK_NGX_DLSS_Feature_Flags_AutoExposure)) {
                    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                        LogOncePerParam(InName, "NVNGX: Forcing AutoExposure flag ON (SetUI, was 0x%X)", InValue);
                    InValue |= nvngx_hook_NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
                }
            } else if (mode == "off") {
                if (InValue & nvngx_hook_NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) {
                    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                        LogOncePerParam(InName, "NVNGX: Forcing AutoExposure flag OFF (SetUI, was 0x%X)", InValue);
                    InValue &= ~nvngx_hook_NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
                }
            }
            // DoSharpening flag override
            const auto& cfg = GetActiveGraphicsConfig();
            if (cfg.parsed.dlssSharpening > -1.5f) {
                const bool forceDisableSharpening = (cfg.parsed.dlssSharpening < 0.0001f);
                if (forceDisableSharpening) {
                    if (InValue & nvngx_hook_NVSDK_NGX_DLSS_Feature_Flags_DoSharpening) {
                        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                            LogOncePerParam(InName, "NVNGX: Forcing DoSharpening flag OFF (SetUI, was 0x%X)", InValue);
                        InValue &= ~nvngx_hook_NVSDK_NGX_DLSS_Feature_Flags_DoSharpening;
                    }
                } else {
                    if (!(InValue & nvngx_hook_NVSDK_NGX_DLSS_Feature_Flags_DoSharpening)) {
                        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                            LogOncePerParam(InName, "NVNGX: Forcing DoSharpening flag ON (SetUI, was 0x%X)", InValue);
                        InValue |= nvngx_hook_NVSDK_NGX_DLSS_Feature_Flags_DoSharpening;
                    }
                }
            }
        } else if (strcmp(InName, NVSDK_NGX_Parameter_AutoExposure) == 0) {
            std::string mode = GetActiveGraphicsConfig().dlssAutoExposure;
            if (mode == "on")
                InValue = 1;
            else if (mode == "off")
                InValue = 0;
        } else {
            const auto& cfg = GetActiveGraphicsConfig();
            if (strcmp(InName, NVSDK_NGX_Parameter_FrameGenerationMultiplier) == 0) {
                const int fgMultiplier = GetConfiguredFGMultiplier(cfg);
                if (fgMultiplier > 0 && InValue != (unsigned int)fgMultiplier) {
                    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                        LogOncePerParam(InName, "NVNGX: Overriding FrameGenerationMultiplier via SetUI -> %d (was %u)",
                                        fgMultiplier, InValue);
                    InValue = (unsigned int)fgMultiplier;
                }
            }

            // Preset Overrides (Super Resolution)
            bool isDlssPreset = false;
            uint32_t specificOverride = 0;

            if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA) == 0) {
                isDlssPreset = true;
                specificOverride = cfg.parsed.presetDLAA;
            } else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality) == 0) {
                isDlssPreset = true;
                specificOverride = cfg.parsed.presetQuality;
            } else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced) == 0) {
                isDlssPreset = true;
                specificOverride = cfg.parsed.presetBalanced;
            } else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance) == 0) {
                isDlssPreset = true;
                specificOverride = cfg.parsed.presetPerformance;
            } else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance) == 0) {
                isDlssPreset = true;
                specificOverride = cfg.parsed.presetUltraPerformance;
            } else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality) == 0) {
                isDlssPreset = true;
                specificOverride = cfg.parsed.presetUltraQuality;
            }

            if (isDlssPreset) {
                uint32_t val = (specificOverride > 0) ? specificOverride : cfg.parsed.srPreset;

                // Debug: Log the override decision
                static int setUILogCount = 0;
                if (setUILogCount < 10 && g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
                    NVNGXLog("NVNGX: SetUI(%s): game=%u, specific=%u, global=%u, final=%u", InName, InValue,
                             specificOverride, cfg.parsed.srPreset, val);
                    setUILogCount++;
                }

                if (val > 0) {
                    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                        LogOncePerParam(InName, "NVNGX: Overriding %s via SetUI -> %u", InName, val);
                    InValue = val;
                    UpdatePresetHint(InName, val, "SetUI_Override");
                } else {
                    UpdatePresetHint(InName, InValue, "SetUI_Game");
                }
            } else {
                // Preset Overrides (Ray Reconstruction)
                bool isRRPreset = false;
                uint32_t specificRROverride = 0;

                if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA) == 0) {
                    isRRPreset = true;
                    specificRROverride = cfg.parsed.rrPresetDLAA;
                } else if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality) == 0) {
                    isRRPreset = true;
                    specificRROverride = cfg.parsed.rrPresetQuality;
                } else if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced) == 0) {
                    isRRPreset = true;
                    specificRROverride = cfg.parsed.rrPresetBalanced;
                } else if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance) == 0) {
                    isRRPreset = true;
                    specificRROverride = cfg.parsed.rrPresetPerformance;
                } else if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance) ==
                           0) {
                    isRRPreset = true;
                    specificRROverride = cfg.parsed.rrPresetUltraPerformance;
                } else if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality) == 0) {
                    isRRPreset = true;
                    specificRROverride = cfg.parsed.rrPresetUltraQuality;
                }

                if (isRRPreset) {
                    uint32_t finalRRPreset = specificRROverride > 0 ? specificRROverride : cfg.parsed.rrPreset;
                    if (finalRRPreset > 0) {
                        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                            LogOncePerParam(InName, "NVNGX: Overriding RR preset %s = %u (was %u)", InName,
                                            finalRRPreset, InValue);
                        InValue = finalRRPreset;
                    }
                }
            }
        }

        // Capture Quality/Dimensions for Overlay
        if (strcmp(InName, NVSDK_NGX_Parameter_PerfQualityValue) == 0) {
            std::lock_guard<std::mutex> lock(nvngx_hook_g_ParamMapMutex);
            nvngx_hook_g_ParameterQualityMap[pThis] = (int)InValue;
        } else if (strcmp(InName, NVSDK_NGX_Parameter_Width) == 0) {
            std::lock_guard<std::mutex> lock(nvngx_hook_g_ParamMapMutex);
            nvngx_hook_g_ParameterDimsMap[pThis].width = InValue;
        } else if (strcmp(InName, NVSDK_NGX_Parameter_Height) == 0) {
            std::lock_guard<std::mutex> lock(nvngx_hook_g_ParamMapMutex);
            nvngx_hook_g_ParameterDimsMap[pThis].height = InValue;
        } else if (strcmp(InName, NVSDK_NGX_Parameter_OutWidth) == 0) {
            std::lock_guard<std::mutex> lock(nvngx_hook_g_ParamMapMutex);
            nvngx_hook_g_ParameterDimsMap[pThis].outWidth = InValue;
        } else if (strcmp(InName, NVSDK_NGX_Parameter_OutHeight) == 0) {
            std::lock_guard<std::mutex> lock(nvngx_hook_g_ParamMapMutex);
            nvngx_hook_g_ParameterDimsMap[pThis].outHeight = InValue;
        }
    }
    original(pThis, InName, InValue);
}

void STDMETHODCALLTYPE Hooked_SetF(NVSDK_NGX_Parameter* pThis, const char* InName, float InValue) {
    const PFN_SetF original = GetParameterOriginals(pThis).setF;
    if (!original)
        return;
    if (HookIsShuttingDown()) {
        original(pThis, InName, InValue);
        return;
    }
    if (IsSafeString(InName)) {
        const auto& cfg = GetActiveGraphicsConfig();

        if (cfg.dlssExposureNormalization == "on") {
            if (strcmp(InName, NVSDK_NGX_Parameter_ExposureScale) == 0 ||
                strcmp(InName, NVSDK_NGX_Parameter_PreExposure) == 0) {
                if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                    LogOncePerParam(InName, "NVNGX: Normalizing exposure %s = 1.0", InName);
                InValue = 1.0f;
            }
        } else if (cfg.dlssExposureNormalization == "off") {
            // Explicitly off - do nothing, let game control it
        }

        if (strcmp(InName, NVSDK_NGX_Parameter_Sharpness) == 0 ||
            strcmp(InName, NVSDK_NGX_Parameter_Sharpness_Alt) == 0) {
            if (cfg.parsed.dlssSharpening > -1.5f) {
                float overrideVal = cfg.parsed.dlssSharpening;
                if (overrideVal < -0.5f)
                    overrideVal = 0.0f;  // "off" = 0.0

                if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                    LogOncePerParam(InName, "NVNGX: Overriding sharpness = %.2f (was %.2f)", overrideVal, InValue);
                InValue = overrideVal;
            }
        }
    }
    original(pThis, InName, InValue);
}

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetI(NVSDK_NGX_Parameter* pThis, const char* InName, int* OutValue) {
    const PFN_GetI original = GetParameterOriginals(pThis).getI;
    if (!original)
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    if (HookIsShuttingDown())
        return original(pThis, InName, OutValue);
    NVSDK_NGX_Result res = original(pThis, InName, OutValue);
    if (IsSafeString(InName))
        LogObservedRayReconstructionCapability(InName, res, OutValue, "GetI");
    if (res == NVSDK_NGX_Result_Success && OutValue && IsSafeString(InName)) {
        // If the game/driver is reading back a preset or quality value, we capture
        // it
        if (strcmp(InName, NVSDK_NGX_Parameter_PerfQualityValue) == 0) {
            std::lock_guard<std::mutex> lock(nvngx_hook_g_ParamMapMutex);
            nvngx_hook_g_ParameterQualityMap[pThis] = *OutValue;
        } else if (strstr(InName, "Preset")) {
            UpdatePresetHint(InName, (uint32_t)*OutValue, "GetI");
        }
    }
    return res;
}

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetUI(NVSDK_NGX_Parameter* pThis, const char* InName,
                                                unsigned int* OutValue) {
    const PFN_GetUI original = GetParameterOriginals(pThis).getUI;
    if (!original)
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    if (HookIsShuttingDown())
        return original(pThis, InName, OutValue);
    NVSDK_NGX_Result res = original(pThis, InName, OutValue);
    if (IsSafeString(InName))
        LogObservedRayReconstructionCapability(InName, res, OutValue, "GetUI");
    if (res == NVSDK_NGX_Result_Success && OutValue && IsSafeString(InName)) {
        if (strcmp(InName, NVSDK_NGX_Parameter_PerfQualityValue) == 0) {
            std::lock_guard<std::mutex> lock(nvngx_hook_g_ParamMapMutex);
            nvngx_hook_g_ParameterQualityMap[pThis] = (int)*OutValue;
        } else if (strstr(InName, "Preset")) {
            UpdatePresetHint(InName, *OutValue, "GetUI");
        }
    }
    return res;
}

void EnsureVTableHooks(NVSDK_NGX_Parameter* pParams) {
    if (HookIsShuttingDown())
        return;
    if (!IsSafePtr(pParams)) {
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
            NVNGXLog("EnsureVTableHooks: Skipped (Unsafe Ptr %p)", pParams);
        return;
    }
    void** vtable = *(void***)pParams;
    if (!vtable)
        return;

    // Log vtable address to ensure it's sane
    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
        NVNGXLog("EnsureVTableHooks: pParams=%p, vtable=%p", pParams, vtable);

    {
        std::lock_guard<std::mutex> lock(nvngx_hook_g_NVHookMutex);
        auto [routeIt, inserted] = nvngx_hook_g_ParameterVTableOriginals.try_emplace(vtable);
        (void)inserted;
        ParameterVTableOriginals& originals = routeIt->second;
        const bool complete = originals.setI && originals.setUI && originals.setF && originals.getI && originals.getUI;

        if (!complete) {
            if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                NVNGXLog("NVNGX: Installing typed hooks on VTable at %p", vtable);

            auto Install = [&](int idx, LPVOID hook, auto& original) {
                if (original)
                    return true;
                if (!vtable[idx] || vtable[idx] == hook)
                    return false;
                LPVOID captured = nullptr;
                if (VTableHook::Create(reinterpret_cast<void*>(&vtable[idx]), hook, &captured) != VTableHook::Success || !captured)
                    return false;
                original = reinterpret_cast<std::decay_t<decltype(original)>>(captured);
                return true;
            };

            const bool installed = Install(ce::nvngx_parameter_abi::kSetI, (LPVOID)&Hooked_SetI, originals.setI) &&
                                   Install(ce::nvngx_parameter_abi::kSetUI, (LPVOID)&Hooked_SetUI, originals.setUI) &&
                                   Install(ce::nvngx_parameter_abi::kSetF, (LPVOID)&Hooked_SetF, originals.setF) &&
                                   Install(ce::nvngx_parameter_abi::kGetI, (LPVOID)&Hooked_GetI, originals.getI) &&
                                   Install(ce::nvngx_parameter_abi::kGetUI, (LPVOID)&Hooked_GetUI, originals.getUI);
            if (!installed) {
                static std::atomic<uint32_t> failureLogs{0};
                if (failureLogs.fetch_add(1, std::memory_order_relaxed) < 8) {
                    HookLogImportant(
                        "NVNGX: parameter vtable hook incomplete vtable=%p setI=%d setUI=%d setF=%d "
                        "getI=%d getUI=%d",
                        vtable, originals.setI ? 1 : 0, originals.setUI ? 1 : 0, originals.setF ? 1 : 0,
                        originals.getI ? 1 : 0, originals.getUI ? 1 : 0);
                }
            }
        }
    }

    // Initial Injection via SetI (VT[3])
    const auto& cfg = GetActiveGraphicsConfig();

    std::string mode = cfg.dlssAutoExposure;
    if (mode == "on" || mode == "off") {
        int val = (mode == "on") ? 1 : 0;
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
            LogOncePerParam(NVSDK_NGX_Parameter_AutoExposure, "NVNGX: Injecting initial %s = %d",
                            NVSDK_NGX_Parameter_AutoExposure, val);
        if (vtable[3])
            ((PFN_SetI)vtable[3])(pParams, NVSDK_NGX_Parameter_AutoExposure, val);
        if (vtable[4])
            ((PFN_SetUI)vtable[4])(pParams, NVSDK_NGX_Parameter_AutoExposure, (unsigned int)val);
    }

    const int fgMultiplier = GetConfiguredFGMultiplier(cfg);

    if (fgMultiplier > 0) {
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
            LogOncePerParam(NVSDK_NGX_Parameter_FrameGenerationMultiplier,
                            "NVNGX: Injecting initial FrameGenerationMultiplier = %d", fgMultiplier);
        }
        if (vtable[3])
            ((PFN_SetI)vtable[3])(pParams, NVSDK_NGX_Parameter_FrameGenerationMultiplier, fgMultiplier);
        if (vtable[4])
            ((PFN_SetUI)vtable[4])(pParams, NVSDK_NGX_Parameter_FrameGenerationMultiplier, (unsigned int)fgMultiplier);
    }

    // Initial Injection for Presets (via SetUI VT[4] and SetI VT[3])
    auto InjectPreset = [&](const char* name, uint32_t val) {
        if (val > 0) {
            if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                LogOncePerParam(name, "NVNGX: Injecting initial %s = %u", name, val);

            // Update our local state so the overlay knows about this forced preset!
            UpdatePresetHint(name, val, "InitialInjection");

            // Try via SetUI (Standard)
            if (vtable[4])
                ((PFN_SetUI)vtable[4])(pParams, name, val);

            // Try via SetI (Legacy/Compat)
            if (vtable[3])
                ((PFN_SetI)vtable[3])(pParams, name, (int)val);
        }
    };

    InjectPreset(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA,
                 cfg.parsed.presetDLAA ? cfg.parsed.presetDLAA : cfg.parsed.srPreset);
    InjectPreset(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality,
                 cfg.parsed.presetQuality ? cfg.parsed.presetQuality : cfg.parsed.srPreset);
    InjectPreset(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced,
                 cfg.parsed.presetBalanced ? cfg.parsed.presetBalanced : cfg.parsed.srPreset);
    InjectPreset(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance,
                 cfg.parsed.presetPerformance ? cfg.parsed.presetPerformance : cfg.parsed.srPreset);
    InjectPreset(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance,
                 cfg.parsed.presetUltraPerformance ? cfg.parsed.presetUltraPerformance : cfg.parsed.srPreset);
    InjectPreset(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality,
                 cfg.parsed.presetUltraQuality ? cfg.parsed.presetUltraQuality : cfg.parsed.srPreset);

    InjectPreset(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA,
                 cfg.parsed.rrPresetDLAA ? cfg.parsed.rrPresetDLAA : cfg.parsed.rrPreset);
    InjectPreset(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality,
                 cfg.parsed.rrPresetQuality ? cfg.parsed.rrPresetQuality : cfg.parsed.rrPreset);
    InjectPreset(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced,
                 cfg.parsed.rrPresetBalanced ? cfg.parsed.rrPresetBalanced : cfg.parsed.rrPreset);
    InjectPreset(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance,
                 cfg.parsed.rrPresetPerformance ? cfg.parsed.rrPresetPerformance : cfg.parsed.rrPreset);
    InjectPreset(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance,
                 cfg.parsed.rrPresetUltraPerformance ? cfg.parsed.rrPresetUltraPerformance : cfg.parsed.rrPreset);
    InjectPreset(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality,
                 cfg.parsed.rrPresetUltraQuality ? cfg.parsed.rrPresetUltraQuality : cfg.parsed.rrPreset);

    // Initial Sharpening Injection (VT[6])
    if (cfg.parsed.dlssSharpening > -1.5f && vtable[6]) {
        float overrideVal = cfg.parsed.dlssSharpening;
        if (overrideVal < -0.5f)
            overrideVal = 0.0f;  // "off" = 0.0
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
            LogOncePerParam(NVSDK_NGX_Parameter_Sharpness, "NVNGX: Injecting initial sharpness = %.2f", overrideVal);
        }

        // Try both names for compatibility
        ((PFN_SetF)vtable[6])(pParams, NVSDK_NGX_Parameter_Sharpness, overrideVal);
        ((PFN_SetF)vtable[6])(pParams, NVSDK_NGX_Parameter_Sharpness_Alt, overrideVal);
    }

}

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_ProcessParameters(PFN_NVSDK_NGX_GetParameters original,
                                                            NVSDK_NGX_Parameter** OutParameters, const char* source) {
    if (!original)
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    NVSDK_NGX_Result res = original(OutParameters);
    if (HookIsShuttingDown())
        return res;
    if (res == NVSDK_NGX_Result_Success && OutParameters && *OutParameters) {
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
            NVNGXLog("Hooked_ProcessParameters: Success, Params=%p, *Params=%p", OutParameters, *OutParameters);
        EnsureVTableHooks(*OutParameters);
    } else {
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
            NVNGXLog("Hooked_ProcessParameters: Result=%X, Params=%p", res, OutParameters);
    }
    return res;
}

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetParams_D3D11(NVSDK_NGX_Parameter** p) {
    return Hooked_ProcessParameters(nvngx_hook_oGetParameters_D3D11, p, "GetParams_D3D11");
}

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_AllocParams_D3D11(NVSDK_NGX_Parameter** p) {
    return Hooked_ProcessParameters(nvngx_hook_oAllocateParameters_D3D11, p, "AllocParams_D3D11");
}

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetCaps_D3D11(NVSDK_NGX_Parameter** p) {
    return Hooked_ProcessParameters(nvngx_hook_oGetCapabilityParameters_D3D11, p, "GetCaps_D3D11");
}

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetParams_D3D12(NVSDK_NGX_Parameter** p) {
    return Hooked_ProcessParameters(nvngx_hook_oGetParameters_D3D12, p, "GetParams_D3D12");
}

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_AllocParams_D3D12(NVSDK_NGX_Parameter** p) {
    return Hooked_ProcessParameters(nvngx_hook_oAllocateParameters_D3D12, p, "AllocParams_D3D12");
}

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetCaps_D3D12(NVSDK_NGX_Parameter** p) {
    return Hooked_ProcessParameters(nvngx_hook_oGetCapabilityParameters_D3D12, p, "GetCaps_D3D12");
}

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetParams_VULKAN(NVSDK_NGX_Parameter** p) {
    return Hooked_ProcessParameters(nvngx_hook_oGetParameters_VULKAN, p, "GetParams_VULKAN");
}

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_AllocParams_VULKAN(NVSDK_NGX_Parameter** p) {
    return Hooked_ProcessParameters(nvngx_hook_oAllocateParameters_VULKAN, p, "AllocParams_VULKAN");
}

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetCaps_VULKAN(NVSDK_NGX_Parameter** p) {
    return Hooked_ProcessParameters(nvngx_hook_oGetCapabilityParameters_VULKAN, p, "GetCaps_VULKAN");
}

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_ProcessFeatureRequirements(
    PFN_NVSDK_NGX_GetFeatureRequirements original, void* InAdapter,
    const NVSDK_NGX_FeatureDiscoveryInfo* InDiscoveryInfo, NVSDK_NGX_FeatureRequirement* OutSupported) {
    thread_local bool s_insideFeatureRequirements = false;
    ce::ngx::ScopedReentryGate reentryGate(s_insideFeatureRequirements);
    if (!reentryGate.Entered()) {
        static std::atomic<uint32_t> s_reentryCount{0};
        const uint32_t occurrence = s_reentryCount.fetch_add(1, std::memory_order_relaxed) + 1;
        if (occurrence <= 8 || (occurrence % 100) == 0) {
            HookLogImportant(
                "NVNGX: Blocked recursive GetFeatureRequirements interceptor chain "
                "(original=%p discovery=%p occurrence=%u); returning FeatureNotSupported instead of exhausting "
                "the game thread stack",
                reinterpret_cast<void*>(original), reinterpret_cast<const void*>(InDiscoveryInfo), occurrence);
        }
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    }
    const NVSDK_NGX_Result res =
        original ? original(InAdapter, InDiscoveryInfo, OutSupported) : (NVSDK_NGX_Result)0xBAD00000;
    if (HookIsShuttingDown())
        return res;
    if (InDiscoveryInfo && InDiscoveryInfo->FeatureID == nvngx_hook_NVSDK_NGX_Feature_RayReconstruction &&
        GetActiveGraphicsConfig().forceRayReconstruction) {
        if (OutSupported) {
            LogOncePerParam(
                "GetFeatureRequirements_13",
                "NVNGX RR: observed real Feature 13 requirements result=0x%08X support=%d minArch=%u "
                "(not spoofed)",
                static_cast<unsigned int>(res), OutSupported->FeatureSupported, OutSupported->MinHWArchitecture);
        } else {
            LogOncePerParam("GetFeatureRequirements_13",
                            "NVNGX RR: Feature 13 requirements returned no output (result=0x%08X; not spoofed)",
                            static_cast<unsigned int>(res));
        }
    }
    return res;
}

NVSDK_NGX_Result STDMETHODCALLTYPE
Hooked_GetFeatureRequirements_D3D11(void* InAdapter, const NVSDK_NGX_FeatureDiscoveryInfo* InDiscoveryInfo,
                                    NVSDK_NGX_FeatureRequirement* OutSupported) {
    return Hooked_ProcessFeatureRequirements(nvngx_hook_oGetFeatureRequirements_D3D11, InAdapter, InDiscoveryInfo, OutSupported);
}

NVSDK_NGX_Result STDMETHODCALLTYPE
Hooked_GetFeatureRequirements_D3D12(void* InAdapter, const NVSDK_NGX_FeatureDiscoveryInfo* InDiscoveryInfo,
                                    NVSDK_NGX_FeatureRequirement* OutSupported) {
    return Hooked_ProcessFeatureRequirements(nvngx_hook_oGetFeatureRequirements_D3D12, InAdapter, InDiscoveryInfo, OutSupported);
}

NVSDK_NGX_Result STDMETHODCALLTYPE
Hooked_GetFeatureRequirements_VULKAN(void* InAdapter, const NVSDK_NGX_FeatureDiscoveryInfo* InDiscoveryInfo,
                                     NVSDK_NGX_FeatureRequirement* OutSupported) {
    return Hooked_ProcessFeatureRequirements(nvngx_hook_oGetFeatureRequirements_VULKAN, InAdapter, InDiscoveryInfo, OutSupported);
}
