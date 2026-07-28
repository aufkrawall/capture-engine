#include "nvngx_hook.h"
#include <atomic>
#include <mutex>
#include <type_traits>
#include <unordered_map>
#include <vector>
#include "../common/fg_detection.h"  // For FG_SetDLSSActive
#include "../common/hook_common.h"
#include "../common/nvngx_parameter_abi.h"
#include "../wrappers/iat_hook.h"
#include "../wrappers/vtable_hook.h"

// --- NGX SDK Mini-Definitions ---
enum NVSDK_NGX_Result {
    NVSDK_NGX_Result_Success = 0x1,
    NVSDK_NGX_Result_FAIL_FeatureNotSupported = 0xBAD00001,
};

struct NVSDK_NGX_Parameter {
    virtual ~NVSDK_NGX_Parameter() {}
};

struct NVSDK_NGX_FeatureDiscoveryInfo;
struct NVSDK_NGX_FeatureRequirement;

typedef NVSDK_NGX_Result(STDMETHODCALLTYPE* PFN_NVSDK_NGX_GetFeatureRequirements)(
    void* InAdapter, const NVSDK_NGX_FeatureDiscoveryInfo* InDiscoveryInfo, NVSDK_NGX_FeatureRequirement* OutSupported);

static PFN_NVSDK_NGX_GetFeatureRequirements oGetFeatureRequirements_D3D11 = nullptr;
static PFN_NVSDK_NGX_GetFeatureRequirements oGetFeatureRequirements_D3D12 = nullptr;
static PFN_NVSDK_NGX_GetFeatureRequirements oGetFeatureRequirements_VULKAN = nullptr;

#define NVSDK_NGX_Parameter_AutoExposure "DLSS.AutoExposure"
#define NVSDK_NGX_Parameter_CreateFlags "DLSS.Feature.Create.Flags"
#define NVSDK_NGX_Parameter_ExposureScale "DLSS.Exposure.Scale"
#define NVSDK_NGX_Parameter_PreExposure "DLSS.Pre.Exposure"
#define NVSDK_NGX_Parameter_Sharpness "DLSS.Sharpness"
#define NVSDK_NGX_Parameter_Sharpness_Alt "Sharpness"
#define NVSDK_NGX_Parameter_PerfQualityValue "PerfQualityValue"
#define NVSDK_NGX_Parameter_Width "Width"
#define NVSDK_NGX_Parameter_Height "Height"
#define NVSDK_NGX_Parameter_OutWidth "OutWidth"
#define NVSDK_NGX_Parameter_OutHeight "OutHeight"

static std::mutex g_ParamMapMutex;
static std::map<void*, int> g_ParameterQualityMap;
struct DLSSDims {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t outWidth = 0;
    uint32_t outHeight = 0;
};
static std::map<void*, DLSSDims> g_ParameterDimsMap;

// DLSS Preset Hints
#define NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA "DLSS.Hint.Render.Preset.DLAA"
#define NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality "DLSS.Hint.Render.Preset.Quality"
#define NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced "DLSS.Hint.Render.Preset.Balanced"
#define NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance "DLSS.Hint.Render.Preset.Performance"
#define NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance "DLSS.Hint.Render.Preset.UltraPerformance"
#define NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality "DLSS.Hint.Render.Preset.UltraQuality"

// Ray Reconstruction Preset Hints
#define NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA "RayReconstruction.Hint.Render.Preset.DLAA"
#define NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality "RayReconstruction.Hint.Render.Preset.Quality"
#define NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced \
    "RayReconstruction.Hint.Render.Preset.Balanced"
#define NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance \
    "RayReconstruction.Hint.Render.Preset.Performance"
#define NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance \
    "RayReconstruction.Hint.Render.Preset.UltraPerformance"
#define NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality \
    "RayReconstruction.Hint.Render.Preset.UltraQuality"

#define NVSDK_NGX_Parameter_RayReconstruction_Available "RayReconstruction.Available"
#define NVSDK_NGX_Parameter_RayReconstruction_FeatureInitResult "RayReconstruction.FeatureInitResult"

// DLSS Multi-Frame Generation (MFG) parameter
#define NVSDK_NGX_Parameter_FrameGenerationMultiplier "FrameGenerationMultiplier"  // 2=2x, 3=3x, 4=4x

// Feature IDs for DLSS components
const int NVSDK_NGX_Feature_DLSS_SR = 1;                // Super Resolution
const int NVSDK_NGX_Feature_FrameGeneration = 9;        // Frame Generation (FG)
const int NVSDK_NGX_Feature_FrameGeneration_11 = 0xB;   // Frame Generation (alternate ID)
const int NVSDK_NGX_Feature_RayReconstruction = 13;     // Ray Reconstruction
const int NVSDK_NGX_Feature_MultiFrameGeneration = 18;  // Multi-Frame Generation (MFG) - generates 2x or 3x frames

// Bit 6 in CreateFlags is AutoExposure
const int NVSDK_NGX_DLSS_Feature_Flags_AutoExposure = 1 << 6;

// Bit 5 in CreateFlags is DoSharpening
const int NVSDK_NGX_DLSS_Feature_Flags_DoSharpening = 1 << 5;

// --- Typed VTable Hooks ---
typedef void(STDMETHODCALLTYPE* PFN_SetI)(NVSDK_NGX_Parameter* pThis, const char* InName, int InValue);
typedef void(STDMETHODCALLTYPE* PFN_SetUI)(NVSDK_NGX_Parameter* pThis, const char* InName, unsigned int InValue);
typedef void(STDMETHODCALLTYPE* PFN_SetF)(NVSDK_NGX_Parameter* pThis, const char* InName, float InValue);
typedef NVSDK_NGX_Result(STDMETHODCALLTYPE* PFN_GetI)(NVSDK_NGX_Parameter* pThis, const char* InName, int* OutValue);
typedef NVSDK_NGX_Result(STDMETHODCALLTYPE* PFN_GetUI)(NVSDK_NGX_Parameter* pThis, const char* InName,
                                                       unsigned int* OutValue);

struct ParameterVTableOriginals {
    PFN_SetI setI = nullptr;
    PFN_SetUI setUI = nullptr;
    PFN_SetF setF = nullptr;
    PFN_GetI getI = nullptr;
    PFN_GetUI getUI = nullptr;
};

static std::mutex g_NVHookMutex;
static std::unordered_map<void**, ParameterVTableOriginals> g_ParameterVTableOriginals;

static ParameterVTableOriginals GetParameterOriginals(NVSDK_NGX_Parameter* params) {
    if (!params)
        return {};
    void** vtable = *reinterpret_cast<void***>(params);
    std::lock_guard<std::mutex> lock(g_NVHookMutex);
    const auto it = g_ParameterVTableOriginals.find(vtable);
    return it == g_ParameterVTableOriginals.end() ? ParameterVTableOriginals{} : it->second;
}

// Track active hints per Quality Level (0=Perf, 1=Bal, 2=Qual, 3=UP, 4=UQ,
// 5=DLAA) Initialize to '?'
static std::atomic<char> g_UserPresetHints[6] = {'?', '?', '?', '?', '?', '?'};

static char PresetIDToChar(uint32_t id) {
    if (id >= 1 && id <= 26)
        return 'A' + (id - 1);  // 1=A ... 26=Z
    return '?';
}

static void UpdatePresetHint(const char* paramName, uint32_t presetVal, const char* source) {
    int idx = -1;
    if (strcmp(paramName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance) == 0)
        idx = 0;
    else if (strcmp(paramName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced) == 0)
        idx = 1;
    else if (strcmp(paramName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality) == 0)
        idx = 2;
    else if (strcmp(paramName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance) == 0)
        idx = 3;
    else if (strcmp(paramName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality) == 0)
        idx = 4;
    else if (strcmp(paramName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA) == 0)
        idx = 5;

    if (idx >= 0) {
        char c = PresetIDToChar(presetVal);
        g_UserPresetHints[idx].store(c);
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
            NVNGXLog("NVNGX: UpdatePresetHint [%s]: %s -> %u ('%c')", source, paramName, presetVal, c);
        }
    } else if (strstr(paramName, "RayReconstruction")) {
        // Log RR presets too for visibility
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
            NVNGXLog("NVNGX: UpdatePresetHint RR [%s]: %s -> %u", source, paramName, presetVal);
        }
    }
}

static bool IsSafePtr(const void* p) {
    if (!p || (uintptr_t)p < 0x10000)
        return false;
    return true;
}

static bool IsSafeString(const char* s) {
    return IsSafePtr(s);
}

static int GetConfiguredFGMultiplier(const GraphicsConfig& cfg) {
    return NormalizeDLSSFGFactor(cfg.parsed.dlssFGFactor);
}

static void LogOncePerParam(const char* param, const char* msg, ...) {
    static std::mutex s_mutex;
    static std::vector<std::pair<std::string, std::string>> s_LastLogs;

    // Format for comparison (private stack buffer)
    char buffer[1024];
    va_list args;
    va_start(args, msg);
    vsnprintf(buffer, sizeof(buffer), msg, args);
    va_end(args);

    bool shouldLog = false;
    {
        std::lock_guard<std::mutex> lock(s_mutex);
        for (auto& entry : s_LastLogs) {
            if (entry.first == param) {
                if (entry.second == buffer) {
                    return;  // Same message, skip
                }
                entry.second = buffer;
                shouldLog = true;
                break;
            }
        }
        if (!shouldLog) {
            s_LastLogs.push_back({param, buffer});
            shouldLog = true;
        }
    }
    if (shouldLog) {
        NVNGXLog("%s", buffer);
    }
}

void STDMETHODCALLTYPE Hooked_SetI(NVSDK_NGX_Parameter* pThis, const char* InName, int InValue) {
    const PFN_SetI original = GetParameterOriginals(pThis).setI;
    if (!original)
        return;
    if (IsSafeString(InName)) {
        if (strcmp(InName, NVSDK_NGX_Parameter_CreateFlags) == 0) {
            std::string mode = GetActiveGraphicsConfig().dlssAutoExposure;
            if (mode == "on") {
                if (!(InValue & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure)) {
                    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                        LogOncePerParam(InName, "NVNGX: Forcing AutoExposure flag ON (was 0x%X)", InValue);
                    InValue |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
                }
            } else if (mode == "off") {
                if (InValue & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) {
                    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                        LogOncePerParam(InName, "NVNGX: Forcing AutoExposure flag OFF (was 0x%X)", InValue);
                    InValue &= ~NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
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
                    if (InValue & NVSDK_NGX_DLSS_Feature_Flags_DoSharpening) {
                        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                            LogOncePerParam(InName, "NVNGX: Forcing DoSharpening flag OFF (was 0x%X)", InValue);
                        InValue &= ~NVSDK_NGX_DLSS_Feature_Flags_DoSharpening;
                    }
                } else {
                    if (!(InValue & NVSDK_NGX_DLSS_Feature_Flags_DoSharpening)) {
                        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                            LogOncePerParam(InName, "NVNGX: Forcing DoSharpening flag ON (was 0x%X)", InValue);
                        InValue |= NVSDK_NGX_DLSS_Feature_Flags_DoSharpening;
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
                    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                        NVNGXLog("NVNGX: SetI: Overriding %s: %d -> %u", InName, InValue, val);
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
            std::lock_guard<std::mutex> lock(g_ParamMapMutex);
            g_ParameterQualityMap[pThis] = InValue;
        }
    }
    original(pThis, InName, InValue);
}

void STDMETHODCALLTYPE Hooked_SetUI(NVSDK_NGX_Parameter* pThis, const char* InName, unsigned int InValue) {
    const PFN_SetUI original = GetParameterOriginals(pThis).setUI;
    if (!original)
        return;
    if (IsSafeString(InName)) {
        if (strcmp(InName, NVSDK_NGX_Parameter_CreateFlags) == 0) {
            // AutoExposure flag override
            std::string mode = GetActiveGraphicsConfig().dlssAutoExposure;
            if (mode == "on") {
                if (!(InValue & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure)) {
                    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                        LogOncePerParam(InName, "NVNGX: Forcing AutoExposure flag ON (SetUI, was 0x%X)", InValue);
                    InValue |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
                }
            } else if (mode == "off") {
                if (InValue & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) {
                    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                        LogOncePerParam(InName, "NVNGX: Forcing AutoExposure flag OFF (SetUI, was 0x%X)", InValue);
                    InValue &= ~NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
                }
            }
            // DoSharpening flag override
            const auto& cfg = GetActiveGraphicsConfig();
            if (cfg.parsed.dlssSharpening > -1.5f) {
                const bool forceDisableSharpening = (cfg.parsed.dlssSharpening < 0.0001f);
                if (forceDisableSharpening) {
                    if (InValue & NVSDK_NGX_DLSS_Feature_Flags_DoSharpening) {
                        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                            LogOncePerParam(InName, "NVNGX: Forcing DoSharpening flag OFF (SetUI, was 0x%X)", InValue);
                        InValue &= ~NVSDK_NGX_DLSS_Feature_Flags_DoSharpening;
                    }
                } else {
                    if (!(InValue & NVSDK_NGX_DLSS_Feature_Flags_DoSharpening)) {
                        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging())
                            LogOncePerParam(InName, "NVNGX: Forcing DoSharpening flag ON (SetUI, was 0x%X)", InValue);
                        InValue |= NVSDK_NGX_DLSS_Feature_Flags_DoSharpening;
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
            std::lock_guard<std::mutex> lock(g_ParamMapMutex);
            g_ParameterQualityMap[pThis] = (int)InValue;
        } else if (strcmp(InName, NVSDK_NGX_Parameter_Width) == 0) {
            std::lock_guard<std::mutex> lock(g_ParamMapMutex);
            g_ParameterDimsMap[pThis].width = InValue;
        } else if (strcmp(InName, NVSDK_NGX_Parameter_Height) == 0) {
            std::lock_guard<std::mutex> lock(g_ParamMapMutex);
            g_ParameterDimsMap[pThis].height = InValue;
        } else if (strcmp(InName, NVSDK_NGX_Parameter_OutWidth) == 0) {
            std::lock_guard<std::mutex> lock(g_ParamMapMutex);
            g_ParameterDimsMap[pThis].outWidth = InValue;
        } else if (strcmp(InName, NVSDK_NGX_Parameter_OutHeight) == 0) {
            std::lock_guard<std::mutex> lock(g_ParamMapMutex);
            g_ParameterDimsMap[pThis].outHeight = InValue;
        }
    }
    original(pThis, InName, InValue);
}

void STDMETHODCALLTYPE Hooked_SetF(NVSDK_NGX_Parameter* pThis, const char* InName, float InValue) {
    const PFN_SetF original = GetParameterOriginals(pThis).setF;
    if (!original)
        return;
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
    NVSDK_NGX_Result res = original(pThis, InName, OutValue);
    if (res == NVSDK_NGX_Result_Success && OutValue && IsSafeString(InName)) {
        // If the game/driver is reading back a preset or quality value, we capture
        // it
        if (strcmp(InName, NVSDK_NGX_Parameter_PerfQualityValue) == 0) {
            std::lock_guard<std::mutex> lock(g_ParamMapMutex);
            g_ParameterQualityMap[pThis] = *OutValue;
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
    NVSDK_NGX_Result res = original(pThis, InName, OutValue);
    if (res == NVSDK_NGX_Result_Success && OutValue && IsSafeString(InName)) {
        if (strcmp(InName, NVSDK_NGX_Parameter_PerfQualityValue) == 0) {
            std::lock_guard<std::mutex> lock(g_ParamMapMutex);
            g_ParameterQualityMap[pThis] = (int)*OutValue;
        } else if (strstr(InName, "Preset")) {
            UpdatePresetHint(InName, *OutValue, "GetUI");
        }
    }
    return res;
}

void EnsureVTableHooks(NVSDK_NGX_Parameter* pParams) {
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
        std::lock_guard<std::mutex> lock(g_NVHookMutex);
        auto [routeIt, inserted] = g_ParameterVTableOriginals.try_emplace(vtable);
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
                if (VTableHook::Create(&vtable[idx], hook, &captured) != VTableHook::Success || !captured)
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
