/**
 * DXGI Swapchain Wrapper
 *
 * COM proxy class that wraps IDXGISwapChain1-4 to intercept Present calls,
 * manage overlay resources safely, and prevent FG runtimes from unwrapping.
 */

#pragma once

#include <d3d12.h>
#include <dxgi1_6.h>
#include <atomic>
#include <mutex>
#include "../common/hook_common.h"
#include "wrapper_base.h"

// Streamline base interface GUID - blocks NV DLSS/FSR FG from unwrapping
// {ADEC44E2-61F0-45C3-AD9F-1B37379284FF}
static const GUID IID_IStreamlineBaseInterface = 
{ 0xadec44e2, 0x61f0, 0x45c3, { 0xad, 0x9f, 0x1b, 0x37, 0x37, 0x92, 0x84, 0xff } };

// Forward declare overlay drawing function (implemented in dx12_hook.cpp)
extern void DrawOverlayOnSwapchain(IDXGISwapChain* pSwapChain, ID3D12CommandQueue* pQueue);
extern bool IsOverlayEnabled();

/**
 * CWrapDXGISwapChain - Full IDXGISwapChain4 wrapper
 *
 * This is the core wrapper that:
 * 1. Intercepts Present calls to draw overlay
 * 2. Handles ResizeBuffers to safely cleanup/recreate overlay resources
 * 3. Blocks FG runtime unwrap attempts
 * 4. Applies VSync and other overrides
 */
class CWrapDXGISwapChain : public IDXGISwapChain4 {
public:
    // Constructors for different swapchain versions
    CWrapDXGISwapChain(IDXGISwapChain* pReal, IUnknown* pDevice);
    CWrapDXGISwapChain(IDXGISwapChain1* pReal, IUnknown* pDevice);
    virtual ~CWrapDXGISwapChain();

    // Get the real (unwrapped) swapchain - for internal use only
    IDXGISwapChain* GetReal() const { return m_pReal; }

    // ========================================================================
    // IUnknown
    // ========================================================================
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override;
    ULONG STDMETHODCALLTYPE AddRef() override;
    ULONG STDMETHODCALLTYPE Release() override;

    // ========================================================================
    // IDXGIObject
    // ========================================================================
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override;
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override;
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override;
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override;

    // ========================================================================
    // IDXGIDeviceSubObject
    // ========================================================================
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice) override;

    // ========================================================================
    // IDXGISwapChain
    // ========================================================================
    HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) override;
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) override;
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT NewFormat,
                                            UINT SwapChainFlags) override;
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) override;
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput) override;
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override;
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount) override;

    // ========================================================================
    // IDXGISwapChain1
    // ========================================================================
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) override;
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* pHwnd) override;
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void** ppUnk) override;
    HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags,
                                       const DXGI_PRESENT_PARAMETERS* pPresentParameters) override;
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override;
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) override;
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* pColor) override;
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* pColor) override;
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override;
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* pRotation) override;

    // ========================================================================
    // IDXGISwapChain2
    // ========================================================================
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override;
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* pWidth, UINT* pHeight) override;
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override;
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override;
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override;
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) override;
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) override;

    // ========================================================================
    // IDXGISwapChain3
    // ========================================================================
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override;
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace,
                                                     UINT* pColorSpaceSupport) override;
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) override;
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                             UINT SwapChainFlags, const UINT* pCreationNodeMask,
                                             IUnknown* const* ppPresentQueue) override;

    // ========================================================================
    // IDXGISwapChain4
    // ========================================================================
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData) override;

private:
    // The real swapchain we're wrapping
    IDXGISwapChain* m_pReal;

    // Promoted interfaces (cached for efficiency)
    IDXGISwapChain1* m_pReal1;
    IDXGISwapChain2* m_pReal2;
    IDXGISwapChain3* m_pReal3;
    IDXGISwapChain4* m_pReal4;

    // Device reference
    IUnknown* m_pDevice;
    ID3D12CommandQueue* m_pD3D12Queue;  // For DX12 overlay drawing

    // Reference count
    LONG m_RefCount;

    // Atomic reference count for real swapchain lifetime tracking
    std::atomic<int> m_RealSwapchainRefs{0};

    // Window handle
    HWND m_hWnd;

    // Interface version (1-4)
    int m_Version;

    // Resource protection
    std::mutex m_ResourceLock;
    std::atomic<bool> m_OverlayResourcesValid;

    // Cached safe pointer for Present() operation
    // Set under m_ResourceLock before releasing mutex
    IDXGISwapChain* m_pRealCached = nullptr;

    // Overlay state
    bool m_IsD3D12;
    bool m_Promoted;  // Lazy promotion flag
    
    // Flip model detection for FSR FG compatibility
    struct {
        bool active = false;
        bool native = false;
    } m_FlipModel;
    
    // Destruction notification (DXGI 1.4+)
    UINT m_DestructionCookie = 0;
    std::atomic<bool> m_SwapchainDestroyed{false};
    static void WINAPI DestructionCallback(void* pData);
    HRESULT RegisterDestructionCallback();
    
    // Swapchain state tracking
    struct SwapChainState {
        bool isFullscreen = false;
        UINT frameLatency = 2;
        DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
        UINT width = 0;
        UINT height = 0;
    } m_State;
    void DetectSwapChainState();

    // Helper methods
    void EnsurePromoted();  // Lazy promotion - only when needed
    void PromoteInterfaces();
    void CleanupOverlayResources();
    void DrawOverlay();
    bool IsFSRInternalSwapchain();  // FSR FG internal swapchain detection
};
