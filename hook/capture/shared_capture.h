/**
 * Shared Capture Interface
 *
 * Defines the zero-copy capture mechanism using DXGI shared resources.
 * Both DX11 and DX12 capture paths use this interface.
 */

#pragma once

#include <d3d11.h>
#include <d3d11_1.h>
#include <d3d12.h>
#include <dxgi1_6.h>
#include <wrl/client.h>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>

using Microsoft::WRL::ComPtr;

// ============================================================================
// Shared Frame Descriptor
// ============================================================================

struct SharedFrameDescriptor {
    HANDLE sharedHandle;  // DXGI shared texture handle
    UINT width;
    UINT height;
    DXGI_FORMAT format;
    UINT64 fenceValue;     // Sync fence value
    UINT64 presentTime;    // QPC timestamp
    UINT frameNumber;      // Monotonic frame counter
    int32_t textureIndex;  // Index of shared texture (0-1)
    bool ready;            // Frame is ready for consumption
};

// ============================================================================
// ISharedCaptureTarget - Interface for capture consumers
// ============================================================================

class ISharedCaptureTarget {
public:
    virtual ~ISharedCaptureTarget() = default;

    // Get the current frame descriptor
    virtual bool GetCurrentFrame(SharedFrameDescriptor* pDesc) = 0;

    // Signal that the frame has been consumed
    virtual void ReleaseFrame(UINT frameNumber) = 0;

    // Check if capture is active
    virtual bool IsActive() const = 0;
};

// ============================================================================
// SharedCaptureD3D11 - Zero-copy capture for D3D11
// ============================================================================

class SharedCaptureD3D11 : public ISharedCaptureTarget {
public:
    SharedCaptureD3D11();
    ~SharedCaptureD3D11() override;

    // Initialize with the device and swapchain
    bool Initialize(ID3D11Device* pDevice, IDXGISwapChain* pSwapChain);

    // Call before Present to capture the frame
    bool CaptureFrame(ID3D11DeviceContext* pContext);

    // ISharedCaptureTarget
    bool GetCurrentFrame(SharedFrameDescriptor* pDesc) override;
    void ReleaseFrame(UINT frameNumber) override;
    bool IsActive() const override {
        return m_Active;
    }

private:
    bool CreateSharedTexture(UINT width, UINT height, DXGI_FORMAT format);

    ComPtr<ID3D11Device> m_pDevice;
    ComPtr<ID3D11Device1> m_pDevice1;
    ComPtr<ID3D11DeviceContext> m_pContext;
    ComPtr<IDXGISwapChain> m_pSwapChain;

    // Double-buffered shared textures for producer/consumer
    ComPtr<ID3D11Texture2D> m_SharedTextures[2];
    HANDLE m_SharedHandles[2];

    // Keyed mutex for synchronization
    ComPtr<IDXGIKeyedMutex> m_KeyedMutexes[2];

    // Query for Present completion
    ComPtr<ID3D11Query> m_PresentQueries[2];

    std::mutex m_Lock;
    SharedFrameDescriptor m_CurrentFrame;
    std::atomic<UINT> m_WriteIndex;
    UINT m_FrameCounter;
    std::atomic<bool> m_Active;
};

// ============================================================================
// SharedCaptureD3D12 - Zero-copy capture for D3D12
// ============================================================================

class SharedCaptureD3D12 : public ISharedCaptureTarget {
public:
    SharedCaptureD3D12();
    ~SharedCaptureD3D12() override;

    // Initialize with the device and swapchain
    bool Initialize(ID3D12Device* pDevice, IDXGISwapChain* pSwapChain);

    // Call before Present to capture the frame using the specified command queue
    bool CaptureFrame(ID3D12CommandQueue* pCommandQueue, UINT backBufferIndex);

    // ISharedCaptureTarget
    bool GetCurrentFrame(SharedFrameDescriptor* pDesc) override;
    void ReleaseFrame(UINT frameNumber) override;
    bool IsActive() const override {
        return m_Active;
    }

    // Get shared handles for IPC sync
    HANDLE GetSharedHandle(int index) const {
        return (index >= 0 && index < 2) ? m_SharedHandles[index] : nullptr;
    }
    HANDLE GetFenceShareHandle() const {
        return m_FenceShareHandle;
    }

    // Reset resources (e.g. on swapchain resize)
    void Reset();

private:
    bool CreateSharedResources(UINT width, UINT height, DXGI_FORMAT format);

    ComPtr<ID3D12Device> m_pDevice;
    ComPtr<IDXGISwapChain3> m_pSwapChain;

    // D3D12 shared resources (can be opened by D3D11 encoder cross-process)
    ComPtr<ID3D12Resource> m_SharedResources[2];
    HANDLE m_SharedHandles[2];

    // Fence for synchronization
    ComPtr<ID3D12Fence> m_Fence;
    HANDLE m_FenceShareHandle;
    HANDLE m_FenceEvent;
    std::atomic<UINT64> m_FenceValue;

    // Command allocator/list for copy operations
    // Double-buffered allocators to allow CPU to record next frame while GPU
    // processes previous
    ComPtr<ID3D12CommandAllocator> m_CommandAllocators[2];
    UINT64 m_FenceValues[2];  // Fence value to wait for before resetting allocator
                              // [i]

    ComPtr<ID3D12GraphicsCommandList> m_CommandList;

    std::mutex m_Lock;
    SharedFrameDescriptor m_CurrentFrame;
    std::atomic<UINT> m_WriteIndex;
    UINT m_FrameCounter;
    std::atomic<bool> m_Active;
};

// ============================================================================
// Global Capture Manager
// ============================================================================

class CaptureManager {
public:
    static CaptureManager& Get();

    // Register a capture target
    void RegisterCaptureTarget(const char* name, ISharedCaptureTarget* target);
    void UnregisterCaptureTarget(const char* name);

    // Get the active capture target
    ISharedCaptureTarget* GetCaptureTarget(const char* name);

    // Signal capture enabled/disabled
    void SetCaptureEnabled(bool enabled);
    bool IsCaptureEnabled() const {
        return m_CaptureEnabled;
    }

private:
    CaptureManager() : m_CaptureEnabled(false) {}

    std::mutex m_Lock;
    std::unordered_map<std::string, ISharedCaptureTarget*> m_Targets;
    std::atomic<bool> m_CaptureEnabled;
};
