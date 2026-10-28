/**
 * Shared Capture Interface
 *
 * Defines the zero-copy capture mechanism using DXGI shared resources for the
 * D3D12 inject path. D3D11 and the legacy APIs publish through their own
 * producers (hook/d3d11/dx11_hook_capture_frame.cpp and friends).
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
#include "common/ipc/shared_defs.h"
#include "common/capture/capture_base.h"
#include "capture_completion_signal.h"
#include "capture_copy_gpu_timing.h"
#include "capture_swapchain_binding.h"
#include "inline_capture_completion.h"

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
// SharedCaptureD3D12 - Zero-copy capture for D3D12
// ============================================================================

class SharedCaptureD3D12 : public ISharedCaptureTarget {
public:
    static constexpr UINT kSharedTextureCount = SHARED_TEXTURE_SLOT_COUNT;

    SharedCaptureD3D12() noexcept;
    ~SharedCaptureD3D12() override;

    // Initialize with the device and swapchain. The swapchain is bound by
    // identity only; no reference on it outlives the call.
    bool Initialize(ID3D12Device* pDevice, IDXGISwapChain* pSwapChain, bool embeddedCopy = false);

    // Call before Present to capture the frame using the specified command
    // queue. `pSwapChain` is the chain being presented: the capture copies from
    // it only while it is the one this generation was initialized for.
    // kAfterPresent holds the copy's completion Signal for CompletePresentedCopy()
    // (capture_completion_signal.h); an embedded copy never queues a Signal.
    bool CaptureFrame(ID3D12CommandQueue* pCommandQueue, IDXGISwapChain* pSwapChain, UINT backBufferIndex,
                      int64_t timestampQpc = 0, SharedCaptureExecuteCommandListsPtr executeCommandLists = nullptr,
                      CaptureCompletionSignal completionSignal = CaptureCompletionSignal::kImmediate);

    // At every post-Present boundary: queues the completion Signal CaptureFrame
    // held for the Present just forwarded. Try-only; a Signal it cannot reach
    // goes out before the next copy or reset instead.
    void CompletePresentedCopy();

    // Copy-fence progress for the Present-stall attribution. Try-only: invalid
    // while another thread holds the capture state.
    CaptureCopyProgress SampleCopyProgress() const;
    // Present thread, after the game's down-call: pairs it with the copy submitted
    // since the previous call (nullptr: the call followed no copy) and hands over
    // copies whose GPU timestamps are readable (capture_copy_gpu_timing.h).
    // Try-only; returns the count written.
    size_t NotePresentDownCall(const CapturePresentDownCall* present, CaptureCopyGpuSample* out, size_t capacity);

    // Capture state is tied to one device/swapchain generation. This prevents a
    // preserved overlay backend from accidentally capturing an obsolete swapchain.
    bool IsInitializedFor(ID3D12Device* pDevice, IDXGISwapChain* pSwapChain, bool embeddedCopy = false) const;

    // Try-only completion service at existing Present boundaries, also after recording stops.
    void RefreshEmbeddedCopies();
    ComPtr<ID3D12CommandQueue> AcquireBoundCopyQueue(IDXGISwapChain* swapChain) const;

    // Diagnostics for a swapchain resize: whether this capture generation
    // copies from `pSwapChain` and how recently. Never blocks the resizing
    // thread - a capture holding the state lock reports `busy` instead.
    struct SwapChainBinding {
        bool busy = false;
        bool active = false;
        bool targetsSwapChain = false;
        UINT framesCaptured = 0;
        int64_t lastCaptureQpc = 0;
    };
    SwapChainBinding DescribeSwapChainBinding(IDXGISwapChain* pSwapChain);

    // Called before a resize of `pSwapChain` is forwarded: nothing of this
    // capture may still be tied to the old back buffers when DXGI counts their
    // references. Talos Reawakened died of DXGI_ERROR_INVALID_CALL with three
    // extra references on every back buffer, only while this capture copied
    // from the chain (logs/20260926_090625). Waits for this capture's own
    // copies, then releases the generation; if media still leases published
    // frames, the textures stay and only the command list and allocators go and
    // the swapchain binding is cleared. The next capture re-initializes at the
    // new size.
    struct ResizeRelease {
        bool targeted = false;
        bool waitedForCopies = false;
        bool waitTimedOut = false;
        bool texturesKept = false;
        UINT64 pendingFenceValue = 0;
    };
    ResizeRelease ReleaseForSwapChainResize(IDXGISwapChain* pSwapChain);

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

    // Identity address of the bound swapchain, for diagnostics only (never an
    // object: the capture holds no reference on it). Never blocks; nullptr
    // when unbound or while a capture holds the state lock.
    const void* PeekBoundSwapChainKey() const {
        std::unique_lock<std::recursive_mutex> stateLock(m_StateLock, std::try_to_lock);
        return stateLock.owns_lock() ? m_SwapChainBinding.Key() : nullptr;
    }

    // Reset resources (e.g. on swapchain resize)
    // Returns false when an old published generation is still leased by the
    // media process. Callers may retry from a later Present without blocking.
    bool Reset(bool force = false);

private:
    static constexpr size_t kMaxRetiredGenerations = 4;

    struct RetiredGeneration {
        ComPtr<ID3D12Device> device;
        ComPtr<ID3D12Fence> fence;
        ComPtr<ID3D12GraphicsCommandList> commandList;
        std::array<ComPtr<ID3D12CommandAllocator>, kSharedTextureCount> commandAllocators;
        std::array<ComPtr<ID3D12Resource>, kSharedTextureCount> sharedResources;
        UINT64 completionFenceValue = 0;
        std::unique_ptr<InlineCaptureCompletion<kSharedTextureCount>> inlineCompletion;
        CaptureCopyGpuTimer::Resources copyGpuTiming;
    };

    bool CreateSharedResources(UINT width, UINT height, DXGI_FORMAT format);
    void ReapRetiredGenerations();
    void AbandonRetiredGenerations();
    // Requires m_StateLock. `site` names the boundary for the log; anywhere but
    // after the Present it is a late release. False when the Signal failed.
    bool ReleaseHeldCompletionSignal(const char* site, bool afterPresent);

    ComPtr<ID3D12Device> m_pDevice;
    // No COM reference: holding one pins the chain past the game's release and
    // DXGI denies the next swapchain on the HWND (capture_swapchain_binding.h).
    ce::capture::SwapChainIdentityBinding m_SwapChainBinding;

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
    HeldCompletionSignal m_HeldSignal;         // guarded by m_StateLock
    uint64_t m_LateHeldSignalReleases = 0;     // guarded by m_StateLock
    CaptureCopyGpuTimer m_CopyGpuTimer;        // guarded by m_StateLock; off for embedded copies
    std::unique_ptr<InlineCaptureCompletion<kSharedTextureCount>> m_InlineCompletion;
    ComPtr<ID3D12CommandQueue> m_EmbeddedQueue;
    ComPtr<ID3D12CommandQueue> m_LastCopyQueue;

    mutable std::recursive_mutex m_StateLock;
    std::mutex m_Lock;
    SharedFrameDescriptor m_CurrentFrame;
    std::atomic<UINT> m_WriteIndex;
    UINT m_FrameCounter;
    std::atomic<bool> m_Active;
    std::atomic<bool> m_AbandonResourcesOnReset{false};
    std::vector<RetiredGeneration> m_RetiredGenerations;
    CaptureLeaseDeferralEpisode m_LeaseDeferral;  // guarded by m_StateLock
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
