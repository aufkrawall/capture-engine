#pragma once

// The binary interface of Intel's XeSS frame generation (libxess_fg.dll) and XeLL (libxell.dll) that CE observes.
// Only what CE's observe-only detours touch, from the SDK's public interface (XeSS SDK 3.0, inc/xess_fg and inc/xell):
// every export returns a 32-bit result code and takes its arguments in the platform's default calling convention;
// the structures are packed to 8 bytes. CE never allocates these structures and reads only leading fields that
// every SDK generation since 1.0 shares.

#include <d3d12.h>
#include <dxgi1_4.h>

#include <cstdint>

namespace ce::xefg_api {

using Result = int32_t;  // xefg_swapchain_result_t: 0 success, positive warnings, negative errors
constexpr Result kSuccess = 0;

struct OpaqueContext;
using Handle = OpaqueContext*;  // xefg_swapchain_handle_t

enum UiCompositionState : int32_t {
    kUiCompositionDisabled = 0,
    kUiCompositionEnabled = 1,
};

constexpr uint32_t kInitFlagUiTextureNotPremultiplied = 32;  // XEFG_SWAPCHAIN_INIT_FLAG_UITEXTURE_NOT_PREMUL_ALPHA

#pragma pack(push, 8)
// xefg_swapchain_d3d12_init_params_t: its leading fields (the SDK appends to it, never reorders).
struct D3D12InitParamsPrefix {
    IDXGISwapChain* applicationSwapChain;  // InitFromSwapChain only: released by XeFG, refcount must be 1
    uint32_t initFlags;
    uint32_t maxInterpolatedFrames;
};

// xefg_swapchain_present_status_t
struct PresentStatus {
    uint32_t framesPresented;  // frames the last Present queued, generated ones included
    Result frameGenResult;
    uint32_t isFrameGenEnabled;
};
#pragma pack(pop)

using PFN_D3D12CreateContext = Result (*)(ID3D12Device* device, Handle* outHandle);
using PFN_D3D12InitFromSwapChain = Result (*)(Handle handle, ID3D12CommandQueue* queue, const void* initParams);
using PFN_D3D12InitFromSwapChainDesc = Result (*)(Handle handle, HWND hwnd, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                  const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                                  ID3D12CommandQueue* queue, IDXGIFactory2* factory,
                                                  const void* initParams);
using PFN_D3D12GetSwapChainPtr = Result (*)(Handle handle, REFIID riid, void** swapchain);
using PFN_SetEnabled = Result (*)(Handle handle, uint32_t enabled);
using PFN_SetNumInterpolatedFrames = Result (*)(Handle handle, uint32_t frames);
using PFN_SetUiCompositionState = Result (*)(Handle handle, int32_t state);
using PFN_SetPresentId = Result (*)(Handle handle, uint32_t presentId);
using PFN_GetLastPresentStatus = Result (*)(Handle handle, PresentStatus* status);
using PFN_Destroy = Result (*)(Handle handle);
using PFN_SetLatencyReduction = Result (*)(Handle handle, void* xellContext);

}  // namespace ce::xefg_api

namespace ce::xell_api {

using Result = int32_t;  // xell_result_t: 0 success
struct OpaqueContext;
using Handle = OpaqueContext*;  // xell_context_handle_t

enum MarkerType : int32_t {
    kSimulationStart = 0,
    kSimulationEnd = 1,
    kRenderSubmitStart = 2,
    kRenderSubmitEnd = 3,
    kPresentStart = 4,
    kPresentEnd = 5,
    kInputSample = 6,
    kMarkerCount = 7,
};

#pragma pack(push, 8)
// xell_sleep_params_t
struct SleepParams {
    uint32_t minimumIntervalUs;
    uint32_t lowLatencyMode : 1;
    uint32_t lowLatencyBoost : 1;
    uint32_t reserved : 30;
};
#pragma pack(pop)

using PFN_SetSleepMode = Result (*)(Handle context, const SleepParams* params);
using PFN_Sleep = Result (*)(Handle context, uint32_t frameId);
using PFN_AddMarkerData = Result (*)(Handle context, uint32_t frameId, int32_t marker);

}  // namespace ce::xell_api
