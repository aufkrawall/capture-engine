#pragma once
// Minimal NvAPI Reflex type definitions for FPS limiter integration.
// Derived from the public NvAPI specification (nvapi_interface.h IDs).
// We only need the types required to intercept SetSleepMode and read back
// the minimumIntervalUs field — no full NvAPI SDK dependency.

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

// Function pointer types for the Reflex APIs we intercept
using PFN_NvAPI_D3D_SetSleepMode = NvAPI_Status(__cdecl*)(IUnknown* pDev, NV_SET_SLEEP_MODE_PARAMS* pParams);
using PFN_NvAPI_D3D_Sleep = NvAPI_Status(__cdecl*)(IUnknown* pDev);

// NvAPI_QueryInterface — the single entry point of nvapi64.dll
using PFN_NvAPI_QueryInterface = void*(__cdecl*)(uint32_t id);

// Well-known interface IDs (from nvapi_interface.h)
constexpr uint32_t NVAPI_ID_D3D_SetSleepMode = 0xAC1CA9E0;
constexpr uint32_t NVAPI_ID_D3D_Sleep = 0x852CD1D2;
