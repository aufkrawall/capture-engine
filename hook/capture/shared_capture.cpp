/**
 * Shared Capture Implementation — capture manager and D3D11 path
 *
 * Zero-copy capture using DXGI shared resources. The D3D12 path lives in
 * shared_capture_d3d12.cpp.
 */

/**
 * Shared Capture Implementation
 *
 * Zero-copy capture using DXGI shared resources for both DX11 and DX12.
 */

#include "shared_capture.h"
#include <cwchar>
#include <iterator>
#include <string>
#include <unordered_map>
#include "../../common/capture_base.h"
#include "../apis/dx11_hook.h"
#include "../common/hook_common.h"

// ============================================================================
// CaptureManager Implementation
// ============================================================================

CaptureManager& CaptureManager::Get() {
    // Shared capture targets are themselves process-lifetime statics in several
    // hook modules.  A destructed function-static manager can therefore be
    // reached by a later target destructor during CRT/DLL teardown.  Keep the
    // registry alive until process termination; Windows reclaims it together
    // with the injected process.
    static CaptureManager* const instance = new CaptureManager();
    return *instance;
}

void CaptureManager::RegisterCaptureTarget(const char* name, ISharedCaptureTarget* target) {
    if (!name || !*name || !target)
        return;
    std::lock_guard<std::mutex> lock(m_Lock);
    m_Targets[name] = target;
}

void CaptureManager::UnregisterCaptureTarget(const char* name, ISharedCaptureTarget* target) {
    if (!name || !*name)
        return;
    std::lock_guard<std::mutex> lock(m_Lock);
    auto it = m_Targets.find(name);
    if (it != m_Targets.end() && (!target || it->second == target)) {
        m_Targets.erase(it);
    }
}

ISharedCaptureTarget* CaptureManager::GetCaptureTarget(const char* name) {
    if (!name || !*name)
        return nullptr;
    std::lock_guard<std::mutex> lock(m_Lock);
    auto it = m_Targets.find(name);
    return (it != m_Targets.end()) ? it->second : nullptr;
}

void CaptureManager::SetCaptureEnabled(bool enabled) {
    m_CaptureEnabled = enabled;
}

// ============================================================================
// SharedCaptureD3D11 Implementation
// ============================================================================

SharedCaptureD3D11::SharedCaptureD3D11()
    : m_SharedHandles{nullptr, nullptr},
      m_WriteIndex(0),
      m_FrameCounter(0),
      m_Active(false) {}

SharedCaptureD3D11::~SharedCaptureD3D11() {
    m_Active.store(false, std::memory_order_release);
    CaptureManager::Get().UnregisterCaptureTarget("d3d11", this);

    // CRASH FIX: Skip COM Release() during process exit to avoid driver crashes.
    if (IsProcessTerminating()) {
        m_pDevice.Detach();
        m_pDevice1.Detach();
        m_pContext.Detach();
        m_pSwapChain.Detach();
        for (int i = 0; i < 2; i++) {
            m_SharedTextures[i].Detach();
            m_KeyedMutexes[i].Detach();
            if (m_SharedHandles[i]) {
                CloseHandle(m_SharedHandles[i]);
                m_SharedHandles[i] = nullptr;
            }
        }
        return;
    }

    Reset(true);
}

bool SharedCaptureD3D11::Reset(bool force) {
    std::lock_guard<std::recursive_mutex> stateLock(m_StateLock);
    m_Active.store(false, std::memory_order_release);
    CaptureManager::Get().UnregisterCaptureTarget("d3d11", this);

    {
        std::lock_guard<std::mutex> lock(m_Lock);
        m_CurrentFrame = {};
    }

    SharedMemoryLayout* sharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    const bool hasPublishedGeneration = m_SharedHandles[0] || m_SharedHandles[1];
    if (!force && hasPublishedGeneration && HasOutstandingCaptureFrameLeases(sharedMem)) {
        static std::atomic<int> s_generationLeaseLogCount{0};
        if (s_generationLeaseLogCount.fetch_add(1, std::memory_order_relaxed) < 12) {
            EarlyLog("DX11: SharedCapture deferring resource reset while old frame leases are outstanding");
        }
        return false;
    }

    for (int i = 0; i < 2; ++i) {
        m_KeyedMutexes[i].Reset();
        m_SharedTextures[i].Reset();
        if (m_SharedHandles[i]) {
            CloseHandle(m_SharedHandles[i]);
            m_SharedHandles[i] = nullptr;
        }
    }
    m_pContext.Reset();
    m_pDevice1.Reset();
    m_pSwapChain.Reset();
    m_pDevice.Reset();
    m_Width = 0;
    m_Height = 0;
    m_Format = DXGI_FORMAT_UNKNOWN;
    m_WriteIndex.store(0, std::memory_order_relaxed);
    m_FrameCounter = 0;
    return true;
}

bool SharedCaptureD3D11::Initialize(ID3D11Device* pDevice, IDXGISwapChain* pSwapChain) {
    std::lock_guard<std::recursive_mutex> stateLock(m_StateLock);
    if (!pDevice || !pSwapChain)
        return false;

    if (!Reset())
        return false;

    m_pDevice = pDevice;
    m_pSwapChain = pSwapChain;

    // Get ID3D11Device1 for shared resources
    DX11Hook_BeginInternalIdentityProbe();
    const HRESULT device1Hr = pDevice->QueryInterface(IID_PPV_ARGS(&m_pDevice1));
    DX11Hook_EndInternalIdentityProbe();
    if (FAILED(device1Hr)) {
        // Fallback to non-1 device
        m_pDevice1 = nullptr;
    }

    pDevice->GetImmediateContext(&m_pContext);
    if (!m_pContext) {
        EarlyLog("DX11: SharedCapture - GetImmediateContext returned null");
        Reset();
        return false;
    }

    // Query the actual buffer rather than DXGI_SWAP_CHAIN_DESC. Width/height can
    // be zero in the creation descriptor when DXGI derives them from the HWND.
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        EarlyLog("DX11: SharedCapture - GetBuffer(0) failed hr=0x%08X", hr);
        Reset();
        return false;
    }
    D3D11_TEXTURE2D_DESC backBufferDesc = {};
    backBuffer->GetDesc(&backBufferDesc);
    if (backBufferDesc.Width == 0 || backBufferDesc.Height == 0 || backBufferDesc.Format == DXGI_FORMAT_UNKNOWN) {
        EarlyLog("DX11: SharedCapture - Invalid backbuffer description %ux%u format=%d", backBufferDesc.Width,
                 backBufferDesc.Height, static_cast<int>(backBufferDesc.Format));
        Reset();
        return false;
    }

    // Create shared textures
    if (!CreateSharedTexture(backBufferDesc.Width, backBufferDesc.Height, backBufferDesc.Format)) {
        Reset();
        return false;
    }

    m_Width = backBufferDesc.Width;
    m_Height = backBufferDesc.Height;
    m_Format = backBufferDesc.Format;
    m_Active.store(true, std::memory_order_release);
    CaptureManager::Get().RegisterCaptureTarget("d3d11", this);

    return true;
}

bool SharedCaptureD3D11::CreateSharedTexture(UINT width, UINT height, DXGI_FORMAT format) {
    if (!m_pDevice || width == 0 || height == 0 || format == DXGI_FORMAT_UNKNOWN)
        return false;

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    // CreateSharedHandle requires an NT-handle resource. The keyed mutex is the
    // ownership protocol between producer key 0 and consumer key 1.
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_NTHANDLE | D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX;

    for (int i = 0; i < 2; i++) {
        HRESULT hr = m_pDevice->CreateTexture2D(&desc, nullptr, &m_SharedTextures[i]);
        if (FAILED(hr) || !m_SharedTextures[i]) {
            EarlyLog("DX11: SharedCapture - CreateTexture2D(%d) failed hr=0x%08X", i, hr);
            return false;
        }

        // Get shared handle
        ComPtr<IDXGIResource1> dxgiResource;
        hr = m_SharedTextures[i].As(&dxgiResource);
        if (FAILED(hr) || !dxgiResource) {
            EarlyLog("DX11: SharedCapture - IDXGIResource1 unavailable for texture %d hr=0x%08X", i, hr);
            return false;
        }
        hr = dxgiResource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ | DXGI_SHARED_RESOURCE_WRITE, nullptr,
                                              &m_SharedHandles[i]);
        if (FAILED(hr) || !m_SharedHandles[i]) {
            EarlyLog("DX11: SharedCapture - CreateSharedHandle(%d) failed hr=0x%08X handle=%p", i, hr,
                     m_SharedHandles[i]);
            return false;
        }

        // Get keyed mutex
        hr = m_SharedTextures[i].As(&m_KeyedMutexes[i]);
        if (FAILED(hr) || !m_KeyedMutexes[i]) {
            EarlyLog("DX11: SharedCapture - IDXGIKeyedMutex unavailable for texture %d hr=0x%08X", i, hr);
            return false;
        }
    }

    return true;
}

bool SharedCaptureD3D11::CaptureFrame(ID3D11DeviceContext* pContext) {
    std::unique_lock<std::recursive_mutex> stateLock(m_StateLock, std::try_to_lock);
    if (!stateLock.owns_lock())
        return false;
    if (!m_Active.load(std::memory_order_acquire) || !CaptureManager::Get().IsCaptureEnabled()) {
        return false;
    }

    ID3D11DeviceContext* captureContext = pContext ? pContext : m_pContext.Get();
    if (!captureContext)
        return false;

    ComPtr<ID3D11Device> contextDevice;
    captureContext->GetDevice(&contextDevice);
    if (!contextDevice || contextDevice.Get() != m_pDevice.Get()) {
        static std::atomic<int> s_wrongDeviceLog{0};
        if (s_wrongDeviceLog.fetch_add(1, std::memory_order_relaxed) < 5) {
            EarlyLog("DX11: SharedCapture - Rejected context from another device (contextDevice=%p captureDevice=%p)",
                     contextDevice.Get(), m_pDevice.Get());
        }
        return false;
    }

    // Check if we should throttle capture (encoder is falling behind)
    if (g_IPC && g_IPC->GetSharedMem()) {
        if (g_IPC->GetSharedMem()->throttleCapture.load(std::memory_order_acquire)) {
            return false;
        }
    }

    // Get the back buffer
    ComPtr<ID3D11Texture2D> backBuffer;
    HRESULT hr = m_pSwapChain->GetBuffer(0, IID_PPV_ARGS(&backBuffer));
    if (FAILED(hr) || !backBuffer) {
        return false;
    }

    D3D11_TEXTURE2D_DESC texDesc = {};
    backBuffer->GetDesc(&texDesc);
    if (texDesc.Width != m_Width || texDesc.Height != m_Height || texDesc.Format != m_Format) {
        static std::atomic<int> s_descMismatchLog{0};
        if (s_descMismatchLog.fetch_add(1, std::memory_order_relaxed) < 5) {
            EarlyLog("DX11: SharedCapture - Backbuffer changed without reinitialize (%ux%u/%d -> %ux%u/%d)", m_Width,
                     m_Height, static_cast<int>(m_Format), texDesc.Width, texDesc.Height,
                     static_cast<int>(texDesc.Format));
        }
        m_Active.store(false, std::memory_order_release);
        return false;
    }

    UINT writeIdx = m_WriteIndex.load(std::memory_order_relaxed);
    SharedMemoryLayout* captureSharedMem = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    const int32_t availableSlot = FindAvailableCaptureTextureSlot(captureSharedMem, static_cast<int32_t>(writeIdx), 2);
    if (availableSlot < 0)
        return false;
    writeIdx = static_cast<UINT>(availableSlot);
    m_WriteIndex.store(writeIdx, std::memory_order_relaxed);

    // Acquire keyed mutex for writing
    if (!m_KeyedMutexes[writeIdx])
        return false;
    hr = m_KeyedMutexes[writeIdx]->AcquireSync(0, 0);
    // WAIT_TIMEOUT is a non-negative HRESULT, so FAILED(hr) is insufficient.
    if (hr != S_OK) {
        return false;
    }

    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);

    // Copy the back buffer to our shared texture
    if (texDesc.SampleDesc.Count > 1) {
        captureContext->ResolveSubresource(m_SharedTextures[writeIdx].Get(), 0, backBuffer.Get(), 0, texDesc.Format);
    } else {
        captureContext->CopyResource(m_SharedTextures[writeIdx].Get(), backBuffer.Get());
    }

    // Release keyed mutex with new key
    hr = m_KeyedMutexes[writeIdx]->ReleaseSync(1);
    if (FAILED(hr)) {
        EarlyLog("DX11: SharedCapture - ReleaseSync failed texture=%u hr=0x%08X", writeIdx, hr);
        return false;
    }

    // Update frame descriptor
    {
        std::lock_guard<std::mutex> lock(m_Lock);

        m_CurrentFrame.sharedHandle = m_SharedHandles[writeIdx];
        m_CurrentFrame.width = texDesc.Width;
        m_CurrentFrame.height = texDesc.Height;
        m_CurrentFrame.format = texDesc.Format;
        m_CurrentFrame.fenceValue = 0;
        m_CurrentFrame.presentTime = qpc.QuadPart;
        m_CurrentFrame.frameNumber = ++m_FrameCounter;
        m_CurrentFrame.textureIndex = static_cast<int32_t>(writeIdx);
        m_CurrentFrame.ready = true;
    }

    // Flip write index
    m_WriteIndex.store(1 - writeIdx, std::memory_order_relaxed);

    return true;
}

bool SharedCaptureD3D11::GetCurrentFrame(SharedFrameDescriptor* pDesc) {
    if (!pDesc)
        return false;

    std::lock_guard<std::mutex> lock(m_Lock);
    if (!m_CurrentFrame.ready)
        return false;

    *pDesc = m_CurrentFrame;
    return true;
}

void SharedCaptureD3D11::ReleaseFrame(UINT frameNumber) {
    std::lock_guard<std::mutex> lock(m_Lock);
    if (m_CurrentFrame.frameNumber == frameNumber) {
        m_CurrentFrame.ready = false;
    }
}
