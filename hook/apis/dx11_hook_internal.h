#pragma once

class D3D11InternalIdentityProbeScope;

struct D3D11ContextVTableOriginals;

struct D3D11SamplerCacheEntry;

struct D3D11StageState;

struct D3D11PerContextState;

class DX11Capture;

#include <atomic>

#include <cmath>

#include <cstdint>

#include <cstdio>

#include <cstring>

#include <limits>

#include <mutex>

#include <shared_mutex>

#include <string>

#include <unordered_map>

#include <unordered_set>

#include <vector>

#include "../../common/frame_timing.h"

#include "../../common/raii_helpers.h"

#include "../common/capture_base.h"

#include "../common/capture_pacing.h"

#include "../common/deferred_release.h"

#include "../common/dll_utils.h"

#include "../common/fg_detection.h"

#include "../common/fps_limiter.h"

#include "../common/graphics_api_identity.h"

#include "../common/hook_common.h"

#include "../common/overlay_adapter.h"

#include "../common/overlay_metrics_publisher.h"

#include "../common/perf_logger.h"

#include "../common/sampler_override_utils.h"

#include "../common/screenshot_hook.h"

#include "../common/system_metrics.h"

#include "../wrappers/dxgi_swapchain_wrap.h"

#include "../wrappers/iat_hook.h"

#include "../wrappers/wrapper_base.h"

#include "dx11_hook.h"

#include "dx12_hook.h"

#include "dxgi_shared.h"

#include "graphics_hook.h"

#include "lod_helper.h"

#include "performance_metrics.h"

// Global Deferred Release Queue for D3D11 resources
// Prevents render thread stalls during resource destruction
extern ce::DeferredReleaseQueue g_DeferredRelease;

#include <d3d10.h>    // For DX10 detection

#include <d3d10_1.h>  // For DX10.1 detection

#include <d3d11.h>

#include <d3d11_1.h>  // For ID3D11DeviceContext1

#include <d3d11_4.h>  // For ID3D11Fence and ID3D11Device5

#include <d3d12.h>    // For ID3D12CommandQueue detection

#include <d3dcompiler.h>

#include <dxgi1_2.h>  // Required for IDXGIResource1 and CreateSharedHandle

#include <dxgi1_4.h>  // For IDXGISwapChain3

#include "../common/input_manager.h"

#include "../wrappers/custom_hook.h"

#include "../wrappers/d3d11_devicecontext_wrap.h"

#include "../wrappers/iat_hook.h"

#include "../wrappers/vtable_hook.h"

#include "../wrappers/wrapper_base.h"

extern thread_local unsigned g_D3D11InternalIdentityProbeDepth;

typedef HRESULT(STDMETHODCALLTYPE* D3D11QueryInterface_t)(IUnknown*, REFIID, void**);

// Re-entrancy guard: prevents mutual recursion with other Present hooks (e.g. Steam overlay)
extern thread_local bool g_InPresentHook;

typedef HRESULT(STDMETHODCALLTYPE* Present_t)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT Flags);

typedef HRESULT(STDMETHODCALLTYPE* ResizeBuffers_t)(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width,
                                                    UINT Height, DXGI_FORMAT NewFormat, UINT SwapChainFlags);

typedef HRESULT(STDMETHODCALLTYPE* Present1_t)(IDXGISwapChain* pSwapChain, UINT SyncInterval, UINT PresentFlags,
                                               const DXGI_PRESENT_PARAMETERS* pPresentParameters);

#ifndef DXGI_PRESENT_ALLOW_TEARING
#define DXGI_PRESENT_ALLOW_TEARING 0x00000200UL
#endif

typedef HRESULT(STDMETHODCALLTYPE* CreateSamplerState_t)(ID3D11Device* pDevice, const D3D11_SAMPLER_DESC* pSamplerDesc,
                                                         ID3D11SamplerState** ppSamplerState);

typedef HRESULT(STDMETHODCALLTYPE* CreatePixelShader11_t)(ID3D11Device* pDevice, const void* pShaderBytecode,
                                                          SIZE_T BytecodeLength, ID3D11ClassLinkage* pClassLinkage,
                                                          ID3D11PixelShader** ppPixelShader);

typedef HRESULT(STDMETHODCALLTYPE* CreateDeferredContext11_t)(ID3D11Device* pDevice, UINT ContextFlags,
                                                              ID3D11DeviceContext** ppDeferredContext);

typedef void(STDMETHODCALLTYPE* SetShaderResources11_t)(ID3D11DeviceContext* pContext, UINT StartSlot, UINT NumViews,
                                                        ID3D11ShaderResourceView* const* ppShaderResourceViews);

typedef void(STDMETHODCALLTYPE* SetSamplers11_t)(ID3D11DeviceContext* pContext, UINT StartSlot, UINT NumSamplers,
                                                 ID3D11SamplerState* const* ppSamplers);

typedef void(STDMETHODCALLTYPE* PSSetShader11_t)(ID3D11DeviceContext* pContext, ID3D11PixelShader* pPixelShader,
                                                 ID3D11ClassInstance* const* ppClassInstances, UINT NumClassInstances);

typedef void(STDMETHODCALLTYPE* DrawIndexed11_t)(ID3D11DeviceContext* pContext, UINT IndexCount,
                                                 UINT StartIndexLocation, INT BaseVertexLocation);

typedef void(STDMETHODCALLTYPE* Draw11_t)(ID3D11DeviceContext* pContext, UINT VertexCount, UINT StartVertexLocation);

typedef void(STDMETHODCALLTYPE* DrawIndexedInstanced11_t)(ID3D11DeviceContext* pContext, UINT IndexCountPerInstance,
                                                          UINT InstanceCount, UINT StartIndexLocation,
                                                          INT BaseVertexLocation, UINT StartInstanceLocation);

typedef void(STDMETHODCALLTYPE* DrawInstanced11_t)(ID3D11DeviceContext* pContext, UINT VertexCountPerInstance,
                                                   UINT InstanceCount, UINT StartVertexLocation,
                                                   UINT StartInstanceLocation);

typedef void(STDMETHODCALLTYPE* DrawAuto11_t)(ID3D11DeviceContext* pContext);

typedef void(STDMETHODCALLTYPE* DrawIndexedInstancedIndirect11_t)(ID3D11DeviceContext* pContext,
                                                                  ID3D11Buffer* pBufferForArgs,
                                                                  UINT AlignedByteOffsetForArgs);

typedef void(STDMETHODCALLTYPE* DrawInstancedIndirect11_t)(ID3D11DeviceContext* pContext, ID3D11Buffer* pBufferForArgs,
                                                           UINT AlignedByteOffsetForArgs);

typedef void(STDMETHODCALLTYPE* ExecuteCommandList11_t)(ID3D11DeviceContext* pContext, ID3D11CommandList* pCommandList,
                                                        BOOL RestoreContextState);

// D3D10 CreateSamplerState hook
typedef HRESULT(STDMETHODCALLTYPE* CreateSamplerState10_t)(ID3D10Device* pDevice,
                                                           const D3D10_SAMPLER_DESC* pSamplerDesc,
                                                           ID3D10SamplerState** ppSamplerState);

#include <algorithm>

#include <atomic>

#include <shared_mutex>

#include <unordered_map>

#include <unordered_set>


#include "dx11_hook_types.h"

#include "dx11_hook_types2.h"

class DX11Capture : public HookCaptureBase {
public:
    std::recursive_mutex captureMutex;
    ID3D11Texture2D* sharedTextures[CAPTURE_TEXTURE_COUNT]{};
    ID3D11Query* copyQueries[CAPTURE_TEXTURE_COUNT]{};  // GPU sync queries
    ID3D11Device* cachedDevice = nullptr;
    ID3D11DeviceContext* cachedContext = nullptr;
    IUnknown* cachedSwapChainIdentity = nullptr;
    bool generationResetPending = false;

    // DX10 capture must stay on the real D3D10 device to avoid invalid
    // cross-device copies from a DX10 swapchain buffer into D3D11-owned
    // textures, which can produce corrupted output.
    ID3D10Device* cachedDevice10 = nullptr;
    ID3D10Texture2D* sharedTextures10[CAPTURE_TEXTURE_COUNT]{};
    ID3D10Query* copyQueries10[CAPTURE_TEXTURE_COUNT]{};
    bool isDX10Mode = false;

    // For DXVK games: the game's D3D11 is DXVK's (Vulkan-backed). Shared handles
    // from DXVK are Vulkan-internal IDs the real encoder D3D11 can't open.
    // Fix: create ring buffer textures in a real system D3D11 device (ownedDevice),
    // then import each into DXVK's device for CopyResource.
    ID3D11Device* ownedDevice = nullptr;
    ID3D11DeviceContext* ownedContext = nullptr;
    bool isDXVKMode = false;
    ID3D11Texture2D* dxvkImportedTextures[CAPTURE_TEXTURE_COUNT]{};  // DXVK-side imports

    // DX11.3 Fence Support
    ID3D11Fence* fence = nullptr;
    ID3D11DeviceContext4* context4 = nullptr;  // Needed for Signal
    bool useFences = false;
    UINT64 fenceValue = 0;
    UINT64 slotFenceValues[CAPTURE_TEXTURE_COUNT]{};

    // Keyed Mutex Support (Proper Fix)
    IDXGIKeyedMutex* keyedMutexes[CAPTURE_TEXTURE_COUNT]{};
    bool useKeyedMutex = false;
    bool sharedTextureHandlesAreNt[CAPTURE_TEXTURE_COUNT]{};

    // sharedTextureHandles are in base class
void Cleanup() override;void RequestGenerationReset(IDXGISwapChain* swapChain);void CreateSharedResources(uint32_t w, uint32_t h, uint32_t fmt) override;

    // Initialize for DX10 games - capture stays on the real D3D10 device and
    // publishes DXGI shared handles that the media-side D3D11 device opens.
bool InitDX10(IDXGISwapChain* swapChain);

    // Detect if the loaded d3d11.dll is DXVK's (not the system one).
    // DXVK games place their d3d11.dll in the game directory, shadowing System32.
bool IsCurrentD3D11DXVK();

    // Create a real system D3D11 device on the GPU identified by a LUID.
    // Used for DXVK games so ring buffer textures carry valid Windows NT handles.
bool CreateSystemD3D11DeviceForLUID(int32_t luidLowPart, int32_t luidHighPart);void Init(ID3D11Device* device, IDXGISwapChain* swapChain);

    // Wait for a specific query to complete (with timeout)
bool WaitForCopy(ID3D11DeviceContext* context, int idx, DWORD timeoutMs = 10);

    // Get the context to use for DX11 capture operations
ID3D11DeviceContext* GetCaptureContext();

    // Capture a frame from the swapchain to shared texture
    // Returns true if frame was captured, false if skipped/dropped
bool CaptureFrame(IDXGISwapChain* swapChain);
};

    // NOLINTNEXTLINE(bugprone-throwing-static-initialization) - static object default construction is non-allocating (members are trivial or empty)
inline DX11Capture dx11_hook_g_DX11Capture;OverlayConfig GetActiveDX11OverlayConfig(SharedMemoryLayout* shm);void CaptureDX11Screenshot(IDXGISwapChain* pSwapChain, SharedMemoryLayout* shm, uint64_t requestId);void CaptureDX10Screenshot(IDXGISwapChain* pSwapChain, SharedMemoryLayout* shm, uint64_t requestId);void CaptureRequestedDX11Screenshot(IDXGISwapChain* pSwapChain, SharedMemoryLayout* shm, uint64_t requestId);void ProcessDX11FrameWithOverlayOrdering(IDXGISwapChain* pSwapChain);
