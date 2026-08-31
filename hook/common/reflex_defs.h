#pragma once
// Minimal NvAPI Reflex type definitions for FPS limiter and latency telemetry.
// Derived from the public NvAPI specification (nvapi_interface.h IDs).
// Keep only the entry points used by CE so injected hooks do not depend on the
// full NvAPI SDK.

#include <unknwn.h>
#include <cstddef>
#include <cstdint>

// NvAPI status codes (subset)
typedef int32_t NvAPI_Status;
constexpr NvAPI_Status NVAPI_OK = 0;
constexpr NvAPI_Status NVAPI_ERROR = -1;
constexpr NvAPI_Status NVAPI_INCOMPATIBLE_STRUCT_VERSION = -9;

using NvAPI_Bool = uint8_t;

// NvAPI_D3D_SetSleepMode parameters (version 1)
struct NV_SET_SLEEP_MODE_PARAMS_V1 {
    uint32_t version;                  // NV_SET_SLEEP_MODE_PARAMS_VER
    NvAPI_Bool bLowLatencyMode;        // 0 = disabled, 1 = enabled
    NvAPI_Bool bLowLatencyBoost;       // 0 = disabled, 1 = enabled
    uint32_t minimumIntervalUs;        // Minimum frame interval in microseconds (FPS cap)
    NvAPI_Bool bUseMarkersToOptimize;  // Use latency markers for optimization
    NvAPI_Bool bUseMinQueueTime;       // Consider all submission overlaps
    uint8_t rsvd[30];                  // Reserved. Must be set to 0s.
};
// Version tag — low 16 bits = struct size, high 16 bits = version number
constexpr uint32_t NV_SET_SLEEP_MODE_PARAMS_VER1 = (sizeof(NV_SET_SLEEP_MODE_PARAMS_V1)) | (1 << 16);
static_assert(sizeof(NV_SET_SLEEP_MODE_PARAMS_V1) == 44, "NVAPI sleep-mode params must match the 44-byte ABI");
static_assert(offsetof(NV_SET_SLEEP_MODE_PARAMS_V1, bLowLatencyMode) == 4, "Unexpected Reflex ABI layout");
static_assert(offsetof(NV_SET_SLEEP_MODE_PARAMS_V1, minimumIntervalUs) == 8, "Unexpected Reflex ABI layout");
static_assert(offsetof(NV_SET_SLEEP_MODE_PARAMS_V1, bUseMarkersToOptimize) == 12, "Unexpected Reflex ABI layout");
static_assert(offsetof(NV_SET_SLEEP_MODE_PARAMS_V1, bUseMinQueueTime) == 13, "Unexpected Reflex ABI layout");
static_assert(offsetof(NV_SET_SLEEP_MODE_PARAMS_V1, rsvd) == 14, "Unexpected Reflex ABI layout");

using NV_SET_SLEEP_MODE_PARAMS = NV_SET_SLEEP_MODE_PARAMS_V1;
constexpr uint32_t NV_SET_SLEEP_MODE_PARAMS_VER = NV_SET_SLEEP_MODE_PARAMS_VER1;

// NvAPI_D3D_GetLatency parameters (version 1). The explicit alignment word is
// the public ABI's implicit padding between version and the 8-byte reports; it
// keeps the layout identical under both 32-bit and 64-bit MinGW.
struct NV_LATENCY_RESULT_PARAMS_V1 {
    struct FrameReport {
        uint64_t frameID;
        uint64_t inputSampleTime;
        uint64_t simStartTime;
        uint64_t simEndTime;
        uint64_t renderSubmitStartTime;
        uint64_t renderSubmitEndTime;
        uint64_t presentStartTime;
        uint64_t presentEndTime;
        uint64_t driverStartTime;
        uint64_t driverEndTime;
        uint64_t osRenderQueueStartTime;
        uint64_t osRenderQueueEndTime;
        uint64_t gpuRenderStartTime;
        uint64_t gpuRenderEndTime;
        uint32_t gpuActiveRenderTimeUs;
        uint32_t gpuFrameTimeUs;
        uint8_t rsvd[120];
    };

    uint32_t version;
    uint32_t alignmentPadding;
    FrameReport frameReport[64];
    uint8_t rsvd[32];
};

using NV_LATENCY_RESULT_PARAMS = NV_LATENCY_RESULT_PARAMS_V1;
constexpr uint32_t NV_LATENCY_RESULT_PARAMS_VER = sizeof(NV_LATENCY_RESULT_PARAMS_V1) | (1 << 16);
static_assert(sizeof(NV_LATENCY_RESULT_PARAMS_V1::FrameReport) == 240,
              "NVAPI latency frame report must match the public ABI");
static_assert(offsetof(NV_LATENCY_RESULT_PARAMS_V1, frameReport) == 8, "Unexpected latency-report alignment");
static_assert(sizeof(NV_LATENCY_RESULT_PARAMS_V1) == 15400, "NVAPI latency result must match the public ABI");

// Function pointer types for the Reflex APIs we intercept
using PFN_NvAPI_D3D_SetSleepMode = NvAPI_Status(__cdecl*)(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pParams);
using PFN_NvAPI_D3D_Sleep = NvAPI_Status(__cdecl*)(IUnknown* pDev);
using PFN_NvAPI_D3D_GetLatency = NvAPI_Status(__cdecl*)(IUnknown* pDev, NV_LATENCY_RESULT_PARAMS* pParams);

// NvAPI_QueryInterface — the single entry point of nvapi64.dll
using PFN_NvAPI_QueryInterface = void*(__cdecl*)(uint32_t id);

// Well-known interface IDs (from nvapi_interface.h)
constexpr uint32_t NVAPI_ID_D3D_SetSleepMode = 0xAC1CA9E0;
constexpr uint32_t NVAPI_ID_D3D_Sleep = 0x852CD1D2;
constexpr uint32_t NVAPI_ID_D3D_GetLatency = 0x1A587F9C;
