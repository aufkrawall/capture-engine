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

    // Force Ray Reconstruction Availability
    if (cfg.forceRayReconstruction) {
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
            LogOncePerParam("NVNGX_ForceRR_Caps",
                            "NVNGX: Injecting RayReconstruction.Available=1 and "
                            "FeatureInitResult=1");
        }
        // Force available = 1 (true)
        if (vtable[3])
            ((PFN_SetI)vtable[3])(pParams, NVSDK_NGX_Parameter_RayReconstruction_Available, 1);
        if (vtable[4])
            ((PFN_SetUI)vtable[4])(pParams, NVSDK_NGX_Parameter_RayReconstruction_Available, 1);

        // Force init result = 0 (Success/Supported check usually looks for 0 or 1
        // depending on param, but FeatureInitResult is often a success code 0 or 1)
        // Actually FeatureInitResult usually stores the NVSDK_NGX_Result. 1 =
        // Success.
        if (vtable[3])
            ((PFN_SetI)vtable[3])(pParams, NVSDK_NGX_Parameter_RayReconstruction_FeatureInitResult, 1);
        if (vtable[4])
            ((PFN_SetUI)vtable[4])(pParams, NVSDK_NGX_Parameter_RayReconstruction_FeatureInitResult, 1);
    }
}

// --- Factory Hooks ---
typedef NVSDK_NGX_Result(STDMETHODCALLTYPE* PFN_NVSDK_NGX_GetParameters)(NVSDK_NGX_Parameter** OutParameters);
static PFN_NVSDK_NGX_GetParameters oGetParameters_D3D11 = nullptr;
static PFN_NVSDK_NGX_GetParameters oAllocateParameters_D3D11 = nullptr;
static PFN_NVSDK_NGX_GetParameters oGetCapabilityParameters_D3D11 = nullptr;

static PFN_NVSDK_NGX_GetParameters oGetParameters_D3D12 = nullptr;
static PFN_NVSDK_NGX_GetParameters oAllocateParameters_D3D12 = nullptr;
static PFN_NVSDK_NGX_GetParameters oGetCapabilityParameters_D3D12 = nullptr;

static PFN_NVSDK_NGX_GetParameters oGetParameters_VULKAN = nullptr;
static PFN_NVSDK_NGX_GetParameters oAllocateParameters_VULKAN = nullptr;
static PFN_NVSDK_NGX_GetParameters oGetCapabilityParameters_VULKAN = nullptr;

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_ProcessParameters(PFN_NVSDK_NGX_GetParameters original,
                                                            NVSDK_NGX_Parameter** OutParameters, const char* source) {
    if (!original)
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;
    NVSDK_NGX_Result res = original(OutParameters);
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
    return Hooked_ProcessParameters(oGetParameters_D3D11, p, "GetParams_D3D11");
}
NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_AllocParams_D3D11(NVSDK_NGX_Parameter** p) {
    return Hooked_ProcessParameters(oAllocateParameters_D3D11, p, "AllocParams_D3D11");
}
NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetCaps_D3D11(NVSDK_NGX_Parameter** p) {
    return Hooked_ProcessParameters(oGetCapabilityParameters_D3D11, p, "GetCaps_D3D11");
}

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetParams_D3D12(NVSDK_NGX_Parameter** p) {
    return Hooked_ProcessParameters(oGetParameters_D3D12, p, "GetParams_D3D12");
}
NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_AllocParams_D3D12(NVSDK_NGX_Parameter** p) {
    return Hooked_ProcessParameters(oAllocateParameters_D3D12, p, "AllocParams_D3D12");
}
NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetCaps_D3D12(NVSDK_NGX_Parameter** p) {
    return Hooked_ProcessParameters(oGetCapabilityParameters_D3D12, p, "GetCaps_D3D12");
}

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetParams_VULKAN(NVSDK_NGX_Parameter** p) {
    return Hooked_ProcessParameters(oGetParameters_VULKAN, p, "GetParams_VULKAN");
}
NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_AllocParams_VULKAN(NVSDK_NGX_Parameter** p) {
    return Hooked_ProcessParameters(oAllocateParameters_VULKAN, p, "AllocParams_VULKAN");
}
NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetCaps_VULKAN(NVSDK_NGX_Parameter** p) {
    return Hooked_ProcessParameters(oGetCapabilityParameters_VULKAN, p, "GetCaps_VULKAN");
}

// Definition matching nvsdk_ngx_defs.h
typedef struct NVSDK_NGX_Application_Identifier {
    unsigned int IdentifierType;  // Enum in reality but size int
    union v {
        struct {
            const char* ProjectId;
            int EngineType;
            const char* EngineVersion;
        } ProjectDesc;
        unsigned long long ApplicationId;
    } v;
} NVSDK_NGX_Application_Identifier;

typedef struct NVSDK_NGX_FeatureDiscoveryInfo {
    unsigned int SDKVersion;
    int FeatureID;
    NVSDK_NGX_Application_Identifier Identifier;
    const wchar_t* ApplicationDataPath;
    const void* FeatureInfo;
} NVSDK_NGX_FeatureDiscoveryInfo;

typedef struct NVSDK_NGX_FeatureRequirement {
    int FeatureSupported;  // 0 = Supported
    unsigned int MinHWArchitecture;
    char MinOSVersion[255];
} NVSDK_NGX_FeatureRequirement;

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_ProcessFeatureRequirements(
    PFN_NVSDK_NGX_GetFeatureRequirements original, void* InAdapter,
    const NVSDK_NGX_FeatureDiscoveryInfo* InDiscoveryInfo, NVSDK_NGX_FeatureRequirement* OutSupported) {
    NVSDK_NGX_Result res = NVSDK_NGX_Result_Success;

    // Call original first to populate real data
    if (original) {
        res = original(InAdapter, InDiscoveryInfo, OutSupported);
    } else {
        // Fallback if original missing (unlikely if hooked)
        res = (NVSDK_NGX_Result)0xBAD00000;  // Fail
    }

    if (InDiscoveryInfo && OutSupported) {
        // Feature 13 is Ray Reconstruction
        if (InDiscoveryInfo->FeatureID == 13) {
            const auto& cfg = GetActiveGraphicsConfig();
            if (cfg.forceRayReconstruction) {
                if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
                    LogOncePerParam("GetFeatureRequirements_13",
                                    "NVNGX: Spoofing Ray Reconstruction (Feature 13) as SUPPORTED");
                }

                // Force Success Result
                res = NVSDK_NGX_Result_Success;

                // Force Structure
                OutSupported->FeatureSupported = 0;  // NVSDK_NGX_FeatureSupportResult_Supported

                // Fill placeholders if empty
                if (OutSupported->MinHWArchitecture == 0)
                    OutSupported->MinHWArchitecture = 0;  // Supported
                if (OutSupported->MinOSVersion[0] == 0)
                    strcpy_s(OutSupported->MinOSVersion, "10.0.19041");
            }
        }
    }
    return res;
}

NVSDK_NGX_Result STDMETHODCALLTYPE
Hooked_GetFeatureRequirements_D3D11(void* InAdapter, const NVSDK_NGX_FeatureDiscoveryInfo* InDiscoveryInfo,
                                    NVSDK_NGX_FeatureRequirement* OutSupported) {
    return Hooked_ProcessFeatureRequirements(oGetFeatureRequirements_D3D11, InAdapter, InDiscoveryInfo, OutSupported);
}

NVSDK_NGX_Result STDMETHODCALLTYPE
Hooked_GetFeatureRequirements_D3D12(void* InAdapter, const NVSDK_NGX_FeatureDiscoveryInfo* InDiscoveryInfo,
                                    NVSDK_NGX_FeatureRequirement* OutSupported) {
    return Hooked_ProcessFeatureRequirements(oGetFeatureRequirements_D3D12, InAdapter, InDiscoveryInfo, OutSupported);
}

NVSDK_NGX_Result STDMETHODCALLTYPE
Hooked_GetFeatureRequirements_VULKAN(void* InAdapter, const NVSDK_NGX_FeatureDiscoveryInfo* InDiscoveryInfo,
                                     NVSDK_NGX_FeatureRequirement* OutSupported) {
    return Hooked_ProcessFeatureRequirements(oGetFeatureRequirements_VULKAN, InAdapter, InDiscoveryInfo, OutSupported);
}

// --- CreateFeature Hooks ---
// CreateFeature signature is __cdecl (standard C export)
// CreateFeature signature (4 args based on NGX docs/reversing)
typedef NVSDK_NGX_Result(__cdecl* PFN_NVSDK_NGX_CreateFeature)(void* InCmdList, int InFeatureID,
                                                               NVSDK_NGX_Parameter* InParameters, void** OutHandle);
static PFN_NVSDK_NGX_CreateFeature oCreateFeature_D3D11 = nullptr;
static PFN_NVSDK_NGX_CreateFeature oCreateFeature_D3D12 = nullptr;
static PFN_NVSDK_NGX_CreateFeature oCreateFeature_VULKAN = nullptr;

// DLSS Create Params (Approximation based on common RE)
struct NVSDK_NGX_DLSS_Create_Params {
    unsigned int InFeatureCreateFlags;
    unsigned int InEnableOutputSubrects;
    unsigned int InRenderWidth;
    unsigned int InRenderHeight;
    unsigned int InTargetWidth;
    unsigned int InTargetHeight;
    int InPerfQualityValue;
    int InDoSharpening;
};

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
        char hint = g_UserPresetHints[qualityValue].load();
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

static float CalculateScale(const NVSDK_NGX_DLSS_Create_Params* p) {
    if (p->InTargetWidth > 0 && p->InRenderWidth > 0) {
        return (float)p->InTargetWidth / (float)p->InRenderWidth;
    }
    return 0.0f;
}

static void UpdateDLSSStatus(const NVSDK_NGX_FeatureDiscoveryInfo* info) {
    if (!IsSafePtr(info) || !g_IPC || !g_IPC->GetSharedMem())
        return;

    auto& state = g_IPC->GetSharedMem()->dlssState;
    // Version is updated at install time now

    // Safety check for FeatureInfo
    if (!IsSafePtr(info->FeatureInfo))
        return;

    if (info->FeatureID == 1) {  // Super Resolution
        state.srActive = true;
        if (info->FeatureInfo) {
            const auto* params = (const NVSDK_NGX_DLSS_Create_Params*)info->FeatureInfo;
            state.renderScale = CalculateScale(params);
            state.srPreset = GetPresetChar(params->InPerfQualityValue);

            if (g_IPC->GetSharedMem()->GetDebugLogging()) {
                LogOncePerParam("DLSS_SR_Create", "NVNGX: Created DLSS SR. Scale=%.2f, Quality=%d, Preset=%c",
                                state.renderScale.load(), params->InPerfQualityValue, state.srPreset.load());
            }
        }
    } else if (info->FeatureID == 13) {  // Ray Reconstruction
        state.rrActive = true;
        if (g_IPC->GetSharedMem()->GetDebugLogging()) {
            LogOncePerParam("DLSS_RR_Create", "NVNGX: Created DLSS RR.");
        }
    }
}

NVSDK_NGX_Result __cdecl Hooked_CreateFeature_Process(PFN_NVSDK_NGX_CreateFeature original, void* ctx, int featureID,
                                                      NVSDK_NGX_Parameter* params, void** handle) {
    static std::atomic<bool> s_firstCall{true};
    if (s_firstCall.exchange(false)) {
        HookLogImportant("NVNGX: First CreateFeature call — NVNGX hooks are active (featureID=%d)", featureID);
    }

    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
        NVNGXLog("Hooked_CreateFeature_Process: Entry ctx=%p, ID=%d, params=%p", ctx, featureID, params);

    if (!original)
        return NVSDK_NGX_Result_FAIL_FeatureNotSupported;

    if (params)
        EnsureVTableHooks(params);
    const NVSDK_NGX_Result res = original(ctx, featureID, params, handle);

    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
        NVNGXLog("Hooked_CreateFeature_Process: Original Result=%X", res);

    // On success, update status
    if (res == NVSDK_NGX_Result_Success) {
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
                state.srActive = true;
                if (g_IPC->GetSharedMem()->GetDebugLogging())
                    NVNGXLog("Hooked_CreateFeature: DLSS SR Activated (ID 1)");

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
                    std::lock_guard<std::mutex> lock(g_ParamMapMutex);
                    if (g_ParameterQualityMap.count(params)) {
                        int q = g_ParameterQualityMap[params];
                        state.qualityMode = q;
                        state.srPreset = GetPresetChar(q);
                        if (g_IPC->GetSharedMem()->GetDebugLogging())
                            NVNGXLog("Hooked_CreateFeature: Quality=%d -> srPreset='%c'", q, state.srPreset.load());
                        g_ParameterQualityMap.erase(params);
                    }
                    if (g_ParameterDimsMap.count(params)) {
                        const auto& d = g_ParameterDimsMap[params];
                        float w1 = (float)d.width;
                        float w2 = (float)d.outWidth;
                        if (w1 > 0 && w2 > 0) {
                            if (w1 >= w2)
                                state.renderScale = w1 / w2;
                            else
                                state.renderScale = w2 / w1;
                        }
                        g_ParameterDimsMap.erase(params);
                    }
                }
            } else if (featureID == NVSDK_NGX_Feature_RayReconstruction ||
                       featureID == NVSDK_NGX_Feature_FrameGeneration_11 ||
                       featureID == NVSDK_NGX_Feature_FrameGeneration ||
                       featureID == NVSDK_NGX_Feature_MultiFrameGeneration) {
                if (featureID == NVSDK_NGX_Feature_RayReconstruction) {
                    state.rrActive = true;
                    if (g_IPC->GetSharedMem()->GetDebugLogging())
                        NVNGXLog("Hooked_CreateFeature: DLSS RR Activated (ID 13)");
                } else if (featureID == NVSDK_NGX_Feature_MultiFrameGeneration) {
                    // DLSS Multi-Frame Generation (MFG) - Feature ID 18
                    // MFG generates 2x, 3x, or 4x frames per rendered frame
                    state.fgActive = true;

                    // Try to read the multiplier from parameters
                    int mfgMultiplier = 2;  // Default to 2x
                    const auto& cfg = GetActiveGraphicsConfig();
                    const int configuredMultiplier = GetConfiguredFGMultiplier(cfg);
                    if (configuredMultiplier > 0) {
                        mfgMultiplier = configuredMultiplier;
                    }
                    if (params && parameterOriginals.getI) {
                        int multValue = 0;
                        NVSDK_NGX_Result multRes =
                            parameterOriginals.getI(params, NVSDK_NGX_Parameter_FrameGenerationMultiplier, &multValue);
                        if (multRes == NVSDK_NGX_Result_Success && multValue >= 2 && multValue <= 4) {
                            mfgMultiplier = multValue;
                        }
                    }
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
                    // DLSS Frame Generation - Feature IDs 9 and 0xB (11)
                    state.fgActive = true;
                    state.mfgMultiplier = 2;  // Standard FG is a 2x output multiplier
                    if (g_IPC->GetSharedMem()->GetDebugLogging())
                        NVNGXLog("Hooked_CreateFeature: DLSS FG ACTIVATED (ID 0x%X)", featureID);

                    // CRITICAL: Signal FG activation to the detection system
                    // This enables usage-based detection instead of DLL-based detection
                    g_FGCompat.SetDLSSFGMultiplier(2);
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
    return Hooked_CreateFeature_Process(oCreateFeature_D3D11, ctx, featureID, params, handle);
}
NVSDK_NGX_Result __cdecl Hooked_CreateFeature_D3D12(void* ctx, int featureID, NVSDK_NGX_Parameter* params,
                                                    void** handle) {
    return Hooked_CreateFeature_Process(oCreateFeature_D3D12, ctx, featureID, params, handle);
}
NVSDK_NGX_Result __cdecl Hooked_CreateFeature_VULKAN(void* ctx, int featureID, NVSDK_NGX_Parameter* params,
                                                     void** handle) {
    return Hooked_CreateFeature_Process(oCreateFeature_VULKAN, ctx, featureID, params, handle);
}

void NVNGXHook::Install() {
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
