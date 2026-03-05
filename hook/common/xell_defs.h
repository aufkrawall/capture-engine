#pragma once
// Intel XeLL (Xe Low Latency) minimal type definitions.
// Based on Intel XeSS SDK xell.h and xell_d3d12.h (Intel proprietary license).
//
// XeLL is distributed via libxell.dll (Intel Arc driver).
// All functions are loaded via GetProcAddress at runtime — no import lib needed.
//
// Key API (DX12 only):
//   xellD3D12CreateContext(device, &ctx)  - create context
//   xellSetSleepMode(ctx, &params)        - set minimumIntervalUs + enable mode
//   xellSleep(ctx, frameId)              - blocking sleep (enforce FPS cap)
//   xellDestroyContext(ctx)              - release resources

#include <cstdint>

extern "C" {

typedef enum _xell_result_t {
    XELL_RESULT_SUCCESS                  =    0,
    XELL_RESULT_ERROR_UNSUPPORTED_DEVICE =   -1,
    XELL_RESULT_ERROR_UNSUPPORTED_DRIVER =   -2,
    XELL_RESULT_ERROR_UNINITIALIZED      =   -3,
    XELL_RESULT_ERROR_INVALID_ARGUMENT   =   -4,
    XELL_RESULT_ERROR_DEVICE             =   -6,
    XELL_RESULT_ERROR_NOT_IMPLEMENTED    =   -7,
    XELL_RESULT_ERROR_INVALID_CONTEXT    =   -8,
    XELL_RESULT_ERROR_UNSUPPORTED        =  -10,
    XELL_RESULT_ERROR_UNKNOWN            = -1000,
} xell_result_t;

// Opaque context handle
typedef struct _xell_context_handle_s* xell_context_handle_t;

#pragma pack(push, 8)
typedef struct _xell_sleep_params_t {
    uint32_t minimumIntervalUs;    // FPS cap interval (us); 0 = no cap
    uint32_t bLowLatencyMode  : 1; // Enable latency reduction
    uint32_t bLowLatencyBoost : 1; // Not supported yet
    uint32_t reserved         : 30;
} xell_sleep_params_t;
#pragma pack(pop)

typedef enum _xell_logging_level_t {
    XELL_LOGGING_LEVEL_DEBUG   = 0,
    XELL_LOGGING_LEVEL_INFO    = 1,
    XELL_LOGGING_LEVEL_WARNING = 2,
    XELL_LOGGING_LEVEL_ERROR   = 3,
} xell_logging_level_t;

typedef void (*xell_app_log_callback_t)(const char* message, xell_logging_level_t loggingLevel);

// Function pointer types for runtime DLL loading
typedef xell_result_t(__cdecl* PFN_xellD3D12CreateContext)(void* pDevice,
                                                           xell_context_handle_t* pContext);
typedef xell_result_t(__cdecl* PFN_xellDestroyContext)(xell_context_handle_t context);
typedef xell_result_t(__cdecl* PFN_xellSetSleepMode)(xell_context_handle_t context,
                                                     const xell_sleep_params_t* param);
typedef xell_result_t(__cdecl* PFN_xellSleep)(xell_context_handle_t context, uint32_t frame_id);
typedef xell_result_t(__cdecl* PFN_xellSetLoggingCallback)(xell_context_handle_t context,
                                                           xell_logging_level_t level,
                                                           xell_app_log_callback_t callback);

}  // extern "C"
