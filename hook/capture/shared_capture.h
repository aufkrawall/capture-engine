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
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>
#include "../../common/shared_defs.h"

using Microsoft::WRL::ComPtr;

using SharedCaptureExecuteCommandListsPtr =
    void(STDMETHODCALLTYPE*)(ID3D12CommandQueue*, UINT, ID3D12CommandList* const*);

// ============================================================================
// Shared Frame Descriptor
// ============================================================================

struct SharedFrameDescriptor {
    HANDLE sharedHandle = nullptr;  // DXGI shared texture handle
    UINT width = 0;
    UINT height = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    UINT64 fenceValue = 0;      // Sync fence value
    UINT64 presentTime = 0;     // QPC timestamp
    UINT frameNumber = 0;       // Monotonic frame counter
    int32_t textureIndex = -1;  // Index of shared texture
    bool ready = false;         // Frame is ready for consumption
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
    // Returns false when an old published generation is still leased by the
    // media process. Callers may retry from a later Present without blocking.
    bool Reset(bool force = false);

    ComPtr<ID3D11Device> m_pDevice;
    ComPtr<ID3D11Device1> m_pDevice1;
    ComPtr<ID3D11DeviceContext> m_pContext;
    ComPtr<IDXGISwapChain> m_pSwapChain;

    // Double-buffered shared textures for producer/consumer
    ComPtr<ID3D11Texture2D> m_SharedTextures[2];
    HANDLE m_SharedHandles[2];
    UINT m_Width = 0;
    UINT m_Height = 0;
    DXGI_FORMAT m_Format = DXGI_FORMAT_UNKNOWN;

    // Keyed mutex for synchronization
    ComPtr<IDXGIKeyedMutex> m_KeyedMutexes[2];

    mutable std::recursive_mutex m_StateLock;
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
    static constexpr UINT kSharedTextureCount = SHARED_TEXTURE_SLOT_COUNT;

    SharedCaptureD3D12() noexcept;
    ~SharedCaptureD3D12() override;

    // Initialize with the device and swapchain
    bool Initialize(ID3D12Device* pDevice, IDXGISwapChain* pSwapChain);

    // Call before Present to capture the frame using the specified command queue
    bool CaptureFrame(ID3D12CommandQueue* pCommandQueue, UINT backBufferIndex, int64_t timestampQpc = 0,
                      SharedCaptureExecuteCommandListsPtr executeCommandLists = nullptr);

    // Capture state is tied to one device/swapchain generation. This prevents a
    // preserved overlay backend from accidentally capturing an obsolete swapchain.
    bool IsInitializedFor(ID3D12Device* pDevice, IDXGISwapChain* pSwapChain) const;

    // ISharedCaptureTarget
    bool GetCurrentFrame(SharedFrameDescriptor* pDesc) override;
    void ReleaseFrame(UINT frameNumber) override;
    bool IsActive() const override {
        return m_Active;
    }

    // Get shared handles for IPC sync
    HANDLE GetSharedHandle(int index) const {
        std::lock_guard<std::recursive_mutex> stateLock(m_StateLock);
        return (index >= 0 && index < static_cast<int>(kSharedTextureCount)) ? m_SharedHandles[index] : nullptr;
    }
    HANDLE GetFenceShareHandle() const {
        std::lock_guard<std::recursive_mutex> stateLock(m_StateLock);
        return m_FenceShareHandle;
    }

    // Reset resources (e.g. on swapchain resize)
    // Returns false when an old published generation is still leased by the
    // media process. Callers may retry from a later Present without blocking.
    bool Reset(bool force = false);

private:
    static constexpr size_t kMaxRetiredGenerations = 4;

    struct RetiredGeneration {
        ComPtr<ID3D12Device> device;
        ComPtr<IDXGISwapChain3> swapChain;
        ComPtr<IUnknown> swapChainIdentity;
        ComPtr<ID3D12Fence> fence;
        ComPtr<ID3D12GraphicsCommandList> commandList;
        std::array<ComPtr<ID3D12CommandAllocator>, kSharedTextureCount> commandAllocators;
        std::array<ComPtr<ID3D12Resource>, kSharedTextureCount> sharedResources;
        UINT64 completionFenceValue = 0;
    };

    bool CreateSharedResources(UINT width, UINT height, DXGI_FORMAT format);
    void ReapRetiredGenerations();
    void AbandonRetiredGenerations();

    ComPtr<ID3D12Device> m_pDevice;
    ComPtr<IDXGISwapChain3> m_pSwapChain;
    ComPtr<IUnknown> m_pSwapChainIdentity;

    // Multi-buffered D3D12 shared resources opened by the D3D11 encoder.
    // A deeper ring avoids source-side capture starvation when the GPU is saturated.
    ComPtr<ID3D12Resource> m_SharedResources[kSharedTextureCount];
    HANDLE m_SharedHandles[kSharedTextureCount];

    // Fence for synchronization
    ComPtr<ID3D12Fence> m_Fence;
    HANDLE m_FenceShareHandle;
    std::atomic<UINT64> m_FenceValue;

    // Per-slot allocators let the hook keep multiple capture copies in flight without
    // stalling on the two-slot producer ring under heavy GPU load.
    ComPtr<ID3D12CommandAllocator> m_CommandAllocators[kSharedTextureCount];
    UINT64 m_FenceValues[kSharedTextureCount];

    ComPtr<ID3D12GraphicsCommandList> m_CommandList;

    mutable std::recursive_mutex m_StateLock;
    std::mutex m_Lock;
    SharedFrameDescriptor m_CurrentFrame;
    std::atomic<UINT> m_WriteIndex;
    UINT m_FrameCounter;
    std::atomic<bool> m_Active;
    std::atomic<bool> m_AbandonResourcesOnReset{false};
    std::vector<RetiredGeneration> m_RetiredGenerations;
};

// ============================================================================
// Global Capture Manager
// ============================================================================

class CaptureManager {
public:
    static CaptureManager& Get();

    // Register a capture target
    void RegisterCaptureTarget(const char* name, ISharedCaptureTarget* target);
    void UnregisterCaptureTarget(const char* name, ISharedCaptureTarget* target = nullptr);

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
