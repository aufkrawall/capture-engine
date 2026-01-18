#include "nvngx_hook.h"
#include "hook_common.h"
#include "../wrappers/vtable_hook.h"
#include "../wrappers/iat_hook.h"
#include <mutex>
#include <vector>


// --- NGX SDK Mini-Definitions ---
enum NVSDK_NGX_Result {
    NVSDK_NGX_Result_Success = 0x1,
};

struct NVSDK_NGX_Parameter {
    virtual ~NVSDK_NGX_Parameter() {} 
};

struct NVSDK_NGX_FeatureDiscoveryInfo;
struct NVSDK_NGX_FeatureRequirement;

typedef NVSDK_NGX_Result (STDMETHODCALLTYPE *PFN_NVSDK_NGX_GetFeatureRequirements)(void* InAdapter, const NVSDK_NGX_FeatureDiscoveryInfo* InDiscoveryInfo, NVSDK_NGX_FeatureRequirement* OutSupported);

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
struct DLSSDims { uint32_t width=0; uint32_t height=0; uint32_t outWidth=0; uint32_t outHeight=0; };
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
#define NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced "RayReconstruction.Hint.Render.Preset.Balanced"
#define NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance "RayReconstruction.Hint.Render.Preset.Performance"
#define NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance "RayReconstruction.Hint.Render.Preset.UltraPerformance"
#define NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality "RayReconstruction.Hint.Render.Preset.UltraQuality"

#define NVSDK_NGX_Parameter_RayReconstruction_Available "RayReconstruction.Available"
#define NVSDK_NGX_Parameter_RayReconstruction_FeatureInitResult "RayReconstruction.FeatureInitResult"

// Bit 6 in CreateFlags is AutoExposure
const int NVSDK_NGX_DLSS_Feature_Flags_AutoExposure = 1 << 6;

// Bit 5 in CreateFlags is DoSharpening
const int NVSDK_NGX_DLSS_Feature_Flags_DoSharpening = 1 << 5;

// --- Typed VTable Hooks ---
typedef void (STDMETHODCALLTYPE *PFN_SetI)(NVSDK_NGX_Parameter* pThis, const char* InName, int InValue);
typedef void (STDMETHODCALLTYPE *PFN_SetUI)(NVSDK_NGX_Parameter* pThis, const char* InName, unsigned int InValue);
typedef void (STDMETHODCALLTYPE *PFN_SetF)(NVSDK_NGX_Parameter* pThis, const char* InName, float InValue);
typedef NVSDK_NGX_Result (STDMETHODCALLTYPE *PFN_GetI)(NVSDK_NGX_Parameter* pThis, const char* InName, int* OutValue);
typedef NVSDK_NGX_Result (STDMETHODCALLTYPE *PFN_GetUI)(NVSDK_NGX_Parameter* pThis, const char* InName, unsigned int* OutValue);

static PFN_SetI oSetI = nullptr;
static PFN_SetUI oSetUI = nullptr;
static PFN_SetF oSetF = nullptr;
static PFN_GetI oGetI = nullptr;
static PFN_GetUI oGetUI = nullptr;

// Track active hints per Quality Level (0=Perf, 1=Bal, 2=Qual, 3=UP, 4=UQ, 5=DLAA)
// Initialize to '?'
static std::atomic<char> g_UserPresetHints[6] = { '?', '?', '?', '?', '?', '?' };

static char PresetIDToChar(uint32_t id) {
    if (id >= 1 && id <= 13) return 'A' + (id - 1); // 1=A ... 13=M
    return '?';
}

static void UpdatePresetHint(const char* paramName, uint32_t presetVal, const char* source) {
    int idx = -1;
    if (strcmp(paramName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance) == 0) idx = 0;
    else if (strcmp(paramName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced) == 0) idx = 1;
    else if (strcmp(paramName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality) == 0) idx = 2;
    else if (strcmp(paramName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance) == 0) idx = 3;
    else if (strcmp(paramName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality) == 0) idx = 4;
    else if (strcmp(paramName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA) == 0) idx = 5;
    
    if (idx >= 0) {
        char c = PresetIDToChar(presetVal);
        g_UserPresetHints[idx].store(c);
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) {
            NVNGXLog("NVNGX: UpdatePresetHint [%s]: %s -> %u ('%c')", source, paramName, presetVal, c);
        }
    } else if (strstr(paramName, "RayReconstruction")) {
        // Log RR presets too for visibility
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) {
            NVNGXLog("NVNGX: UpdatePresetHint RR [%s]: %s -> %u", source, paramName, presetVal);
        }
    }
}

static bool IsSafePtr(const void* p) {
    if (!p || (uintptr_t)p < 0x10000) return false;
    return true;
}

static bool IsSafeString(const char* s) {
    return IsSafePtr(s);
}

static void LogOncePerParam(const char* param, const char* msg, ...) {
    static CRITICAL_SECTION s_cs;
    static volatile LONG s_csInit = 0;
    static std::vector<std::pair<std::string, std::string>> s_LastLogs;

    // Thread-safe CS init
    if (InterlockedCompareExchange(&s_csInit, 1, 0) == 0) {
        InitializeCriticalSection(&s_cs);
        InterlockedExchange(&s_csInit, 2);
    }
    while (s_csInit < 2) { Sleep(0); }

    // Format for comparison (private stack buffer)
    char buffer[1024];
    va_list args;
    va_start(args, msg);
    vsnprintf(buffer, sizeof(buffer), msg, args);
    va_end(args);

    EnterCriticalSection(&s_cs);
    for (auto& entry : s_LastLogs) {
        if (entry.first == param) {
            if (entry.second == buffer) {
                LeaveCriticalSection(&s_cs);
                return; // Same message, skip
            }
            entry.second = buffer;
            LeaveCriticalSection(&s_cs);
            NVNGXLog("%s", buffer);
            return;
        }
    }
    s_LastLogs.push_back({param, buffer});
    LeaveCriticalSection(&s_cs);
    NVNGXLog("%s", buffer);
}

void STDMETHODCALLTYPE Hooked_SetI(NVSDK_NGX_Parameter* pThis, const char* InName, int InValue) {
    if (IsSafeString(InName)) {

        if (strcmp(InName, NVSDK_NGX_Parameter_CreateFlags) == 0) {
            std::string mode = GetActiveGraphicsConfig().dlssAutoExposure;
            if (mode == "on") {
                if (!(InValue & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure)) {
                    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) LogOncePerParam(InName, "NVNGX: Forcing AutoExposure flag ON (was 0x%X)", InValue);
                    InValue |= NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
                }
            } else if (mode == "off") {
                if (InValue & NVSDK_NGX_DLSS_Feature_Flags_AutoExposure) {
                    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) LogOncePerParam(InName, "NVNGX: Forcing AutoExposure flag OFF (was 0x%X)", InValue);
                    InValue &= ~NVSDK_NGX_DLSS_Feature_Flags_AutoExposure;
                }
            }

            // DLSS sharpening is controlled by both a float parameter (Sharpness) AND a feature flag.
            // Setting sharpness to 0 does not necessarily disable sharpening unless this bit is cleared.
            const auto &cfg = GetActiveGraphicsConfig();
            if (cfg.parsed.dlssSharpening > -1.5f) {
                // Treat explicit 0 as "off" (matches user expectation in our config).
                const bool forceDisableSharpening = (cfg.parsed.dlssSharpening < 0.0001f);
                if (forceDisableSharpening) {
                    if (InValue & NVSDK_NGX_DLSS_Feature_Flags_DoSharpening) {
                        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging)
                            LogOncePerParam(InName, "NVNGX: Forcing DoSharpening flag OFF (was 0x%X)", InValue);
                        InValue &= ~NVSDK_NGX_DLSS_Feature_Flags_DoSharpening;
                    }
                } else {
                    if (!(InValue & NVSDK_NGX_DLSS_Feature_Flags_DoSharpening)) {
                        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging)
                            LogOncePerParam(InName, "NVNGX: Forcing DoSharpening flag ON (was 0x%X)", InValue);
                        InValue |= NVSDK_NGX_DLSS_Feature_Flags_DoSharpening;
                    }
                }
            }
        } else if (strcmp(InName, NVSDK_NGX_Parameter_AutoExposure) == 0) {
            std::string mode = GetActiveGraphicsConfig().dlssAutoExposure;
            if (mode == "on") InValue = 1;
            else if (mode == "off") InValue = 0;
            if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) LogOncePerParam(InName, "NVNGX: Forcing AutoExposure to %d", InValue);
        } else {
             const auto& cfg = GetActiveGraphicsConfig();
            
            // Preset Overrides (Super Resolution) via SetI
            bool isDlssPreset = false;
            uint32_t specificOverride = 0;

            if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA) == 0) { isDlssPreset = true; specificOverride = cfg.parsed.presetDLAA; }
            else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality) == 0) { isDlssPreset = true; specificOverride = cfg.parsed.presetQuality; }
            else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced) == 0) { isDlssPreset = true; specificOverride = cfg.parsed.presetBalanced; }
            else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance) == 0) { isDlssPreset = true; specificOverride = cfg.parsed.presetPerformance; }
            else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance) == 0) { isDlssPreset = true; specificOverride = cfg.parsed.presetUltraPerformance; }
            else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality) == 0) { isDlssPreset = true; specificOverride = cfg.parsed.presetUltraQuality; }

            if (isDlssPreset) {
                uint32_t val = (specificOverride > 0) ? specificOverride : cfg.parsed.srPreset;
                if (val > 0) {
                    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) 
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

                if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA) == 0) { isRRPreset = true; specificRROverride = cfg.parsed.rrPresetDLAA; }
                else if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality) == 0) { isRRPreset = true; specificRROverride = cfg.parsed.rrPresetQuality; }
                else if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced) == 0) { isRRPreset = true; specificRROverride = cfg.parsed.rrPresetBalanced; }
                else if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance) == 0) { isRRPreset = true; specificRROverride = cfg.parsed.rrPresetPerformance; }
                else if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance) == 0) { isRRPreset = true; specificRROverride = cfg.parsed.rrPresetUltraPerformance; }
                else if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality) == 0) { isRRPreset = true; specificRROverride = cfg.parsed.rrPresetUltraQuality; }

                if (isRRPreset) {
                    uint32_t finalRRPreset = specificRROverride > 0 ? specificRROverride : cfg.parsed.rrPreset;
                    if (finalRRPreset > 0) {
                        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) LogOncePerParam(InName, "NVNGX: Overriding RR preset (SetI) %s = %u (was %d)", InName, finalRRPreset, InValue);
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
    oSetI(pThis, InName, InValue);
}

void STDMETHODCALLTYPE Hooked_SetUI(NVSDK_NGX_Parameter* pThis, const char* InName, unsigned int InValue) {
    if (IsSafeString(InName)) {

        if (strcmp(InName, NVSDK_NGX_Parameter_AutoExposure) == 0) {
            std::string mode = GetActiveGraphicsConfig().dlssAutoExposure;
            if (mode == "on") InValue = 1;
            else if (mode == "off") InValue = 0;
        }
        else {
            const auto& cfg = GetActiveGraphicsConfig();
            
            // Preset Overrides (Super Resolution)
            bool isDlssPreset = false;
            uint32_t specificOverride = 0;

            if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA) == 0) { isDlssPreset = true; specificOverride = cfg.parsed.presetDLAA; }
            else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality) == 0) { isDlssPreset = true; specificOverride = cfg.parsed.presetQuality; }
            else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced) == 0) { isDlssPreset = true; specificOverride = cfg.parsed.presetBalanced; }
            else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance) == 0) { isDlssPreset = true; specificOverride = cfg.parsed.presetPerformance; }
            else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance) == 0) { isDlssPreset = true; specificOverride = cfg.parsed.presetUltraPerformance; }
            else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality) == 0) { isDlssPreset = true; specificOverride = cfg.parsed.presetUltraQuality; }

            if (isDlssPreset) {
                uint32_t val = (specificOverride > 0) ? specificOverride : cfg.parsed.srPreset;
                
                // Debug: Log the override decision
                static int setUILogCount = 0;
                if (setUILogCount < 10 && g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) {
                    NVNGXLog("NVNGX: SetUI(%s): game=%u, specific=%u, global=%u, final=%u", 
                             InName, InValue, specificOverride, cfg.parsed.srPreset, val);
                    setUILogCount++;
                }
                
                if (val > 0) {
                    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) 
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

                if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA) == 0) { isRRPreset = true; specificRROverride = cfg.parsed.rrPresetDLAA; }
                else if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality) == 0) { isRRPreset = true; specificRROverride = cfg.parsed.rrPresetQuality; }
                else if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced) == 0) { isRRPreset = true; specificRROverride = cfg.parsed.rrPresetBalanced; }
                else if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance) == 0) { isRRPreset = true; specificRROverride = cfg.parsed.rrPresetPerformance; }
                else if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance) == 0) { isRRPreset = true; specificRROverride = cfg.parsed.rrPresetUltraPerformance; }
                else if (strcmp(InName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality) == 0) { isRRPreset = true; specificRROverride = cfg.parsed.rrPresetUltraQuality; }

                if (isRRPreset) {
                    uint32_t finalRRPreset = specificRROverride > 0 ? specificRROverride : cfg.parsed.rrPreset;
                    if (finalRRPreset > 0) {
                        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) LogOncePerParam(InName, "NVNGX: Overriding RR preset %s = %u (was %u)", InName, finalRRPreset, InValue);
                        InValue = finalRRPreset;
                    }
                }
            }
        }

        // Capture Quality/Dimensions for Overlay
        if (strcmp(InName, NVSDK_NGX_Parameter_PerfQualityValue) == 0) {
             std::lock_guard<std::mutex> lock(g_ParamMapMutex);
             g_ParameterQualityMap[pThis] = (int)InValue;
        }
        else if (strcmp(InName, NVSDK_NGX_Parameter_Width) == 0) {
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
    oSetUI(pThis, InName, InValue);
}

void STDMETHODCALLTYPE Hooked_SetF(NVSDK_NGX_Parameter* pThis, const char* InName, float InValue) {
    if (IsSafeString(InName)) {
        const auto& cfg = GetActiveGraphicsConfig();

        if (cfg.dlssExposureNormalization == "on") {
            if (strcmp(InName, NVSDK_NGX_Parameter_ExposureScale) == 0 || strcmp(InName, NVSDK_NGX_Parameter_PreExposure) == 0) {
                if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) LogOncePerParam(InName, "NVNGX: Normalizing exposure %s = 1.0", InName);
                InValue = 1.0f;
            }
        } else if (cfg.dlssExposureNormalization == "off") {
            // Explicitly off - do nothing, let game control it
        }

        if (strcmp(InName, NVSDK_NGX_Parameter_Sharpness) == 0 || strcmp(InName, NVSDK_NGX_Parameter_Sharpness_Alt) == 0) {
            if (cfg.parsed.dlssSharpening > -1.5f) {
                float overrideVal = cfg.parsed.dlssSharpening;
                if (overrideVal < -0.5f) overrideVal = 0.0f; // "off" = 0.0
                
                if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) LogOncePerParam(InName, "NVNGX: Overriding sharpness = %.2f (was %.2f)", overrideVal, InValue);
                InValue = overrideVal;
            }
        }
    }
    oSetF(pThis, InName, InValue);
}

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetI(NVSDK_NGX_Parameter* pThis, const char* InName, int* OutValue) {
    NVSDK_NGX_Result res = oGetI(pThis, InName, OutValue);
    if (res == NVSDK_NGX_Result_Success && OutValue && IsSafeString(InName)) {
        // If the game/driver is reading back a preset or quality value, we capture it
        if (strcmp(InName, NVSDK_NGX_Parameter_PerfQualityValue) == 0) {
            std::lock_guard<std::mutex> lock(g_ParamMapMutex);
            g_ParameterQualityMap[pThis] = *OutValue;
        } else if (strstr(InName, "Preset")) {
            UpdatePresetHint(InName, (uint32_t)*OutValue, "GetI");
        }
    }
    return res;
}

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetUI(NVSDK_NGX_Parameter* pThis, const char* InName, unsigned int* OutValue) {
    NVSDK_NGX_Result res = oGetUI(pThis, InName, OutValue);
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

static std::mutex g_NVHookMutex;
static std::vector<void*> g_HookedVTables;

void EnsureVTableHooks(NVSDK_NGX_Parameter* pParams) {
    if (!IsSafePtr(pParams)) {
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) NVNGXLog("EnsureVTableHooks: Skipped (Unsafe Ptr %p)", pParams);
        return;
    }
    std::lock_guard<std::mutex> lock(g_NVHookMutex);

    void** vtable = *(void***)pParams;
    if (!vtable) return;

    // Log vtable address to ensure it's sane
    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) NVNGXLog("EnsureVTableHooks: pParams=%p, vtable=%p", pParams, vtable);

    // Check if VTable is already hooked
    bool alreadyHooked = false;
    for (void* v : g_HookedVTables) {
        if (v == vtable) {
            alreadyHooked = true;
            break;
        }
    }

    if (!alreadyHooked) {
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) NVNGXLog("NVNGX: Installing typed hooks on VTable at %p", vtable);
        
        auto Install = [&](int idx, LPVOID pHook, LPVOID* ppOrig) {
            if (!vtable[idx] || *ppOrig) return;
            VTableHook::Create(&vtable[idx], pHook, ppOrig);
        };

        Install(3, (LPVOID)&Hooked_SetI, (LPVOID*)&oSetI);
        Install(4, (LPVOID)&Hooked_SetUI, (LPVOID*)&oSetUI);
        Install(6, (LPVOID)&Hooked_SetF, (LPVOID*)&oSetF);
        Install(8, (LPVOID)&Hooked_GetI, (LPVOID*)&oGetI);
        Install(9, (LPVOID)&Hooked_GetUI, (LPVOID*)&oGetUI);

        g_HookedVTables.push_back(vtable);
    }

    // Initial Injection via SetI (VT[3])
    const auto& cfg = GetActiveGraphicsConfig();

    std::string mode = cfg.dlssAutoExposure;
    if (mode == "on" || mode == "off") {
        int val = (mode == "on") ? 1 : 0;
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) LogOncePerParam(NVSDK_NGX_Parameter_AutoExposure, "NVNGX: Injecting initial %s = %d", NVSDK_NGX_Parameter_AutoExposure, val);
        if (vtable[3]) ((PFN_SetI)vtable[3])(pParams, NVSDK_NGX_Parameter_AutoExposure, val);
        if (vtable[4]) ((PFN_SetUI)vtable[4])(pParams, NVSDK_NGX_Parameter_AutoExposure, (unsigned int)val);
    }

    // Initial Injection for Presets (via SetUI VT[4] and SetI VT[3])
    auto InjectPreset = [&](const char* name, uint32_t val) {
        if (val > 0) {
             if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) LogOncePerParam(name, "NVNGX: Injecting initial %s = %u", name, val);
             
             // Update our local state so the overlay knows about this forced preset!
             UpdatePresetHint(name, val, "InitialInjection");

             // Try via SetUI (Standard)
             if (vtable[4]) ((PFN_SetUI)vtable[4])(pParams, name, val);
             
             // Try via SetI (Legacy/Compat)
             if (vtable[3]) ((PFN_SetI)vtable[3])(pParams, name, (int)val);
        }
    };

    InjectPreset(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA, cfg.parsed.presetDLAA ? cfg.parsed.presetDLAA : cfg.parsed.srPreset);
    InjectPreset(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality, cfg.parsed.presetQuality ? cfg.parsed.presetQuality : cfg.parsed.srPreset);
    InjectPreset(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced, cfg.parsed.presetBalanced ? cfg.parsed.presetBalanced : cfg.parsed.srPreset);
    InjectPreset(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance, cfg.parsed.presetPerformance ? cfg.parsed.presetPerformance : cfg.parsed.srPreset);
    InjectPreset(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance, cfg.parsed.presetUltraPerformance ? cfg.parsed.presetUltraPerformance : cfg.parsed.srPreset);
    InjectPreset(NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality, cfg.parsed.presetUltraQuality ? cfg.parsed.presetUltraQuality : cfg.parsed.srPreset);

    InjectPreset(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA, cfg.parsed.rrPresetDLAA ? cfg.parsed.rrPresetDLAA : cfg.parsed.rrPreset);
    InjectPreset(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality, cfg.parsed.rrPresetQuality ? cfg.parsed.rrPresetQuality : cfg.parsed.rrPreset);
    InjectPreset(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced, cfg.parsed.rrPresetBalanced ? cfg.parsed.rrPresetBalanced : cfg.parsed.rrPreset);
    InjectPreset(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance, cfg.parsed.rrPresetPerformance ? cfg.parsed.rrPresetPerformance : cfg.parsed.rrPreset);
    InjectPreset(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance, cfg.parsed.rrPresetUltraPerformance ? cfg.parsed.rrPresetUltraPerformance : cfg.parsed.rrPreset);
    InjectPreset(NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality, cfg.parsed.rrPresetUltraQuality ? cfg.parsed.rrPresetUltraQuality : cfg.parsed.rrPreset);

    // Initial Sharpening Injection (VT[6])
    if (cfg.parsed.dlssSharpening > -1.5f && vtable[6]) {
        float overrideVal = cfg.parsed.dlssSharpening;
        if (overrideVal < -0.5f) overrideVal = 0.0f; // "off" = 0.0
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) {
            LogOncePerParam(NVSDK_NGX_Parameter_Sharpness, "NVNGX: Injecting initial sharpness = %.2f", overrideVal);
        }

        // Try both names for compatibility
        ((PFN_SetF)vtable[6])(pParams, NVSDK_NGX_Parameter_Sharpness, overrideVal);
        ((PFN_SetF)vtable[6])(pParams, NVSDK_NGX_Parameter_Sharpness_Alt, overrideVal);
    }

    // Force Ray Reconstruction Availability
    if (cfg.forceRayReconstruction) {
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) {
            LogOncePerParam("NVNGX_ForceRR_Caps", "NVNGX: Injecting RayReconstruction.Available=1 and FeatureInitResult=1");
        }
        // Force available = 1 (true)
        if (vtable[3]) ((PFN_SetI)vtable[3])(pParams, NVSDK_NGX_Parameter_RayReconstruction_Available, 1);
        if (vtable[4]) ((PFN_SetUI)vtable[4])(pParams, NVSDK_NGX_Parameter_RayReconstruction_Available, 1);
        
        // Force init result = 0 (Success/Supported check usually looks for 0 or 1 depending on param, but FeatureInitResult is often a success code 0 or 1)
        // Actually FeatureInitResult usually stores the NVSDK_NGX_Result. 1 = Success.
        if (vtable[3]) ((PFN_SetI)vtable[3])(pParams, NVSDK_NGX_Parameter_RayReconstruction_FeatureInitResult, 1);
        if (vtable[4]) ((PFN_SetUI)vtable[4])(pParams, NVSDK_NGX_Parameter_RayReconstruction_FeatureInitResult, 1);
    }

}

// --- Factory Hooks ---
typedef NVSDK_NGX_Result (STDMETHODCALLTYPE *PFN_NVSDK_NGX_GetParameters)(NVSDK_NGX_Parameter** OutParameters);
static PFN_NVSDK_NGX_GetParameters oGetParameters_D3D11 = nullptr;
static PFN_NVSDK_NGX_GetParameters oAllocateParameters_D3D11 = nullptr;
static PFN_NVSDK_NGX_GetParameters oGetCapabilityParameters_D3D11 = nullptr;

static PFN_NVSDK_NGX_GetParameters oGetParameters_D3D12 = nullptr;
static PFN_NVSDK_NGX_GetParameters oAllocateParameters_D3D12 = nullptr;
static PFN_NVSDK_NGX_GetParameters oGetCapabilityParameters_D3D12 = nullptr;

static PFN_NVSDK_NGX_GetParameters oGetParameters_VULKAN = nullptr;
static PFN_NVSDK_NGX_GetParameters oAllocateParameters_VULKAN = nullptr;
static PFN_NVSDK_NGX_GetParameters oGetCapabilityParameters_VULKAN = nullptr;

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_ProcessParameters(PFN_NVSDK_NGX_GetParameters original, NVSDK_NGX_Parameter** OutParameters, const char* source) {
    if (!original) return NVSDK_NGX_Result_Success;
    NVSDK_NGX_Result res = original(OutParameters);
    if (res == NVSDK_NGX_Result_Success && OutParameters && *OutParameters) {
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) NVNGXLog("Hooked_ProcessParameters: Success, Params=%p, *Params=%p", OutParameters, *OutParameters);
        EnsureVTableHooks(*OutParameters);
    } else {
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) NVNGXLog("Hooked_ProcessParameters: Result=%X, Params=%p", res, OutParameters);
    }
    return res;
}

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetParams_D3D11(NVSDK_NGX_Parameter** p) { return Hooked_ProcessParameters(oGetParameters_D3D11, p, "GetParams_D3D11"); }
NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_AllocParams_D3D11(NVSDK_NGX_Parameter** p) { return Hooked_ProcessParameters(oAllocateParameters_D3D11, p, "AllocParams_D3D11"); }
NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetCaps_D3D11(NVSDK_NGX_Parameter** p) { return Hooked_ProcessParameters(oGetCapabilityParameters_D3D11, p, "GetCaps_D3D11"); }

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetParams_D3D12(NVSDK_NGX_Parameter** p) { return Hooked_ProcessParameters(oGetParameters_D3D12, p, "GetParams_D3D12"); }
NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_AllocParams_D3D12(NVSDK_NGX_Parameter** p) { return Hooked_ProcessParameters(oAllocateParameters_D3D12, p, "AllocParams_D3D12"); }
NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetCaps_D3D12(NVSDK_NGX_Parameter** p) { return Hooked_ProcessParameters(oGetCapabilityParameters_D3D12, p, "GetCaps_D3D12"); }

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetParams_VULKAN(NVSDK_NGX_Parameter** p) { return Hooked_ProcessParameters(oGetParameters_VULKAN, p, "GetParams_VULKAN"); }
NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_AllocParams_VULKAN(NVSDK_NGX_Parameter** p) { return Hooked_ProcessParameters(oAllocateParameters_VULKAN, p, "AllocParams_VULKAN"); }
NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetCaps_VULKAN(NVSDK_NGX_Parameter** p) { return Hooked_ProcessParameters(oGetCapabilityParameters_VULKAN, p, "GetCaps_VULKAN"); }

// Definition matching nvsdk_ngx_defs.h
typedef struct NVSDK_NGX_Application_Identifier
{
    unsigned int IdentifierType; // Enum in reality but size int
    union v {
        struct {
            const char* ProjectId;
            int EngineType;
            const char* EngineVersion;
        } ProjectDesc;
        unsigned long long ApplicationId;
    } v;
} NVSDK_NGX_Application_Identifier;

typedef struct NVSDK_NGX_FeatureDiscoveryInfo
{
    unsigned int SDKVersion;
    int FeatureID;
    NVSDK_NGX_Application_Identifier Identifier;
    const wchar_t* ApplicationDataPath;
    const void* FeatureInfo;
} NVSDK_NGX_FeatureDiscoveryInfo;

typedef struct NVSDK_NGX_FeatureRequirement
{
    int FeatureSupported; // 0 = Supported
    unsigned int MinHWArchitecture;
    char MinOSVersion[255];
} NVSDK_NGX_FeatureRequirement;

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_ProcessFeatureRequirements(PFN_NVSDK_NGX_GetFeatureRequirements original, void* InAdapter, const NVSDK_NGX_FeatureDiscoveryInfo* InDiscoveryInfo, NVSDK_NGX_FeatureRequirement* OutSupported) {
    NVSDK_NGX_Result res = NVSDK_NGX_Result_Success;
    
    // Call original first to populate real data
    if (original) {
        res = original(InAdapter, InDiscoveryInfo, OutSupported);
    } else {
        // Fallback if original missing (unlikely if hooked)
        res = (NVSDK_NGX_Result)0xBAD00000; // Fail
    }

    if (InDiscoveryInfo && OutSupported) {
        // Feature 13 is Ray Reconstruction
        if (InDiscoveryInfo->FeatureID == 13) {
             const auto& cfg = GetActiveGraphicsConfig();
             if (cfg.forceRayReconstruction) {
                 if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) {
                    LogOncePerParam("GetFeatureRequirements_13", "NVNGX: Spoofing Ray Reconstruction (Feature 13) as SUPPORTED");
                 }
                 
                 // Force Success Result
                 res = NVSDK_NGX_Result_Success;
                 
                 // Force Structure
                 OutSupported->FeatureSupported = 0; // NVSDK_NGX_FeatureSupportResult_Supported
                 
                 // Fill placeholders if empty
                 if (OutSupported->MinHWArchitecture == 0) OutSupported->MinHWArchitecture = 0; // Supported
                 if (OutSupported->MinOSVersion[0] == 0) strcpy_s(OutSupported->MinOSVersion, "10.0.19041");
             }
        }
    }
    return res;
}

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetFeatureRequirements_D3D11(void* InAdapter, const NVSDK_NGX_FeatureDiscoveryInfo* InDiscoveryInfo, NVSDK_NGX_FeatureRequirement* OutSupported) {
    return Hooked_ProcessFeatureRequirements(oGetFeatureRequirements_D3D11, InAdapter, InDiscoveryInfo, OutSupported);
}

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetFeatureRequirements_D3D12(void* InAdapter, const NVSDK_NGX_FeatureDiscoveryInfo* InDiscoveryInfo, NVSDK_NGX_FeatureRequirement* OutSupported) {
    return Hooked_ProcessFeatureRequirements(oGetFeatureRequirements_D3D12, InAdapter, InDiscoveryInfo, OutSupported);
}

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetFeatureRequirements_VULKAN(void* InAdapter, const NVSDK_NGX_FeatureDiscoveryInfo* InDiscoveryInfo, NVSDK_NGX_FeatureRequirement* OutSupported) {
    return Hooked_ProcessFeatureRequirements(oGetFeatureRequirements_VULKAN, InAdapter, InDiscoveryInfo, OutSupported);
}

// --- CreateFeature Hooks ---
// CreateFeature signature is __cdecl (standard C export)
// CreateFeature signature (4 args based on NGX docs/reversing)
typedef NVSDK_NGX_Result (__cdecl *PFN_NVSDK_NGX_CreateFeature)(void* InCmdList, int InFeatureID, NVSDK_NGX_Parameter* InParameters, void** OutHandle);
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
    if (!g_IPC || !g_IPC->GetSharedMem()) return;

    // UpdateDLSSVersion: Only look for the CORE DLSS implementation
    // User requested to ignore "Streamline version number", so we focus on nvngx_dlss
    HMODULE hNGX = GetModuleHandleA("nvngx_dlss.dll");
    
    // If not loaded yet, check dlssg
    if (!hNGX) hNGX = GetModuleHandleA("nvngx_dlssg.dll");
    
    // DO NOT accept sl.dlss.dll as it reports wrapper versions
    
    // Fallback?
    if (!hNGX) hNGX = GetModuleHandleA("_nvngx.dll");
    
    // Safety check
    if (!hNGX) return;

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
                    
                    if (g_IPC->GetSharedMem()->debugLogging) {
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
        if (hint != '?') return hint;
    }
    // If no hint set, check our configuration for what we INTENDED to set
    // This handles cases where SetI hook was missed or game bypassed it, but we want to show the target.
    // Also, if we are overriding, this is likely what is active.
    
    char configChar = '?';
    const auto& cfg = GetActiveGraphicsConfig();
    
    // Map qualityValue to specific config
    uint32_t presetVal = 0;
    switch (qualityValue) {
        case 0: presetVal = cfg.parsed.presetPerformance; break;
        case 1: presetVal = cfg.parsed.presetBalanced; break;
        case 2: presetVal = cfg.parsed.presetQuality; break;
        case 3: presetVal = cfg.parsed.presetUltraPerformance; break;
        case 4: presetVal = cfg.parsed.presetUltraQuality; break;
        case 5: presetVal = cfg.parsed.presetDLAA; break;
    }
    
    // Fallback to global srPreset if specific is 0
    if (presetVal == 0) presetVal = cfg.parsed.srPreset;
    
    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) {
         static int logCount = 0;
         if (logCount < 5) {
             NVNGXLog("NVNGX: GetPresetChar quality=%d -> presetVal=%u (GlobalSR=%u)", qualityValue, presetVal, cfg.parsed.srPreset);
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
    if (!IsSafePtr(info) || !g_IPC || !g_IPC->GetSharedMem()) return;

    auto& state = g_IPC->GetSharedMem()->dlssState;
    // Version is updated at install time now
    
    // Safety check for FeatureInfo
    if (!IsSafePtr(info->FeatureInfo)) return;

    if (info->FeatureID == 1) { // Super Resolution
        state.srActive = true;
        if (info->FeatureInfo) {
             const auto* params = (const NVSDK_NGX_DLSS_Create_Params*)info->FeatureInfo;
             state.renderScale = CalculateScale(params);
             state.srPreset = GetPresetChar(params->InPerfQualityValue);
             
             if (g_IPC->GetSharedMem()->debugLogging) {
                 LogOncePerParam("DLSS_SR_Create", "NVNGX: Created DLSS SR. Scale=%.2f, Quality=%d, Preset=%c", state.renderScale.load(), params->InPerfQualityValue, state.srPreset.load());
             }
        }
    } else if (info->FeatureID == 13) { // Ray Reconstruction
        state.rrActive = true;
        if (g_IPC->GetSharedMem()->debugLogging) {
             LogOncePerParam("DLSS_RR_Create", "NVNGX: Created DLSS RR.");
        }
    }
}

NVSDK_NGX_Result __cdecl Hooked_CreateFeature_Process(PFN_NVSDK_NGX_CreateFeature original, void* ctx, int featureID, NVSDK_NGX_Parameter* params, void** handle) {
    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) NVNGXLog("Hooked_CreateFeature_Process: Entry ctx=%p, ID=%d, params=%p", ctx, featureID, params);
    
    NVSDK_NGX_Result res = NVSDK_NGX_Result_Success;
    if (original) res = original(ctx, featureID, params, handle);

    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) NVNGXLog("Hooked_CreateFeature_Process: Original Result=%X", res);
    
    // On success, update status
    if ((int)res >= 0) { 
        // Update version if not yet found (handling lazy load)
        if (g_IPC && g_IPC->GetSharedMem()) {
            if (g_IPC->GetSharedMem()->dlssState.versionMajor == 0) {
                 UpdateDLSSVersion();
            }
        }

        if (g_IPC && g_IPC->GetSharedMem()) {
            auto& state = g_IPC->GetSharedMem()->dlssState;
            
            if (featureID == 1) {
                state.srActive = true;
                if (g_IPC->GetSharedMem()->debugLogging) NVNGXLog("Hooked_CreateFeature: DLSS SR Activated (ID 1)");
                
                // Proactive Sync: Query effective presets from the parameter object
                auto SyncPreset = [&](const char* name) {
                    unsigned int val = 0;
                    NVSDK_NGX_Result getRes;
                    if (oGetUI) {
                        getRes = oGetUI(params, name, &val);
                        if (getRes == NVSDK_NGX_Result_Success) {
                            UpdatePresetHint(name, val, "SyncUI");
                            return;
                        } else if (g_IPC->GetSharedMem()->debugLogging) {
                             NVNGXLog("NVNGX: oGetUI(%s) returned %X", name, getRes);
                        }
                    }
                    if (oGetI) {
                        getRes = oGetI(params, name, (int*)&val);
                        if (getRes == NVSDK_NGX_Result_Success) {
                            UpdatePresetHint(name, val, "SyncI");
                        } else if (g_IPC->GetSharedMem()->debugLogging) {
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

                // Probe for undocumented internal parameters that might expose driver-forced preset
                static bool probeOnce = false;
                if (!probeOnce && oGetUI && g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) {
                    probeOnce = true;
                    const char* probeParams[] = {
                        "DLSS.Preset.Active",
                        "DLSS.Preset.Effective", 
                        "DLSS.Feature.Preset",
                        "DLSS.Internal.Preset",
                        "DLSS.Preset",
                        "DLSS.RenderPreset",
                        "DLSS.ActivePreset",
                        "NVSDK_NGX_Parameter_DLSS_Preset",
                        "Preset",
                        nullptr
                    };
                    for (int i = 0; probeParams[i]; i++) {
                        unsigned int probedVal = 0;
                        NVSDK_NGX_Result probeRes = oGetUI(params, probeParams[i], &probedVal);
                        if (probeRes == NVSDK_NGX_Result_Success && probedVal > 0) {
                            NVNGXLog("NVNGX: PROBE SUCCESS! '%s' = %u ('%c')", probeParams[i], probedVal, PresetIDToChar(probedVal));
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
                        if (g_IPC->GetSharedMem()->debugLogging) NVNGXLog("Hooked_CreateFeature: Quality=%d -> srPreset='%c'", q, state.srPreset.load());
                        g_ParameterQualityMap.erase(params);
                    }
                    if (g_ParameterDimsMap.count(params)) {
                        const auto& d = g_ParameterDimsMap[params];
                        float w1 = (float)d.width;
                        float w2 = (float)d.outWidth;
                        if (w1 > 0 && w2 > 0) {
                            if (w1 >= w2) state.renderScale = w1 / w2;
                            else state.renderScale = w2 / w1;
                        }
                        g_ParameterDimsMap.erase(params);
                    }
                }
            } else if (featureID == 13 || featureID == 0xB) {
                if (featureID == 13) {
                     state.rrActive = true;
                     if (g_IPC->GetSharedMem()->debugLogging) NVNGXLog("Hooked_CreateFeature: DLSS RR Activated (ID 13)");
                } else if (featureID == 0xB) { 
                     state.fgActive = true;
                     if (g_IPC->GetSharedMem()->debugLogging) NVNGXLog("Hooked_CreateFeature: DLSS FG Activated (ID 11)");
                }
            } else {
                if (g_IPC->GetSharedMem()->debugLogging) NVNGXLog("Hooked_CreateFeature: Other ID 0x%X Activated", featureID);
            }

            // Force Config Overrides (Final step)
            const auto& cfg = GetActiveGraphicsConfig();
            if (cfg.parsed.srPreset > 0) {
                char oldP = state.srPreset.load();
                state.srPreset = PresetIDToChar(cfg.parsed.srPreset);
                if (g_IPC->GetSharedMem()->debugLogging) {
                    NVNGXLog("NV_NGX: Final Force SR Preset: '%c' -> '%c' (Config ID %u, From String: %s)", 
                        oldP, state.srPreset.load(), cfg.parsed.srPreset, cfg.dlssSRPreset.c_str());
                }
            }
            if (cfg.parsed.rrPreset > 0 && featureID == 13) {
                 state.rrPreset = PresetIDToChar(cfg.parsed.rrPreset);
                 if (g_IPC->GetSharedMem()->debugLogging) NVNGXLog("NV_NGX: Final Force RR Preset to '%c'", state.rrPreset.load());
            }
        }
    }
    return res;
}

NVSDK_NGX_Result __cdecl Hooked_CreateFeature_D3D11(void* ctx, int featureID, NVSDK_NGX_Parameter* params, void** handle) {
    return Hooked_CreateFeature_Process(oCreateFeature_D3D11, ctx, featureID, params, handle);
}
NVSDK_NGX_Result __cdecl Hooked_CreateFeature_D3D12(void* ctx, int featureID, NVSDK_NGX_Parameter* params, void** handle) {
    return Hooked_CreateFeature_Process(oCreateFeature_D3D12, ctx, featureID, params, handle);
}
NVSDK_NGX_Result __cdecl Hooked_CreateFeature_VULKAN(void* ctx, int featureID, NVSDK_NGX_Parameter* params, void** handle) {
    return Hooked_CreateFeature_Process(oCreateFeature_VULKAN, ctx, featureID, params, handle);
}






void NVNGXHook::Install() {
    if (m_Installed.exchange(true)) return;

    // Update version once at install time
    UpdateDLSSVersion();

    // Force initialize user's configured preset (handles Streamline bypass)
    const auto& cfg = GetActiveGraphicsConfig();
    uint32_t presetVal = cfg.parsed.srPreset;
    if (presetVal > 0) {
        // We can't access g_IPC yet reliably if it's too early, but we can set static state?
        // Wait, 'state' is in shared mem. g_IPC might not be connected if game just started.
        // But GetSharedMem() checks connection.
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

    HMODULE hNGX = GetModuleHandleA("nvngx.dll");
    if (!hNGX) hNGX = GetModuleHandleA("_nvngx.dll");
    
    // Also try Streamline DLSS core (it exports the same NGX factories sometimes)
    if (!hNGX) hNGX = GetModuleHandleA("sl.dlss.dll");
    
    if (!hNGX) { m_Installed = false; return; }

    // Register Dynamic Hook for GetProcAddress interception
    // and Patch IAT for static imports
    auto InstallFactory = [&](const char* name, LPVOID pHook, LPVOID* ppOrig) {
        // Register for dynamic loading via GetProcAddress
        IATHook::RegisterDynamicHook(name, pHook, ppOrig);
        
        // Patch explicit imports in all modules (for _nvngx.dll etc)
        void* dummy;
        IATHook::PatchIATAllModules("_nvngx.dll", name, pHook, &dummy);
        IATHook::PatchIATAllModules("nvngx.dll", name, pHook, &dummy);
        IATHook::PatchIATAllModules("sl.dlss.dll", name, pHook, &dummy);
    };

    InstallFactory("NVSDK_NGX_D3D11_GetParameters", (LPVOID)&Hooked_GetParams_D3D11, (LPVOID*)&oGetParameters_D3D11);
    InstallFactory("NVSDK_NGX_D3D11_AllocateParameters", (LPVOID)&Hooked_AllocParams_D3D11, (LPVOID*)&oAllocateParameters_D3D11);
    InstallFactory("NVSDK_NGX_D3D11_GetCapabilityParameters", (LPVOID)&Hooked_GetCaps_D3D11, (LPVOID*)&oGetCapabilityParameters_D3D11);

    InstallFactory("NVSDK_NGX_D3D12_GetParameters", (LPVOID)&Hooked_GetParams_D3D12, (LPVOID*)&oGetParameters_D3D12);
    InstallFactory("NVSDK_NGX_D3D12_AllocateParameters", (LPVOID)&Hooked_AllocParams_D3D12, (LPVOID*)&oAllocateParameters_D3D12);
    InstallFactory("NVSDK_NGX_D3D12_GetCapabilityParameters", (LPVOID)&Hooked_GetCaps_D3D12, (LPVOID*)&oGetCapabilityParameters_D3D12);

    InstallFactory("NVSDK_NGX_VULKAN_GetParameters", (LPVOID)&Hooked_GetParams_VULKAN, (LPVOID*)&oGetParameters_VULKAN);
    InstallFactory("NVSDK_NGX_VULKAN_AllocateParameters", (LPVOID)&Hooked_AllocParams_VULKAN, (LPVOID*)&oAllocateParameters_VULKAN);
    InstallFactory("NVSDK_NGX_VULKAN_GetCapabilityParameters", (LPVOID)&Hooked_GetCaps_VULKAN, (LPVOID*)&oGetCapabilityParameters_VULKAN);

    InstallFactory("NVSDK_NGX_D3D11_GetFeatureRequirements", (LPVOID)&Hooked_GetFeatureRequirements_D3D11, (LPVOID*)&oGetFeatureRequirements_D3D11);
    InstallFactory("NVSDK_NGX_D3D12_GetFeatureRequirements", (LPVOID)&Hooked_GetFeatureRequirements_D3D12, (LPVOID*)&oGetFeatureRequirements_D3D12);
    InstallFactory("NVSDK_NGX_VULKAN_GetFeatureRequirements", (LPVOID)&Hooked_GetFeatureRequirements_VULKAN, (LPVOID*)&oGetFeatureRequirements_VULKAN);

    InstallFactory("NVSDK_NGX_D3D11_CreateFeature", (LPVOID)&Hooked_CreateFeature_D3D11, (LPVOID*)&oCreateFeature_D3D11);
    InstallFactory("NVSDK_NGX_D3D12_CreateFeature", (LPVOID)&Hooked_CreateFeature_D3D12, (LPVOID*)&oCreateFeature_D3D12);
    InstallFactory("NVSDK_NGX_VULKAN_CreateFeature", (LPVOID)&Hooked_CreateFeature_VULKAN, (LPVOID*)&oCreateFeature_VULKAN);
}

void NVNGXHook::Uninstall() {
    if (!m_Installed.exchange(false)) return;
    // IAT hooks are permanent until process exit usually
}
