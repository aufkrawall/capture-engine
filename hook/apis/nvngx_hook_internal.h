#pragma once

struct NVSDK_NGX_Parameter;

struct DLSSDims;

struct ParameterVTableOriginals;

struct NVSDK_NGX_DLSS_Create_Params;

#include "nvngx_hook.h"

#include <atomic>

#include <mutex>

#include <type_traits>

#include <unordered_map>

#include <vector>

#include <cstdint>

#include "../common/fg_detection.h"  // For FG_SetDLSSActive

#include "../common/hook_common.h"

#include "../common/ngx_feature_lifecycle.h"

#include "../common/nvngx_parameter_abi.h"

#include "../common/ngx_module_policy.h"

#include "../wrappers/iat_hook.h"

#include "../wrappers/inline_hook.h"

#include "../wrappers/vtable_hook.h"

// --- NGX SDK Mini-Definitions ---
enum NVSDK_NGX_Result {
    NVSDK_NGX_Result_Success = 0x1,
    NVSDK_NGX_Result_FAIL_FeatureNotSupported = 0xBAD00001,
};

struct NVSDK_NGX_FeatureDiscoveryInfo;

struct NVSDK_NGX_FeatureRequirement;

typedef NVSDK_NGX_Result(STDMETHODCALLTYPE* PFN_NVSDK_NGX_GetFeatureRequirements)(
    void* InAdapter, const NVSDK_NGX_FeatureDiscoveryInfo* InDiscoveryInfo, NVSDK_NGX_FeatureRequirement* OutSupported);

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

#define NVSDK_NGX_Parameter_SuperSamplingDenoising_Available "SuperSamplingDenoising.Available"

#define NVSDK_NGX_Parameter_SuperSamplingDenoising_FeatureInitResult \
    "SuperSamplingDenoising.FeatureInitResult"

// DLSS Multi-Frame Generation (MFG) parameter
#define NVSDK_NGX_Parameter_FrameGenerationMultiplier "FrameGenerationMultiplier"  // 2=2x, 3=3x, 4=4x
#define NVSDK_NGX_DLSSG_Parameter_MultiFrameCount "MultiFrameCount"  // generated frames between real frames: 1=2x, 2=3x, 3=4x

// --- Typed VTable Hooks ---
typedef void(STDMETHODCALLTYPE* PFN_SetI)(NVSDK_NGX_Parameter* pThis, const char* InName, int InValue);

typedef void(STDMETHODCALLTYPE* PFN_SetUI)(NVSDK_NGX_Parameter* pThis, const char* InName, unsigned int InValue);

typedef void(STDMETHODCALLTYPE* PFN_SetF)(NVSDK_NGX_Parameter* pThis, const char* InName, float InValue);

typedef NVSDK_NGX_Result(STDMETHODCALLTYPE* PFN_GetI)(NVSDK_NGX_Parameter* pThis, const char* InName, int* OutValue);

typedef NVSDK_NGX_Result(STDMETHODCALLTYPE* PFN_GetUI)(NVSDK_NGX_Parameter* pThis, const char* InName,
                                                       unsigned int* OutValue);

// --- Factory Hooks ---
typedef NVSDK_NGX_Result(STDMETHODCALLTYPE* PFN_NVSDK_NGX_GetParameters)(NVSDK_NGX_Parameter** OutParameters);

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

// --- CreateFeature Hooks ---
// CreateFeature signature is __cdecl (standard C export)
// CreateFeature signature (4 args based on NGX docs/reversing)
typedef NVSDK_NGX_Result(__cdecl* PFN_NVSDK_NGX_CreateFeature)(void* InCmdList, int InFeatureID,
                                                               NVSDK_NGX_Parameter* InParameters, void** OutHandle);

typedef NVSDK_NGX_Result(__cdecl* PFN_NVSDK_NGX_CreateFeatureVulkan1)(
    void* InDevice, void* InCmdList, int InFeatureID, NVSDK_NGX_Parameter* InParameters, void** OutHandle);

typedef NVSDK_NGX_Result(__cdecl* PFN_NVSDK_NGX_EvaluateFeature)(
    void* InCmdList, const void* InFeatureHandle, const NVSDK_NGX_Parameter* InParameters, void* InCallback);

typedef NVSDK_NGX_Result(__cdecl* PFN_NVSDK_NGX_ReleaseFeature)(void* InFeatureHandle);

void STDMETHODCALLTYPE Hooked_SetI(NVSDK_NGX_Parameter* pThis, const char* InName, int InValue);

void STDMETHODCALLTYPE Hooked_SetUI(NVSDK_NGX_Parameter* pThis, const char* InName, unsigned int InValue);

void STDMETHODCALLTYPE Hooked_SetF(NVSDK_NGX_Parameter* pThis, const char* InName, float InValue);

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetI(NVSDK_NGX_Parameter* pThis, const char* InName, int* OutValue);

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetUI(NVSDK_NGX_Parameter* pThis, const char* InName, unsigned int* OutValue);

void EnsureVTableHooks(NVSDK_NGX_Parameter* pParams);

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_ProcessParameters(PFN_NVSDK_NGX_GetParameters original, NVSDK_NGX_Parameter** OutParameters, const char* source);

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetParams_D3D11(NVSDK_NGX_Parameter** p);

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_AllocParams_D3D11(NVSDK_NGX_Parameter** p);

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetCaps_D3D11(NVSDK_NGX_Parameter** p);

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetParams_D3D12(NVSDK_NGX_Parameter** p);

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_AllocParams_D3D12(NVSDK_NGX_Parameter** p);

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetCaps_D3D12(NVSDK_NGX_Parameter** p);

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetParams_VULKAN(NVSDK_NGX_Parameter** p);

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_AllocParams_VULKAN(NVSDK_NGX_Parameter** p);

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetCaps_VULKAN(NVSDK_NGX_Parameter** p);

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_ProcessFeatureRequirements( PFN_NVSDK_NGX_GetFeatureRequirements original, void* InAdapter, const NVSDK_NGX_FeatureDiscoveryInfo* InDiscoveryInfo, NVSDK_NGX_FeatureRequirement* OutSupported);

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetFeatureRequirements_D3D11(void* InAdapter, const NVSDK_NGX_FeatureDiscoveryInfo* InDiscoveryInfo, NVSDK_NGX_FeatureRequirement* OutSupported);

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetFeatureRequirements_D3D12(void* InAdapter, const NVSDK_NGX_FeatureDiscoveryInfo* InDiscoveryInfo, NVSDK_NGX_FeatureRequirement* OutSupported);

NVSDK_NGX_Result STDMETHODCALLTYPE Hooked_GetFeatureRequirements_VULKAN(void* InAdapter, const NVSDK_NGX_FeatureDiscoveryInfo* InDiscoveryInfo, NVSDK_NGX_FeatureRequirement* OutSupported);

NVSDK_NGX_Result __cdecl Hooked_CreateFeature_D3D11(void* ctx, int featureID, NVSDK_NGX_Parameter* params, void** handle);

NVSDK_NGX_Result __cdecl Hooked_CreateFeature_D3D12(void* ctx, int featureID, NVSDK_NGX_Parameter* params, void** handle);

NVSDK_NGX_Result __cdecl Hooked_CreateFeature_VULKAN(void* ctx, int featureID, NVSDK_NGX_Parameter* params, void** handle);

NVSDK_NGX_Result __cdecl Hooked_CreateFeature_VULKAN1(void* device, void* ctx, int featureID,
                                                      NVSDK_NGX_Parameter* params, void** handle);

NVSDK_NGX_Result __cdecl Hooked_EvaluateFeature_D3D11(void* ctx, const void* handle,
                                                       const NVSDK_NGX_Parameter* params, void* callback);
NVSDK_NGX_Result __cdecl Hooked_EvaluateFeature_D3D11_C(void* ctx, const void* handle,
                                                         const NVSDK_NGX_Parameter* params, void* callback);
NVSDK_NGX_Result __cdecl Hooked_EvaluateFeature_D3D12(void* ctx, const void* handle,
                                                       const NVSDK_NGX_Parameter* params, void* callback);
NVSDK_NGX_Result __cdecl Hooked_EvaluateFeature_D3D12_C(void* ctx, const void* handle,
                                                         const NVSDK_NGX_Parameter* params, void* callback);
NVSDK_NGX_Result __cdecl Hooked_EvaluateFeature_VULKAN(void* ctx, const void* handle,
                                                        const NVSDK_NGX_Parameter* params, void* callback);
NVSDK_NGX_Result __cdecl Hooked_EvaluateFeature_VULKAN_C(void* ctx, const void* handle,
                                                          const NVSDK_NGX_Parameter* params, void* callback);

NVSDK_NGX_Result __cdecl Hooked_ReleaseFeature_D3D11(void* handle);
NVSDK_NGX_Result __cdecl Hooked_ReleaseFeature_D3D12(void* handle);
NVSDK_NGX_Result __cdecl Hooked_ReleaseFeature_VULKAN(void* handle);

void TrackNgxFeatureCreation(int featureID, NVSDK_NGX_Result result, void** handle);

struct NVSDK_NGX_Parameter {
    virtual ~NVSDK_NGX_Parameter() {}
};

inline PFN_NVSDK_NGX_GetFeatureRequirements nvngx_hook_oGetFeatureRequirements_D3D11 = nullptr;

inline PFN_NVSDK_NGX_GetFeatureRequirements nvngx_hook_oGetFeatureRequirements_D3D12 = nullptr;

inline PFN_NVSDK_NGX_GetFeatureRequirements nvngx_hook_oGetFeatureRequirements_VULKAN = nullptr;

inline std::mutex nvngx_hook_g_ParamMapMutex;

inline std::map<void*, int> nvngx_hook_g_ParameterQualityMap;

struct DLSSDims {
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t outWidth = 0;
    uint32_t outHeight = 0;
};

inline std::map<void*, DLSSDims> nvngx_hook_g_ParameterDimsMap;

inline const int nvngx_hook_NVSDK_NGX_Feature_FrameGeneration = 9;        // Frame Generation (FG)

inline const int nvngx_hook_NVSDK_NGX_Feature_FrameGeneration_11 = 0xB;   // Frame Generation (alternate ID)

inline const int nvngx_hook_NVSDK_NGX_Feature_RayReconstruction = 13;     // Ray Reconstruction

inline const int nvngx_hook_NVSDK_NGX_Feature_MultiFrameGeneration = 18;  // Multi-Frame Generation (MFG) - generates 2x or 3x frames

// Bit 6 in CreateFlags is AutoExposure
inline const int nvngx_hook_NVSDK_NGX_DLSS_Feature_Flags_AutoExposure = 1 << 6;

// Bit 5 in CreateFlags is DoSharpening
inline const int nvngx_hook_NVSDK_NGX_DLSS_Feature_Flags_DoSharpening = 1 << 5;

struct ParameterVTableOriginals {
    PFN_SetI setI = nullptr;
    PFN_SetUI setUI = nullptr;
    PFN_SetF setF = nullptr;
    PFN_GetI getI = nullptr;
    PFN_GetUI getUI = nullptr;
};

inline std::mutex nvngx_hook_g_NVHookMutex;

inline std::unordered_map<void**, ParameterVTableOriginals> nvngx_hook_g_ParameterVTableOriginals;

inline ParameterVTableOriginals GetParameterOriginals(NVSDK_NGX_Parameter* params) {
    if (!params)
        return {};
    void** vtable = *reinterpret_cast<void***>(params);
    std::lock_guard<std::mutex> lock(nvngx_hook_g_NVHookMutex);
    const auto it = nvngx_hook_g_ParameterVTableOriginals.find(vtable);
    return it == nvngx_hook_g_ParameterVTableOriginals.end() ? ParameterVTableOriginals{} : it->second;
}

// Track active hints per Quality Level (0=Perf, 1=Bal, 2=Qual, 3=UP, 4=UQ,
// 5=DLAA) Initialize to '?'
inline std::atomic<char> nvngx_hook_g_UserPresetHints[6] = {'?', '?', '?', '?', '?', '?'};

// Last observed Ray Reconstruction hint per quality level (order matches
// GetRRPresetIndex below). Initialized to a sentinel so the first observation
// of each hint is always logged.
inline std::atomic<uint32_t> nvngx_hook_g_UserRRPresetHints[6] = {UINT32_MAX, UINT32_MAX, UINT32_MAX,
                                                                  UINT32_MAX, UINT32_MAX, UINT32_MAX};

inline int GetRRPresetIndex(const char* paramName) {
    if (strcmp(paramName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_DLAA) == 0)
        return 0;
    if (strcmp(paramName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Quality) == 0)
        return 1;
    if (strcmp(paramName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Balanced) == 0)
        return 2;
    if (strcmp(paramName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_Performance) == 0)
        return 3;
    if (strcmp(paramName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraPerformance) == 0)
        return 4;
    if (strcmp(paramName, NVSDK_NGX_Parameter_RayReconstruction_Hint_Render_Preset_UltraQuality) == 0)
        return 5;
    return -1;
}

inline char PresetIDToChar(uint32_t id) {
    if (id >= 1 && id <= 26)
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        return 'A' + (id - 1);  // 1=A ... 26=Z
    return '?';
}

inline void UpdatePresetHint(const char* paramName, uint32_t presetVal, const char* source) {
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
        const char previous = nvngx_hook_g_UserPresetHints[idx].exchange(c);
        // Metered diagnostic: the game re-applies the same hints repeatedly
        // (SetI/SetUI/GetI/GetUI during startup and feature activation), and the
        // old code logged every call even when the value did not change. Log
        // only on actual changes; the first observation always logs because the
        // array starts at '?'.
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging() && previous != c) {
            NVNGXLog("NVNGX: UpdatePresetHint [%s]: %s -> %u ('%c')", source, paramName, presetVal, c);
        }
    } else if (strstr(paramName, "RayReconstruction")) {
        const int rrIdx = GetRRPresetIndex(paramName);
        if (rrIdx >= 0) {
            const uint32_t previous = nvngx_hook_g_UserRRPresetHints[rrIdx].exchange(presetVal);
            // Same change-only metering as the SR hints above.
            if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging() && previous != presetVal) {
                NVNGXLog("NVNGX: UpdatePresetHint RR [%s]: %s -> %u", source, paramName, presetVal);
            }
        }
    }
}

inline bool IsSafePtr(const void* p) {
    if (!p || (uintptr_t)p < 0x10000)
        return false;
    return true;
}

inline bool IsSafeString(const char* s) {
    return IsSafePtr(s);
}

inline int GetConfiguredFGMultiplier(const GraphicsConfig& cfg) {
    return NormalizeDLSSFGFactor(cfg.parsed.dlssFGFactor);
}

inline void LogOncePerParam(const char* param, const char* msg, ...) {
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

inline PFN_NVSDK_NGX_GetParameters nvngx_hook_oGetParameters_D3D11 = nullptr;

inline PFN_NVSDK_NGX_GetParameters nvngx_hook_oAllocateParameters_D3D11 = nullptr;

inline PFN_NVSDK_NGX_GetParameters nvngx_hook_oGetCapabilityParameters_D3D11 = nullptr;

inline PFN_NVSDK_NGX_GetParameters nvngx_hook_oGetParameters_D3D12 = nullptr;

inline PFN_NVSDK_NGX_GetParameters nvngx_hook_oAllocateParameters_D3D12 = nullptr;

inline PFN_NVSDK_NGX_GetParameters nvngx_hook_oGetCapabilityParameters_D3D12 = nullptr;

inline PFN_NVSDK_NGX_GetParameters nvngx_hook_oGetParameters_VULKAN = nullptr;

inline PFN_NVSDK_NGX_GetParameters nvngx_hook_oAllocateParameters_VULKAN = nullptr;

inline PFN_NVSDK_NGX_GetParameters nvngx_hook_oGetCapabilityParameters_VULKAN = nullptr;

inline PFN_NVSDK_NGX_CreateFeature nvngx_hook_oCreateFeature_D3D11 = nullptr;

inline PFN_NVSDK_NGX_CreateFeature nvngx_hook_oCreateFeature_D3D12 = nullptr;

inline PFN_NVSDK_NGX_EvaluateFeature nvngx_hook_oEvaluateFeature_D3D11 = nullptr;
inline PFN_NVSDK_NGX_EvaluateFeature nvngx_hook_oEvaluateFeature_D3D11_C = nullptr;
inline PFN_NVSDK_NGX_EvaluateFeature nvngx_hook_oEvaluateFeature_D3D12 = nullptr;
inline PFN_NVSDK_NGX_EvaluateFeature nvngx_hook_oEvaluateFeature_D3D12_C = nullptr;
inline PFN_NVSDK_NGX_EvaluateFeature nvngx_hook_oEvaluateFeature_VULKAN = nullptr;
inline PFN_NVSDK_NGX_EvaluateFeature nvngx_hook_oEvaluateFeature_VULKAN_C = nullptr;

inline PFN_NVSDK_NGX_ReleaseFeature nvngx_hook_oReleaseFeature_D3D11 = nullptr;
inline PFN_NVSDK_NGX_ReleaseFeature nvngx_hook_oReleaseFeature_D3D12 = nullptr;
inline PFN_NVSDK_NGX_ReleaseFeature nvngx_hook_oReleaseFeature_VULKAN = nullptr;

inline ce::ngx_lifecycle::FeatureHandleRegistry<64> nvngx_hook_g_FeatureRegistry;

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
