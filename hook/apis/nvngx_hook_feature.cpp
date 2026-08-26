#include "nvngx_hook_internal.h"
#include "../common/module_export_resolver.h"

static PFN_NVSDK_NGX_CreateFeature oCreateFeature_VULKAN = nullptr;
static PFN_NVSDK_NGX_CreateFeatureVulkan1 oCreateFeature_VULKAN1 = nullptr;

// Read the game's frame-generation multiplier from the CreateFeature
// parameter object. Modern FG v2+ runtimes (DLSS 310.x) carry the generated
// frame count in NVSDK_NGX_DLSSG_Parameter_MultiFrameCount (1=2x, 2=3x, 3=4x);
// older SDKs use NVSDK_NGX_Parameter_FrameGenerationMultiplier (2=2x, 3=3x,
// 4=4x). Talos Reawakened sets MultiFrameCount=3 for 4x MFG (verified in its
// shipped nvngx_dlssg.dll, which contains the "MultiFrameCount" string but no
// "FrameGenerationMultiplier"). Both getI and getUI are tried because the
// runtime may store the value in either slot. Returns 0 when absent or
// unreadable so the caller falls back to the default/config multiplier.
static int ReadNVNGXFGMultiplierParam(NVSDK_NGX_Parameter* params,
                                      const ParameterVTableOriginals& parameterOriginals) {
    if (!params) {
        return 0;
    }
    auto readInt = [&](const char* name) -> int {
        int value = 0;
        if (parameterOriginals.getI &&
            parameterOriginals.getI(params, name, &value) == NVSDK_NGX_Result_Success) {
            return value;
        }
        unsigned int uvalue = 0;
        if (parameterOriginals.getUI &&
            parameterOriginals.getUI(params, name, &uvalue) == NVSDK_NGX_Result_Success) {
            return static_cast<int>(uvalue);
        }
        return 0;
    };
    const int generatedFrames = readInt(NVSDK_NGX_DLSSG_Parameter_MultiFrameCount);
    const int legacyMultiplier = readInt(NVSDK_NGX_Parameter_FrameGenerationMultiplier);
    const int resolved = ce::ngx_lifecycle::ResolveNVNGXObservedFrameGenerationMultiplier(
        generatedFrames, legacyMultiplier);
    if (resolved > 0 && g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
        LogOncePerParam("FrameGenerationMultiplierResolution",
                        "NVNGX: observed FG factor modernGeneratedFrames=%d legacyMultiplier=%d -> %dx",
                        generatedFrames, legacyMultiplier, resolved);
    }
    return resolved;
}

// Helper to get DLSS Version from loaded DLL
static void UpdateDLSSVersion() {
    if (!g_IPC || !g_IPC->GetSharedMem())
        return;

    // UpdateDLSSVersion: Only look for the CORE DLSS implementation
    // User requested to ignore "Streamline version number", so we focus on
    // nvngx_dlss
    HMODULE hNGX = GetModuleHandleA("nvngx_dlss.dll");

    // If not loaded yet, check dlssg
    if (!hNGX)
        hNGX = GetModuleHandleA("nvngx_dlssg.dll");

    // DO NOT accept sl.dlss.dll as it reports wrapper versions

    // Fallback?
    if (!hNGX)
        hNGX = GetModuleHandleA("_nvngx.dll");

    // Safety check
    if (!hNGX)
        return;

    WCHAR fileName[MAX_PATH];
    GetModuleFileNameW(hNGX, fileName, MAX_PATH);

    DWORD handle;
    DWORD size = GetFileVersionInfoSizeW(fileName, &handle);
    if (size > 0) {
        std::vector<BYTE> data(size);
        if (GetFileVersionInfoW(fileName, handle, size, data.data())) {
            VS_FIXEDFILEINFO* fileInfo = nullptr;
            UINT len = 0;
            if (VerQueryValueW(data.data(), L"\\", (void**)&fileInfo, &len)) {
                if (g_IPC && g_IPC->GetSharedMem()) {
                    auto& state = g_IPC->GetSharedMem()->dlssState;
                    int major = HIWORD(fileInfo->dwFileVersionMS);
                    int minor = LOWORD(fileInfo->dwFileVersionMS);
                    int patch = HIWORD(fileInfo->dwFileVersionLS);
                    // int build = LOWORD(fileInfo->dwFileVersionLS);

                    state.versionMajor = major;
                    state.versionMinor = minor;
                    state.versionPatch = patch;

                    if (g_IPC->GetSharedMem()->GetDebugLogging()) {
                        NVNGXLog("NVNGX: Detected Version from %S: v%d.%d.%d", fileName, major, minor, patch);
                    }
                }
            }
        }
    }
}

static char GetPresetChar(int qualityValue) {
    // qualityValue maps to:
    // 0: Perf, 1: Bal, 2: Qual, 3: UP, 4: UQ, 5: DLAA
    if (qualityValue >= 0 && qualityValue <= 5) {
        char hint = nvngx_hook_g_UserPresetHints[qualityValue].load();
        if (hint != '?')
            return hint;
    }
    // If no hint set, check our configuration for what we INTENDED to set
    // This handles cases where SetI hook was missed or game bypassed it, but we
    // want to show the target. Also, if we are overriding, this is likely what is
    // active.

    char configChar = '?';
    const auto& cfg = GetActiveGraphicsConfig();

    // Map qualityValue to specific config
    uint32_t presetVal = 0;
    switch (qualityValue) {
        case 0:
            presetVal = cfg.parsed.presetPerformance;
            break;
        case 1:
            presetVal = cfg.parsed.presetBalanced;
            break;
        case 2:
            presetVal = cfg.parsed.presetQuality;
            break;
        case 3:
            presetVal = cfg.parsed.presetUltraPerformance;
            break;
        case 4:
            presetVal = cfg.parsed.presetUltraQuality;
            break;
        case 5:
            presetVal = cfg.parsed.presetDLAA;
            break;
        default:
            break;
    }

    // Fallback to global srPreset if specific is 0
    if (presetVal == 0)
        presetVal = cfg.parsed.srPreset;

    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
        static int logCount = 0;
        if (logCount < 5) {
            NVNGXLog("NVNGX: GetPresetChar quality=%d -> presetVal=%u (GlobalSR=%u)", qualityValue, presetVal,
                     cfg.parsed.srPreset);
            logCount++;
        }
    }

    if (presetVal > 0) {
        return PresetIDToChar(presetVal);
    }

    return '?';
}

template <typename CreateCall>
NVSDK_NGX_Result ProcessCreateFeature(CreateCall create, void* ctx, int featureID, NVSDK_NGX_Parameter* params,
                                      void** handle) {
    if (HookIsShuttingDown())
        return create();

    static std::atomic<bool> s_firstCall{true};
    if (s_firstCall.exchange(false)) {
        HookLogImportant("NVNGX: First CreateFeature call — NVNGX hooks are active (featureID=%d)", featureID);
    }

    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
        NVNGXLog("Hooked_CreateFeature_Process: Entry ctx=%p, ID=%d, params=%p", ctx, featureID, params);

    if (params)
        EnsureVTableHooks(params);
    const NVSDK_NGX_Result res = create();
    TrackNgxFeatureCreation(featureID, res, handle);

    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
        NVNGXLog("Hooked_CreateFeature_Process: Original Result=%X", res);

    // On success, update status
    if (ce::ngx_lifecycle::IsSuccessfulResult(static_cast<uint32_t>(res))) {
        // Update version if not yet found (handling lazy load)
        if (g_IPC && g_IPC->GetSharedMem()) {
            if (g_IPC->GetSharedMem()->dlssState.versionMajor == 0) {
                UpdateDLSSVersion();
            }
        }

        if (g_IPC && g_IPC->GetSharedMem()) {
            auto& state = g_IPC->GetSharedMem()->dlssState;
            const ParameterVTableOriginals parameterOriginals = GetParameterOriginals(params);

            if (featureID == 1) {
                if (g_IPC->GetSharedMem()->GetDebugLogging())
                    NVNGXLog("Hooked_CreateFeature: DLSS SR created (ID 1); awaiting EvaluateFeature evidence");

                // Proactive Sync: Query effective presets from the parameter object
                auto SyncPreset = [&](const char* name) {
                    unsigned int val = 0;
                    NVSDK_NGX_Result getRes;
                    if (parameterOriginals.getUI) {
                        getRes = parameterOriginals.getUI(params, name, &val);
                        if (getRes == NVSDK_NGX_Result_Success) {
                            UpdatePresetHint(name, val, "SyncUI");
                            return;
                        } else if (g_IPC->GetSharedMem()->GetDebugLogging()) {
                            NVNGXLog("NVNGX: oGetUI(%s) returned %X", name, getRes);
                        }
                    }
                    if (parameterOriginals.getI) {
                        getRes = parameterOriginals.getI(params, name, (int*)&val);
                        if (getRes == NVSDK_NGX_Result_Success) {
                            UpdatePresetHint(name, val, "SyncI");
                        } else if (g_IPC->GetSharedMem()->GetDebugLogging()) {
                            NVNGXLog("NVNGX: oGetI(%s) returned %X", name, getRes);
                        }
                    }
                };
                SyncPreset(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA);
                SyncPreset(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality);
                SyncPreset(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced);
                SyncPreset(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance);
                SyncPreset(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance);
                SyncPreset(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality);

                // Probe for undocumented internal parameters that might expose
                // driver-forced preset
                static bool probeOnce = false;
                if (!probeOnce && parameterOriginals.getUI && g_IPC && g_IPC->GetSharedMem() &&
                    g_IPC->GetSharedMem()->GetDebugLogging()) {
                    probeOnce = true;
                    const char* probeParams[] = {"DLSS.Preset.Active",
                                                 "DLSS.Preset.Effective",
                                                 "DLSS.Feature.Preset",
                                                 "DLSS.Internal.Preset",
                                                 "DLSS.Preset",
                                                 "DLSS.RenderPreset",
                                                 "DLSS.ActivePreset",
                                                 "NVSDK_NGX_Parameter_DLSS_Preset",
                                                 "Preset",
                                                 nullptr};
                    for (int i = 0; probeParams[i]; i++) {
                        unsigned int probedVal = 0;
                        NVSDK_NGX_Result probeRes = parameterOriginals.getUI(params, probeParams[i], &probedVal);
                        if (probeRes == NVSDK_NGX_Result_Success && probedVal > 0) {
                            NVNGXLog("NVNGX: PROBE SUCCESS! '%s' = %u ('%c')", probeParams[i], probedVal,
                                     PresetIDToChar(probedVal));
                        }
                    }
                    NVNGXLog("NVNGX: Undocumented parameter probe complete.");
                }

                // Retrieve captured Quality Mode and Dimensions
                {
                    std::lock_guard<std::mutex> lock(nvngx_hook_g_ParamMapMutex);
                    if (nvngx_hook_g_ParameterQualityMap.count(params)) {
                        int q = nvngx_hook_g_ParameterQualityMap[params];
                        state.qualityMode = q;
                        state.srPreset = GetPresetChar(q);
                        if (g_IPC->GetSharedMem()->GetDebugLogging())
                            NVNGXLog("Hooked_CreateFeature: Quality=%d -> srPreset='%c'", q, state.srPreset.load());
                        nvngx_hook_g_ParameterQualityMap.erase(params);
                    }
                    if (nvngx_hook_g_ParameterDimsMap.count(params)) {
                        const auto& d = nvngx_hook_g_ParameterDimsMap[params];
                        float w1 = (float)d.width;
                        float w2 = (float)d.outWidth;
                        if (w1 > 0 && w2 > 0) {
                            if (w1 >= w2)
                                state.renderScale = w1 / w2;
                            else
                                state.renderScale = w2 / w1;
                        }
                        nvngx_hook_g_ParameterDimsMap.erase(params);
                    }
                }
            } else if (featureID == nvngx_hook_NVSDK_NGX_Feature_RayReconstruction ||
                       featureID == nvngx_hook_NVSDK_NGX_Feature_FrameGeneration_11 ||
                       featureID == nvngx_hook_NVSDK_NGX_Feature_FrameGeneration ||
                       featureID == nvngx_hook_NVSDK_NGX_Feature_MultiFrameGeneration) {
                if (featureID == nvngx_hook_NVSDK_NGX_Feature_RayReconstruction) {
                    if (g_IPC->GetSharedMem()->GetDebugLogging())
                        NVNGXLog("Hooked_CreateFeature: DLSS RR created (ID 13); awaiting EvaluateFeature evidence");
                } else if (featureID == nvngx_hook_NVSDK_NGX_Feature_MultiFrameGeneration) {
                    // DLSS Multi-Frame Generation (MFG) - Feature ID 18
                    // MFG generates 2x, 3x, or 4x frames per rendered frame
                    state.fgActive = true;
                    const int mfgMultiplier = ce::ngx_lifecycle::ResolveNVNGXFrameGenerationMultiplier(
                        GetConfiguredFGMultiplier(GetActiveGraphicsConfig()),
                        ReadNVNGXFGMultiplierParam(params, parameterOriginals));
                    state.mfgMultiplier = mfgMultiplier;

                    if (g_IPC->GetSharedMem()->GetDebugLogging())
                        NVNGXLog(
                            "Hooked_CreateFeature: DLSS Multi-Frame Generation "
                            "ACTIVATED (ID 18, %dx multiplier)",
                            mfgMultiplier);

                    // Signal FG activation (MFG is a form of frame generation)
                    g_FGCompat.SetDLSSFGMultiplier(mfgMultiplier);
                    g_FGCompat.SetDLSSFGActive(true);
                } else {
                    // DLSS Frame Generation - Feature IDs 9 and 0xB (11).
                    // Current DLSS runtimes also carry 2x/3x/4x MFG through the
                    // FrameGenerationMultiplier parameter on these legacy IDs;
                    // do not hardcode 2x (session 20260811_222500: Talos
                    // configured for 4x MFG but late inject showed DLSS 2x).
                    state.fgActive = true;
                    const int resolvedMultiplier = ce::ngx_lifecycle::ResolveNVNGXFrameGenerationMultiplier(
                        GetConfiguredFGMultiplier(GetActiveGraphicsConfig()),
                        ReadNVNGXFGMultiplierParam(params, parameterOriginals));
                    // slDLSSGSetOptions (hooked at startup or via the late-inject
                    // feature-function resolution) is the authoritative multiplier
                    // source for FG v2+ games; the 2x CreateFeature default must
                    // not clobber a latched Streamline multiplier (session
                    // 20260811_231851: transient 4->2 flap before SetOptions
                    // re-asserted 4).
                    const int fgMultiplier = (resolvedMultiplier >= 3 || g_FGCompat.GetFGMultiplier() < 2)
                                                 ? resolvedMultiplier
                                                 : g_FGCompat.GetFGMultiplier();
                    state.mfgMultiplier = fgMultiplier;
                    if (g_IPC->GetSharedMem()->GetDebugLogging())
                        NVNGXLog("Hooked_CreateFeature: DLSS FG ACTIVATED (ID 0x%X, %dx multiplier)", featureID,
                                 fgMultiplier);

                    // CRITICAL: Signal FG activation to the detection system
                    // This enables usage-based detection instead of DLL-based detection
                    g_FGCompat.SetDLSSFGMultiplier(fgMultiplier);
                    g_FGCompat.SetDLSSFGActive(true);
                }
            } else {
                if (g_IPC->GetSharedMem()->GetDebugLogging())
                    NVNGXLog("Hooked_CreateFeature: Other ID 0x%X Activated", featureID);
            }

            // Force Config Overrides (Final step)
            const auto& cfg = GetActiveGraphicsConfig();
            if (g_IPC->GetSharedMem()->GetDebugLogging()) {
                NVNGXLog(
                    "NVNGX: Active overrides — srPreset=%u dlssAutoExposure='%s' dlssSharpening=%.2f "
                    "dlssExposureNormalization='%s'",
                    cfg.parsed.srPreset, cfg.dlssAutoExposure.c_str(), cfg.parsed.dlssSharpening,
                    cfg.dlssExposureNormalization.c_str());
            }
            if (cfg.parsed.srPreset > 0) {
                char oldP = state.srPreset.load();
                state.srPreset = PresetIDToChar(cfg.parsed.srPreset);
                if (g_IPC->GetSharedMem()->GetDebugLogging()) {
                    NVNGXLog(
                        "NV_NGX: Final Force SR Preset: '%c' -> '%c' (Config ID %u, "
                        "From String: %s)",
                        oldP, state.srPreset.load(), cfg.parsed.srPreset, cfg.dlssSRPreset.c_str());
                }
            }
            if (cfg.parsed.rrPreset > 0 && featureID == 13) {
                state.rrPreset = PresetIDToChar(cfg.parsed.rrPreset);
                if (g_IPC->GetSharedMem()->GetDebugLogging())
                    NVNGXLog("NV_NGX: Final Force RR Preset to '%c'", state.rrPreset.load());
            }
        }
    }
    return res;
}

NVSDK_NGX_Result __cdecl Hooked_CreateFeature_D3D11(void* ctx, int featureID, NVSDK_NGX_Parameter* params,
                                                    void** handle) {
    const auto original = nvngx_hook_oCreateFeature_D3D11;
    if (!original)
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    return ProcessCreateFeature([=] { return original(ctx, featureID, params, handle); }, ctx, featureID, params,
                                handle);
}

NVSDK_NGX_Result __cdecl Hooked_CreateFeature_D3D12(void* ctx, int featureID, NVSDK_NGX_Parameter* params,
                                                    void** handle) {
    const auto original = nvngx_hook_oCreateFeature_D3D12;
    if (!original)
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    return ProcessCreateFeature([=] { return original(ctx, featureID, params, handle); }, ctx, featureID, params,
                                handle);
}

NVSDK_NGX_Result __cdecl Hooked_CreateFeature_VULKAN(void* ctx, int featureID, NVSDK_NGX_Parameter* params,
                                                     void** handle) {
    const auto original = oCreateFeature_VULKAN;
    if (!original)
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    return ProcessCreateFeature([=] { return original(ctx, featureID, params, handle); }, ctx, featureID, params,
                                handle);
}

NVSDK_NGX_Result __cdecl Hooked_CreateFeature_VULKAN1(void* device, void* ctx, int featureID,
                                                      NVSDK_NGX_Parameter* params, void** handle) {
    const auto original = oCreateFeature_VULKAN1;
    if (!original)
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    return ProcessCreateFeature([=] { return original(device, ctx, featureID, params, handle); }, ctx, featureID,
                                params, handle);
}

// Inline-hook the NVSDK_NGX_* entry points inside the module that really
// exports them.
//
// Streamline is the reason this is required rather than optional: sl.common.dll
// loads nvngx.dll and resolves every entry point with GetProcAddress when the
// game creates its DLSS feature, roughly ten seconds into a session. CE's IAT
// patches and its GetProcAddress interception are both snapshots of the modules
// loaded when they ran, so neither can ever reach sl.common.dll. Patching the
// export bodies makes interception independent of how - and of when - the
// caller obtained the pointer.
static void InstallNGXExportInlineHooks() {
    static std::mutex s_ExportHookLock;
    static HMODULE s_HookedModule = nullptr;

    HMODULE hNGX = GetModuleHandleA("nvngx.dll");
    const char* moduleName = "nvngx.dll";
    if (!hNGX) {
        hNGX = GetModuleHandleA("_nvngx.dll");
        moduleName = "_nvngx.dll";
    }
    if (!hNGX)
        return;

    std::lock_guard<std::mutex> lock(s_ExportHookLock);
    if (s_HookedModule == hNGX)
        return;

    struct ExportHook {
        const char* name;
        void* detour;
        void** original;
    };
    struct TrampolinePublication {
        void** destination;
        void* fallback;
    };
    const auto publishTrampoline = [](void* trampoline, void* context) {
        auto* publication = static_cast<TrampolinePublication*>(context);
        *publication->destination = trampoline ? trampoline : publication->fallback;
    };
    const ExportHook exports[] = {
        {"NVSDK_NGX_D3D11_GetParameters", (void*)&Hooked_GetParams_D3D11, (void**)&nvngx_hook_oGetParameters_D3D11},
        {"NVSDK_NGX_D3D11_AllocateParameters", (void*)&Hooked_AllocParams_D3D11,
         (void**)&nvngx_hook_oAllocateParameters_D3D11},
        {"NVSDK_NGX_D3D11_GetCapabilityParameters", (void*)&Hooked_GetCaps_D3D11,
         (void**)&nvngx_hook_oGetCapabilityParameters_D3D11},
        {"NVSDK_NGX_D3D11_GetFeatureRequirements", (void*)&Hooked_GetFeatureRequirements_D3D11,
         (void**)&nvngx_hook_oGetFeatureRequirements_D3D11},
        {"NVSDK_NGX_D3D11_CreateFeature", (void*)&Hooked_CreateFeature_D3D11,
         (void**)&nvngx_hook_oCreateFeature_D3D11},
        {"NVSDK_NGX_D3D11_EvaluateFeature", (void*)&Hooked_EvaluateFeature_D3D11,
         (void**)&nvngx_hook_oEvaluateFeature_D3D11},
        {"NVSDK_NGX_D3D11_EvaluateFeature_C", (void*)&Hooked_EvaluateFeature_D3D11_C,
         (void**)&nvngx_hook_oEvaluateFeature_D3D11_C},
        {"NVSDK_NGX_D3D11_ReleaseFeature", (void*)&Hooked_ReleaseFeature_D3D11,
         (void**)&nvngx_hook_oReleaseFeature_D3D11},
        {"NVSDK_NGX_D3D12_GetParameters", (void*)&Hooked_GetParams_D3D12, (void**)&nvngx_hook_oGetParameters_D3D12},
        {"NVSDK_NGX_D3D12_AllocateParameters", (void*)&Hooked_AllocParams_D3D12,
         (void**)&nvngx_hook_oAllocateParameters_D3D12},
        {"NVSDK_NGX_D3D12_GetCapabilityParameters", (void*)&Hooked_GetCaps_D3D12,
         (void**)&nvngx_hook_oGetCapabilityParameters_D3D12},
        {"NVSDK_NGX_D3D12_GetFeatureRequirements", (void*)&Hooked_GetFeatureRequirements_D3D12,
         (void**)&nvngx_hook_oGetFeatureRequirements_D3D12},
        {"NVSDK_NGX_D3D12_CreateFeature", (void*)&Hooked_CreateFeature_D3D12,
         (void**)&nvngx_hook_oCreateFeature_D3D12},
        {"NVSDK_NGX_D3D12_EvaluateFeature", (void*)&Hooked_EvaluateFeature_D3D12,
         (void**)&nvngx_hook_oEvaluateFeature_D3D12},
        {"NVSDK_NGX_D3D12_EvaluateFeature_C", (void*)&Hooked_EvaluateFeature_D3D12_C,
         (void**)&nvngx_hook_oEvaluateFeature_D3D12_C},
        {"NVSDK_NGX_D3D12_ReleaseFeature", (void*)&Hooked_ReleaseFeature_D3D12,
         (void**)&nvngx_hook_oReleaseFeature_D3D12},
        {"NVSDK_NGX_VULKAN_GetParameters", (void*)&Hooked_GetParams_VULKAN, (void**)&nvngx_hook_oGetParameters_VULKAN},
        {"NVSDK_NGX_VULKAN_AllocateParameters", (void*)&Hooked_AllocParams_VULKAN,
         (void**)&nvngx_hook_oAllocateParameters_VULKAN},
        {"NVSDK_NGX_VULKAN_GetCapabilityParameters", (void*)&Hooked_GetCaps_VULKAN,
         (void**)&nvngx_hook_oGetCapabilityParameters_VULKAN},
        {"NVSDK_NGX_VULKAN_GetFeatureRequirements", (void*)&Hooked_GetFeatureRequirements_VULKAN,
         (void**)&nvngx_hook_oGetFeatureRequirements_VULKAN},
        {"NVSDK_NGX_VULKAN_CreateFeature", (void*)&Hooked_CreateFeature_VULKAN, (void**)&oCreateFeature_VULKAN},
        {"NVSDK_NGX_VULKAN_CreateFeature1", (void*)&Hooked_CreateFeature_VULKAN1, (void**)&oCreateFeature_VULKAN1},
        {"NVSDK_NGX_VULKAN_EvaluateFeature", (void*)&Hooked_EvaluateFeature_VULKAN,
         (void**)&nvngx_hook_oEvaluateFeature_VULKAN},
        {"NVSDK_NGX_VULKAN_EvaluateFeature_C", (void*)&Hooked_EvaluateFeature_VULKAN_C,
         (void**)&nvngx_hook_oEvaluateFeature_VULKAN_C},
        {"NVSDK_NGX_VULKAN_ReleaseFeature", (void*)&Hooked_ReleaseFeature_VULKAN,
         (void**)&nvngx_hook_oReleaseFeature_VULKAN},
    };

    int hooked = 0;
    int aliased = 0;
    int missing = 0;
    int failed = 0;
    std::vector<std::pair<void*, void*>> hookedTargets;
    for (const ExportHook& entry : exports) {
        // A game-local proxy can intercept GetProcAddress and return one of its
        // own wrappers for an NGX query. Patching that address as though it were
        // the core export corrupts the proxy's saved-original chain. Read the
        // loaded core image's EAT directly; any established inline entry patch
        // remains visible and InlineHook will preserve it normally.
        void* target = ce::module_export::ResolveAddressDirect(hNGX, entry.name);
        if (!target) {
            ++missing;
            continue;
        }
        const auto existing = std::find_if(hookedTargets.begin(), hookedTargets.end(),
                                           [target](const auto& item) { return item.first == target; });
        if (existing != hookedTargets.end()) {
            *entry.original = existing->second;
            ++aliased;
            continue;
        }
        TrampolinePublication publication{entry.original, *entry.original};
        void* trampoline = nullptr;
        if (!InlineHook::InstallPublished(target, entry.detour, &trampoline, publishTrampoline, &publication) ||
            !trampoline) {
            ++failed;
            HookLogImportant("NVNGX: failed to inline-hook %s!%s at %p", moduleName, entry.name, target);
            continue;
        }
        // Publication replaced any raw address captured by the earlier IAT pass
        // before the export body became callable as our detour.
        hookedTargets.push_back({target, trampoline});
        ++hooked;
    }

    if (hooked > 0)
        s_HookedModule = hNGX;

    HookLogImportant("NVNGX: directly resolved and inline-hooked %d export(s) in %s at %p "
                     "(aliases=%d absent=%d failed=%d); preset/parameter overrides now apply without trusting a "
                     "foreign GetProcAddress chain",
                     hooked, moduleName, (void*)hNGX, aliased, missing, failed);
}

void NVNGXHook::OnModuleLoaded(HMODULE module, const char* moduleNameOrPath) {
    if (!module || !ce::ngx::IsNgxCoreModulePath(moduleNameOrPath))
        return;
    // Runs inside the caller's LoadLibrary, before it can resolve an export.
    InstallNGXExportInlineHooks();
}

void NVNGXHook::Install() {
    // Independent of the one-shot latch below: the NGX provider usually appears
    // long after the first successful Install() pass.
    InstallNGXExportInlineHooks();

    if (m_Installed.exchange(true))
        return;

    static std::atomic<bool> s_DynamicHookRegisterLogged{false};
    static std::atomic<bool> s_DeferredLoadLogged{false};

    // Update version once at install time
    UpdateDLSSVersion();

    // Force initialize user's configured preset (handles Streamline bypass)
    const auto& cfg = GetActiveGraphicsConfig();
    uint32_t presetVal = cfg.parsed.srPreset;
    if (presetVal > 0) {
        // We can't access g_IPC yet reliably if it's too early, but we can set
        // static state? Wait, 'state' is in shared mem. g_IPC might not be
        // connected if game just started. But GetSharedMem() checks connection.
        if (g_IPC && g_IPC->GetSharedMem()) {
            g_IPC->GetSharedMem()->dlssState.srPreset = PresetIDToChar(presetVal);
            NVNGXLog("NVNGX: Config forced SR Preset to '%c' (via Install)", PresetIDToChar(presetVal));
        } else {
            // Store it in a static backup if IPC isn't ready?

            // Actually, IPC connects in DX12Hook::Init or similar.
            // NVNGX Install happens early.
            // Let's rely on the fact that GetPresetChar now has a fallback!
            // Wait, GetPresetChar is only called by CreateFeature.
            // If CreateFeature is bypassed, GetPresetChar updates nothing.
            // So I DO need to update shared state here if possible.
            // If IPC fails, we can't update shared state anyway (it doesn't exist).
        }
    }

    // Phase 1: Register dynamic hooks for GetProcAddress interception UNCONDITIONALLY.
    // This must happen before the DLL-presence check so that if the game loads
    // nvngx.dll after this point and immediately calls GetProcAddress, we intercept it.
    // RegisterDynamicHook is idempotent — safe to call on every retry.
    //
    // The lookups are restricted to the NGX core provider. The feature snippets
    // export the same NVSDK_NGX_* names and the core resolves them internally to
    // dispatch into the feature; answering that with our detour makes the detour
    // forward through the core's own trampoline and re-enter the core body until
    // the stack overflows. See ce::ngx::ShouldInterceptNgxExportLookup.
    auto RegisterDynamic = [](const char* name, LPVOID pHook, LPVOID* ppOrig) {
        IATHook::RegisterDynamicHookFiltered(name, pHook, ppOrig,
                                             [](const char* moduleBaseName, HMODULE) {
                                                 return ce::ngx::ShouldInterceptNgxExportLookup(moduleBaseName);
                                             });
    };

    RegisterDynamic("NVSDK_NGX_D3D11_GetParameters", (LPVOID)&Hooked_GetParams_D3D11, (LPVOID*)&nvngx_hook_oGetParameters_D3D11);
    RegisterDynamic("NVSDK_NGX_D3D11_AllocateParameters", (LPVOID)&Hooked_AllocParams_D3D11,
                    (LPVOID*)&nvngx_hook_oAllocateParameters_D3D11);
    RegisterDynamic("NVSDK_NGX_D3D11_GetCapabilityParameters", (LPVOID)&Hooked_GetCaps_D3D11,
                    (LPVOID*)&nvngx_hook_oGetCapabilityParameters_D3D11);
    RegisterDynamic("NVSDK_NGX_D3D12_GetParameters", (LPVOID)&Hooked_GetParams_D3D12, (LPVOID*)&nvngx_hook_oGetParameters_D3D12);
    RegisterDynamic("NVSDK_NGX_D3D12_AllocateParameters", (LPVOID)&Hooked_AllocParams_D3D12,
                    (LPVOID*)&nvngx_hook_oAllocateParameters_D3D12);
    RegisterDynamic("NVSDK_NGX_D3D12_GetCapabilityParameters", (LPVOID)&Hooked_GetCaps_D3D12,
                    (LPVOID*)&nvngx_hook_oGetCapabilityParameters_D3D12);
    RegisterDynamic("NVSDK_NGX_VULKAN_GetParameters", (LPVOID)&Hooked_GetParams_VULKAN,
                    (LPVOID*)&nvngx_hook_oGetParameters_VULKAN);
    RegisterDynamic("NVSDK_NGX_VULKAN_AllocateParameters", (LPVOID)&Hooked_AllocParams_VULKAN,
                    (LPVOID*)&nvngx_hook_oAllocateParameters_VULKAN);
    RegisterDynamic("NVSDK_NGX_VULKAN_GetCapabilityParameters", (LPVOID)&Hooked_GetCaps_VULKAN,
                    (LPVOID*)&nvngx_hook_oGetCapabilityParameters_VULKAN);
    RegisterDynamic("NVSDK_NGX_D3D11_GetFeatureRequirements", (LPVOID)&Hooked_GetFeatureRequirements_D3D11,
                    (LPVOID*)&nvngx_hook_oGetFeatureRequirements_D3D11);
    RegisterDynamic("NVSDK_NGX_D3D12_GetFeatureRequirements", (LPVOID)&Hooked_GetFeatureRequirements_D3D12,
                    (LPVOID*)&nvngx_hook_oGetFeatureRequirements_D3D12);
    RegisterDynamic("NVSDK_NGX_VULKAN_GetFeatureRequirements", (LPVOID)&Hooked_GetFeatureRequirements_VULKAN,
                    (LPVOID*)&nvngx_hook_oGetFeatureRequirements_VULKAN);
    RegisterDynamic("NVSDK_NGX_D3D11_CreateFeature", (LPVOID)&Hooked_CreateFeature_D3D11,
                    (LPVOID*)&nvngx_hook_oCreateFeature_D3D11);
    RegisterDynamic("NVSDK_NGX_D3D11_EvaluateFeature", (LPVOID)&Hooked_EvaluateFeature_D3D11,
                    (LPVOID*)&nvngx_hook_oEvaluateFeature_D3D11);
    RegisterDynamic("NVSDK_NGX_D3D11_EvaluateFeature_C", (LPVOID)&Hooked_EvaluateFeature_D3D11_C,
                    (LPVOID*)&nvngx_hook_oEvaluateFeature_D3D11_C);
    RegisterDynamic("NVSDK_NGX_D3D11_ReleaseFeature", (LPVOID)&Hooked_ReleaseFeature_D3D11,
                    (LPVOID*)&nvngx_hook_oReleaseFeature_D3D11);
    RegisterDynamic("NVSDK_NGX_D3D12_CreateFeature", (LPVOID)&Hooked_CreateFeature_D3D12,
                    (LPVOID*)&nvngx_hook_oCreateFeature_D3D12);
    RegisterDynamic("NVSDK_NGX_D3D12_EvaluateFeature", (LPVOID)&Hooked_EvaluateFeature_D3D12,
                    (LPVOID*)&nvngx_hook_oEvaluateFeature_D3D12);
    RegisterDynamic("NVSDK_NGX_D3D12_EvaluateFeature_C", (LPVOID)&Hooked_EvaluateFeature_D3D12_C,
                    (LPVOID*)&nvngx_hook_oEvaluateFeature_D3D12_C);
    RegisterDynamic("NVSDK_NGX_D3D12_ReleaseFeature", (LPVOID)&Hooked_ReleaseFeature_D3D12,
                    (LPVOID*)&nvngx_hook_oReleaseFeature_D3D12);
    RegisterDynamic("NVSDK_NGX_VULKAN_CreateFeature", (LPVOID)&Hooked_CreateFeature_VULKAN,
                    (LPVOID*)&oCreateFeature_VULKAN);
    RegisterDynamic("NVSDK_NGX_VULKAN_CreateFeature1", (LPVOID)&Hooked_CreateFeature_VULKAN1,
                    (LPVOID*)&oCreateFeature_VULKAN1);
    RegisterDynamic("NVSDK_NGX_VULKAN_EvaluateFeature", (LPVOID)&Hooked_EvaluateFeature_VULKAN,
                    (LPVOID*)&nvngx_hook_oEvaluateFeature_VULKAN);
    RegisterDynamic("NVSDK_NGX_VULKAN_EvaluateFeature_C", (LPVOID)&Hooked_EvaluateFeature_VULKAN_C,
                    (LPVOID*)&nvngx_hook_oEvaluateFeature_VULKAN_C);
    RegisterDynamic("NVSDK_NGX_VULKAN_ReleaseFeature", (LPVOID)&Hooked_ReleaseFeature_VULKAN,
                    (LPVOID*)&nvngx_hook_oReleaseFeature_VULKAN);

    if (!s_DynamicHookRegisterLogged.exchange(true)) {
        HookLogImportant("NVNGX: Dynamic hooks registered for GetProcAddress interception");
    }

    // Phase 2: Patch IAT entries in already-loaded modules (requires DLL to be present).
    // Only the real provider counts. sl.dlss.dll used to satisfy this check, but
    // it exports no NVSDK_NGX_* symbol at all, so accepting it merely latched the
    // hook as "installed" and stopped every later retry.
    const char* foundDllName = "nvngx.dll";
    HMODULE hNGX = GetModuleHandleA("nvngx.dll");
    if (!hNGX) {
        hNGX = GetModuleHandleA("_nvngx.dll");
        if (hNGX)
            foundDllName = "_nvngx.dll";
    }

    if (!hNGX) {
        if (!s_DeferredLoadLogged.exchange(true)) {
            HookLogImportant("NVNGX: DLL not yet loaded; IAT patches deferred (dynamic hooks active, will retry)");
        }
        m_Installed = false;
        return;
    }

    s_DeferredLoadLogged.store(false, std::memory_order_relaxed);
    HookLogImportant("NVNGX: Found '%s' at %p, installing IAT patches", foundDllName, (void*)hNGX);

    auto PatchIAT = [](const char* name, LPVOID pHook, LPVOID* ppOrig) {
        for (const char* module : {"_nvngx.dll", "nvngx.dll", "sl.dlss.dll"}) {
            void* captured = nullptr;
            if (IATHook::PatchIATAllModules(module, name, pHook, &captured) && captured && ppOrig && !*ppOrig)
                *ppOrig = captured;
        }
    };

    PatchIAT("NVSDK_NGX_D3D11_GetParameters", (LPVOID)&Hooked_GetParams_D3D11, (LPVOID*)&nvngx_hook_oGetParameters_D3D11);
    PatchIAT("NVSDK_NGX_D3D11_AllocateParameters", (LPVOID)&Hooked_AllocParams_D3D11,
             (LPVOID*)&nvngx_hook_oAllocateParameters_D3D11);
    PatchIAT("NVSDK_NGX_D3D11_GetCapabilityParameters", (LPVOID)&Hooked_GetCaps_D3D11,
             (LPVOID*)&nvngx_hook_oGetCapabilityParameters_D3D11);
    PatchIAT("NVSDK_NGX_D3D12_GetParameters", (LPVOID)&Hooked_GetParams_D3D12, (LPVOID*)&nvngx_hook_oGetParameters_D3D12);
    PatchIAT("NVSDK_NGX_D3D12_AllocateParameters", (LPVOID)&Hooked_AllocParams_D3D12,
             (LPVOID*)&nvngx_hook_oAllocateParameters_D3D12);
    PatchIAT("NVSDK_NGX_D3D12_GetCapabilityParameters", (LPVOID)&Hooked_GetCaps_D3D12,
             (LPVOID*)&nvngx_hook_oGetCapabilityParameters_D3D12);
    PatchIAT("NVSDK_NGX_VULKAN_GetParameters", (LPVOID)&Hooked_GetParams_VULKAN, (LPVOID*)&nvngx_hook_oGetParameters_VULKAN);
    PatchIAT("NVSDK_NGX_VULKAN_AllocateParameters", (LPVOID)&Hooked_AllocParams_VULKAN,
             (LPVOID*)&nvngx_hook_oAllocateParameters_VULKAN);
    PatchIAT("NVSDK_NGX_VULKAN_GetCapabilityParameters", (LPVOID)&Hooked_GetCaps_VULKAN,
             (LPVOID*)&nvngx_hook_oGetCapabilityParameters_VULKAN);
    PatchIAT("NVSDK_NGX_D3D11_GetFeatureRequirements", (LPVOID)&Hooked_GetFeatureRequirements_D3D11,
             (LPVOID*)&nvngx_hook_oGetFeatureRequirements_D3D11);
    PatchIAT("NVSDK_NGX_D3D12_GetFeatureRequirements", (LPVOID)&Hooked_GetFeatureRequirements_D3D12,
             (LPVOID*)&nvngx_hook_oGetFeatureRequirements_D3D12);
    PatchIAT("NVSDK_NGX_VULKAN_GetFeatureRequirements", (LPVOID)&Hooked_GetFeatureRequirements_VULKAN,
             (LPVOID*)&nvngx_hook_oGetFeatureRequirements_VULKAN);
    PatchIAT("NVSDK_NGX_D3D11_CreateFeature", (LPVOID)&Hooked_CreateFeature_D3D11, (LPVOID*)&nvngx_hook_oCreateFeature_D3D11);
    PatchIAT("NVSDK_NGX_D3D11_EvaluateFeature", (LPVOID)&Hooked_EvaluateFeature_D3D11,
             (LPVOID*)&nvngx_hook_oEvaluateFeature_D3D11);
    PatchIAT("NVSDK_NGX_D3D11_EvaluateFeature_C", (LPVOID)&Hooked_EvaluateFeature_D3D11_C,
             (LPVOID*)&nvngx_hook_oEvaluateFeature_D3D11_C);
    PatchIAT("NVSDK_NGX_D3D11_ReleaseFeature", (LPVOID)&Hooked_ReleaseFeature_D3D11,
             (LPVOID*)&nvngx_hook_oReleaseFeature_D3D11);
    PatchIAT("NVSDK_NGX_D3D12_CreateFeature", (LPVOID)&Hooked_CreateFeature_D3D12, (LPVOID*)&nvngx_hook_oCreateFeature_D3D12);
    PatchIAT("NVSDK_NGX_D3D12_EvaluateFeature", (LPVOID)&Hooked_EvaluateFeature_D3D12,
             (LPVOID*)&nvngx_hook_oEvaluateFeature_D3D12);
    PatchIAT("NVSDK_NGX_D3D12_EvaluateFeature_C", (LPVOID)&Hooked_EvaluateFeature_D3D12_C,
             (LPVOID*)&nvngx_hook_oEvaluateFeature_D3D12_C);
    PatchIAT("NVSDK_NGX_D3D12_ReleaseFeature", (LPVOID)&Hooked_ReleaseFeature_D3D12,
             (LPVOID*)&nvngx_hook_oReleaseFeature_D3D12);
    PatchIAT("NVSDK_NGX_VULKAN_CreateFeature", (LPVOID)&Hooked_CreateFeature_VULKAN, (LPVOID*)&oCreateFeature_VULKAN);
    PatchIAT("NVSDK_NGX_VULKAN_CreateFeature1", (LPVOID)&Hooked_CreateFeature_VULKAN1,
             (LPVOID*)&oCreateFeature_VULKAN1);
    PatchIAT("NVSDK_NGX_VULKAN_EvaluateFeature", (LPVOID)&Hooked_EvaluateFeature_VULKAN,
             (LPVOID*)&nvngx_hook_oEvaluateFeature_VULKAN);
    PatchIAT("NVSDK_NGX_VULKAN_EvaluateFeature_C", (LPVOID)&Hooked_EvaluateFeature_VULKAN_C,
             (LPVOID*)&nvngx_hook_oEvaluateFeature_VULKAN_C);
    PatchIAT("NVSDK_NGX_VULKAN_ReleaseFeature", (LPVOID)&Hooked_ReleaseFeature_VULKAN,
             (LPVOID*)&nvngx_hook_oReleaseFeature_VULKAN);

    HookLogImportant("NVNGX: IAT patches installed");
}

void NVNGXHook::Uninstall() {
    if (!m_Installed.exchange(false))
        return;
    // IAT hooks are permanent until process exit usually
}
