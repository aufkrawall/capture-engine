#include "nvngx_hook.h"
#include "hook_common.h"
#include <MinHook.h>
#include <mutex>
#include <vector>

// --- NGX SDK Mini-Definitions ---
enum NVSDK_NGX_Result {
    NVSDK_NGX_Result_Success = 0x1,
};

struct NVSDK_NGX_Parameter {
    virtual ~NVSDK_NGX_Parameter() {} 
};

#define NVSDK_NGX_Parameter_AutoExposure "DLSS.AutoExposure"
#define NVSDK_NGX_Parameter_CreateFlags "DLSS.Feature.Create.Flags"
#define NVSDK_NGX_Parameter_ExposureScale "DLSS.Exposure.Scale"
#define NVSDK_NGX_Parameter_PreExposure "DLSS.Pre.Exposure"
#define NVSDK_NGX_Parameter_Sharpness "DLSS.Sharpness"

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

// Bit 6 in CreateFlags is AutoExposure
const int NVSDK_NGX_DLSS_Feature_Flags_AutoExposure = 1 << 6;

// --- Typed VTable Hooks ---
typedef void (STDMETHODCALLTYPE *PFN_SetI)(NVSDK_NGX_Parameter* pThis, const char* InName, int InValue);
typedef void (STDMETHODCALLTYPE *PFN_SetUI)(NVSDK_NGX_Parameter* pThis, const char* InName, unsigned int InValue);
typedef void (STDMETHODCALLTYPE *PFN_SetF)(NVSDK_NGX_Parameter* pThis, const char* InName, float InValue);

static PFN_SetI oSetI = nullptr;
static PFN_SetUI oSetUI = nullptr;
static PFN_SetF oSetF = nullptr;

static bool IsSafeString(const char* s) {
    if (!s || (uintptr_t)s < 0x10000) return false;
    return true; 
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
                uint32_t finalPreset = specificOverride > 0 ? specificOverride : cfg.parsed.srPreset;
                if (finalPreset > 0) {
                    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) LogOncePerParam(InName, "NVNGX: Overriding DLSS preset (SetI) %s = %u (was %d)", InName, finalPreset, InValue);
                    InValue = (int)finalPreset;
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
            // Fix: Check if it IS a preset parameter before identifying the override value
            bool isDlssPreset = false;
            uint32_t specificOverride = 0;

            if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_DLAA) == 0) { isDlssPreset = true; specificOverride = cfg.parsed.presetDLAA; }
            else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Quality) == 0) { isDlssPreset = true; specificOverride = cfg.parsed.presetQuality; }
            else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Balanced) == 0) { isDlssPreset = true; specificOverride = cfg.parsed.presetBalanced; }
            else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_Performance) == 0) { isDlssPreset = true; specificOverride = cfg.parsed.presetPerformance; }
            else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraPerformance) == 0) { isDlssPreset = true; specificOverride = cfg.parsed.presetUltraPerformance; }
            else if (strcmp(InName, NVSDK_NGX_Parameter_DLSS_Hint_Render_Preset_UltraQuality) == 0) { isDlssPreset = true; specificOverride = cfg.parsed.presetUltraQuality; }

            if (isDlssPreset) {
                uint32_t finalPreset = specificOverride > 0 ? specificOverride : cfg.parsed.srPreset;
                if (finalPreset > 0) {
                    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) LogOncePerParam(InName, "NVNGX: Overriding DLSS preset %s = %u (was %u)", InName, finalPreset, InValue);
                    InValue = finalPreset;
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

        if (strcmp(InName, NVSDK_NGX_Parameter_Sharpness) == 0) {
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

static std::mutex g_NVHookMutex;
static std::vector<void*> g_HookedVTables;

void EnsureVTableHooks(NVSDK_NGX_Parameter* pParams) {
    if (!pParams) return;
    std::lock_guard<std::mutex> lock(g_NVHookMutex);

    void** vtable = *(void***)pParams;
    if (!vtable) return;

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
            if (MH_CreateHook(vtable[idx], pHook, ppOrig) == MH_OK) {
                MH_EnableHook(vtable[idx]);
            }
        };

        Install(3, (LPVOID)&Hooked_SetI, (LPVOID*)&oSetI);
        Install(4, (LPVOID)&Hooked_SetUI, (LPVOID*)&oSetUI);
        Install(6, (LPVOID)&Hooked_SetF, (LPVOID*)&oSetF);

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
        if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->debugLogging) LogOncePerParam(NVSDK_NGX_Parameter_Sharpness, "NVNGX: Injecting initial sharpness = %.2f", overrideVal);
        ((PFN_SetF)vtable[6])(pParams, NVSDK_NGX_Parameter_Sharpness, overrideVal);
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
        EnsureVTableHooks(*OutParameters);
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

void NVNGXHook::Install() {
    if (m_Installed.exchange(true)) return;

    HMODULE hNGX = GetModuleHandleA("nvngx.dll");
    if (!hNGX) hNGX = GetModuleHandleA("_nvngx.dll");
    if (!hNGX) { m_Installed = false; return; }

    auto InstallFactory = [&](const char* name, LPVOID pHook, LPVOID* ppOrig) {
        LPVOID pProc = (LPVOID)GetProcAddress(hNGX, name);
        if (pProc) {
            if (MH_CreateHook(pProc, pHook, ppOrig) == MH_OK) {
                MH_EnableHook(pProc);
                NVNGXLog("NVNGX: Factory %s detoured", name);
            }
        }
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
}

void NVNGXHook::Uninstall() {
    if (!m_Installed.exchange(false)) return;
    MH_DisableHook(MH_ALL_HOOKS);
}
