#pragma once

#include <windows.h>
#include <dxgi1_5.h>  // For IDXGISwapChain4
#include <d3d12.h>
#include <atomic>

// Forward declarations
void EarlyLog(const char* fmt, ...);
void HookLog(const char* fmt, ...);
void DX12_OnSwapchainResizeBegin();

// Callback for overlay drawing - set by dx12_hook.cpp
typedef void (*OverlayDrawCallback)(IDXGISwapChain3* pSwapChain, ID3D12CommandQueue* pQueue);
extern OverlayDrawCallback g_OverlayDrawCallback;

// GUID for identifying our wrapper
// {CE8A33B4-1405-424C-AE88-0D3E9D46C914}
static const GUID IID_OverlaySwapChainWrapper = 
    {0xce8a33b4, 0x1405, 0x424c, {0xae, 0x88, 0x0d, 0x3e, 0x9d, 0x46, 0xc9, 0x14}};

// Streamline Native Interface GUID - BLOCK this to prevent Streamline from unwrapping our wrapper
// {ADEC44E2-61F0-45C3-AD9F-1B37379284FF}
// Per SpecialK research: When FG runtimes (DLSS-G, FSR-G) query this GUID, they're trying to
// detect and unwrap any proxy wrappers. By returning E_NOINTERFACE, we prevent them from
// seeing our wrapper, which avoids crashes during FG processing.
static const GUID IID_StreamlineNativeInterfaceBlock = 
    {0xADEC44E2, 0x61F0, 0x45C3, {0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF}};

// Swapchain wrapper that intercepts Present to draw overlay BEFORE FG processing
class OverlaySwapChainWrapper : public IDXGISwapChain4 {
public:
    OverlaySwapChainWrapper(IDXGISwapChain* pReal, ID3D12CommandQueue* pQueue)
        : m_pReal(pReal), m_pQueue(pQueue), m_refs(1) {
        if (m_pReal) m_pReal->AddRef();
        if (m_pQueue) m_pQueue->AddRef();
        
        // Try to get IDXGISwapChain4 interface
        if (FAILED(pReal->QueryInterface(IID_PPV_ARGS(&m_pReal4)))) {
            m_pReal4 = nullptr;
        }
        if (FAILED(pReal->QueryInterface(IID_PPV_ARGS(&m_pReal3)))) {
            m_pReal3 = nullptr;
        }
        if (FAILED(pReal->QueryInterface(IID_PPV_ARGS(&m_pReal1)))) {
            m_pReal1 = nullptr;
        }
        
        EarlyLog("OverlaySwapChainWrapper: Created wrapper for %p (queue=%p)", pReal, pQueue);
    }
    
    virtual ~OverlaySwapChainWrapper() {
        EarlyLog("OverlaySwapChainWrapper: Destroyed");
        if (m_pReal4) m_pReal4->Release();
        if (m_pReal3) m_pReal3->Release();
        if (m_pReal1) m_pReal1->Release();
        if (m_pQueue) m_pQueue->Release();
        if (m_pReal) m_pReal->Release();
    }

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** ppvObj) override {
        if (!ppvObj) return E_POINTER;
        
        // CRITICAL: Block Streamline from unwrapping our wrapper!
        // Per SpecialK research, returning E_NOINTERFACE prevents FG runtimes (DLSS-G, FSR-G)
        // from detecting our wrapper as a proxy, avoiding crashes during FG processing.
        // This is THE key fix for the FSR FG overlay crash.
        if (riid == IID_StreamlineNativeInterfaceBlock) {
            static bool logOnce = true;
            if (logOnce) {
                EarlyLog("OverlaySwapChainWrapper: Blocking Streamline native interface query (FG compat)");
                logOnce = false;
            }
            *ppvObj = nullptr;
            return E_NOINTERFACE;
        }
        
        // Return our wrapper for swapchain interfaces
        if (riid == IID_OverlaySwapChainWrapper ||
            riid == __uuidof(IUnknown) ||
            riid == __uuidof(IDXGIObject) ||
            riid == __uuidof(IDXGIDeviceSubObject) ||
            riid == __uuidof(IDXGISwapChain)) {
            AddRef();
            *ppvObj = this;
            return S_OK;
        }
        
        if (riid == __uuidof(IDXGISwapChain1) && m_pReal1) {
            AddRef();
            *ppvObj = static_cast<IDXGISwapChain1*>(this);
            return S_OK;
        }

        if (riid == __uuidof(IDXGISwapChain2) && m_pReal3) {
            AddRef();
            *ppvObj = static_cast<IDXGISwapChain2*>(this);
            return S_OK;
        }
        
        if (riid == __uuidof(IDXGISwapChain3) && m_pReal3) {
            AddRef();
            *ppvObj = static_cast<IDXGISwapChain3*>(this);
            return S_OK;
        }
        
        if (riid == __uuidof(IDXGISwapChain4) && m_pReal4) {
            AddRef();
            *ppvObj = static_cast<IDXGISwapChain4*>(this);
            return S_OK;
        }
        
        // Forward other queries to real swapchain
        return m_pReal->QueryInterface(riid, ppvObj);
    }
    
    ULONG STDMETHODCALLTYPE AddRef() override {
        return InterlockedIncrement(&m_refs);
    }
    
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG refs = InterlockedDecrement(&m_refs);
        if (refs == 0) {
            delete this;
        }
        return refs;
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID Name, UINT DataSize, const void* pData) override {
        return m_pReal->SetPrivateData(Name, DataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID Name, const IUnknown* pUnknown) override {
        return m_pReal->SetPrivateDataInterface(Name, pUnknown);
    }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID Name, UINT* pDataSize, void* pData) override {
        return m_pReal->GetPrivateData(Name, pDataSize, pData);
    }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID riid, void** ppParent) override {
        return m_pReal->GetParent(riid, ppParent);
    }

    // IDXGIDeviceSubObject
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID riid, void** ppDevice) override {
        return m_pReal->GetDevice(riid, ppDevice);
    }

    // IDXGISwapChain - THE KEY METHODS
    HRESULT STDMETHODCALLTYPE Present(UINT SyncInterval, UINT Flags) override {
        // Draw overlay BEFORE forwarding to real swapchain (and FG runtime)
        if (g_OverlayDrawCallback && m_pReal3) {
            g_OverlayDrawCallback(m_pReal3, m_pQueue);
        }
        return m_pReal->Present(SyncInterval, Flags);
    }
    
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT Buffer, REFIID riid, void** ppSurface) override {
        return m_pReal->GetBuffer(Buffer, riid, ppSurface);
    }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL Fullscreen, IDXGIOutput* pTarget) override {
        return m_pReal->SetFullscreenState(Fullscreen, pTarget);
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* pFullscreen, IDXGIOutput** ppTarget) override {
        return m_pReal->GetFullscreenState(pFullscreen, ppTarget);
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* pDesc) override {
        return m_pReal->GetDesc(pDesc);
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT BufferCount, UINT Width, UINT Height, 
                                            DXGI_FORMAT NewFormat, UINT SwapChainFlags) override {
        EarlyLog("OverlaySwapChainWrapper: ResizeBuffers %ux%u", Width, Height);
        DX12_OnSwapchainResizeBegin();
        return m_pReal->ResizeBuffers(BufferCount, Width, Height, NewFormat, SwapChainFlags);
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* pNewTargetParameters) override {
        return m_pReal->ResizeTarget(pNewTargetParameters);
    }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** ppOutput) override {
        return m_pReal->GetContainingOutput(ppOutput);
    }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* pStats) override {
        return m_pReal->GetFrameStatistics(pStats);
    }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* pLastPresentCount) override {
        return m_pReal->GetLastPresentCount(pLastPresentCount);
    }

    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* pDesc) override {
        return m_pReal1 ? m_pReal1->GetDesc1(pDesc) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pDesc) override {
        return m_pReal1 ? m_pReal1->GetFullscreenDesc(pDesc) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* pHwnd) override {
        return m_pReal1 ? m_pReal1->GetHwnd(pHwnd) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID refiid, void** ppUnk) override {
        return m_pReal1 ? m_pReal1->GetCoreWindow(refiid, ppUnk) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT SyncInterval, UINT PresentFlags, 
                                       const DXGI_PRESENT_PARAMETERS* pPresentParameters) override {
        // Draw overlay BEFORE forwarding to real swapchain
        if (g_OverlayDrawCallback && m_pReal3) {
            g_OverlayDrawCallback(m_pReal3, m_pQueue);
        }
        return m_pReal1 ? m_pReal1->Present1(SyncInterval, PresentFlags, pPresentParameters) : E_NOTIMPL;
    }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override {
        return m_pReal1 ? m_pReal1->IsTemporaryMonoSupported() : FALSE;
    }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** ppRestrictToOutput) override {
        return m_pReal1 ? m_pReal1->GetRestrictToOutput(ppRestrictToOutput) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* pColor) override {
        return m_pReal1 ? m_pReal1->SetBackgroundColor(pColor) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* pColor) override {
        return m_pReal1 ? m_pReal1->GetBackgroundColor(pColor) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION Rotation) override {
        return m_pReal1 ? m_pReal1->SetRotation(Rotation) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* pRotation) override {
        return m_pReal1 ? m_pReal1->GetRotation(pRotation) : E_NOTIMPL;
    }

    // IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT Width, UINT Height) override {
        return m_pReal3 ? static_cast<IDXGISwapChain2*>(m_pReal3)->SetSourceSize(Width, Height) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* pWidth, UINT* pHeight) override {
        return m_pReal3 ? static_cast<IDXGISwapChain2*>(m_pReal3)->GetSourceSize(pWidth, pHeight) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT MaxLatency) override {
        return m_pReal3 ? static_cast<IDXGISwapChain2*>(m_pReal3)->SetMaximumFrameLatency(MaxLatency) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* pMaxLatency) override {
        return m_pReal3 ? static_cast<IDXGISwapChain2*>(m_pReal3)->GetMaximumFrameLatency(pMaxLatency) : E_NOTIMPL;
    }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override {
        return m_pReal3 ? static_cast<IDXGISwapChain2*>(m_pReal3)->GetFrameLatencyWaitableObject() : nullptr;
    }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* pMatrix) override {
        return m_pReal3 ? static_cast<IDXGISwapChain2*>(m_pReal3)->SetMatrixTransform(pMatrix) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* pMatrix) override {
        return m_pReal3 ? static_cast<IDXGISwapChain2*>(m_pReal3)->GetMatrixTransform(pMatrix) : E_NOTIMPL;
    }

    // IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override {
        return m_pReal3 ? m_pReal3->GetCurrentBackBufferIndex() : 0;
    }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE ColorSpace, UINT* pColorSpaceSupport) override {
        return m_pReal3 ? m_pReal3->CheckColorSpaceSupport(ColorSpace, pColorSpaceSupport) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE ColorSpace) override {
        return m_pReal3 ? m_pReal3->SetColorSpace1(ColorSpace) : E_NOTIMPL;
    }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT BufferCount, UINT Width, UINT Height, DXGI_FORMAT Format,
                                             UINT SwapChainFlags, const UINT* pCreationNodeMask,
                                             IUnknown* const* ppPresentQueue) override {
        EarlyLog("OverlaySwapChainWrapper: ResizeBuffers1 %ux%u", Width, Height);
        DX12_OnSwapchainResizeBegin();
        return m_pReal3 ? m_pReal3->ResizeBuffers1(BufferCount, Width, Height, Format, SwapChainFlags, 
                                                    pCreationNodeMask, ppPresentQueue) : E_NOTIMPL;
    }

    // IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE Type, UINT Size, void* pMetaData) override {
        return m_pReal4 ? m_pReal4->SetHDRMetaData(Type, Size, pMetaData) : E_NOTIMPL;
    }

    // Accessors
    IDXGISwapChain* GetReal() const { return m_pReal; }
    IDXGISwapChain3* GetReal3() const { return m_pReal3; }
    ID3D12CommandQueue* GetQueue() const { return m_pQueue; }

private:
    IDXGISwapChain* m_pReal = nullptr;
    IDXGISwapChain1* m_pReal1 = nullptr;
    IDXGISwapChain3* m_pReal3 = nullptr;
    IDXGISwapChain4* m_pReal4 = nullptr;
    ID3D12CommandQueue* m_pQueue = nullptr;
    volatile LONG m_refs = 1;
};
