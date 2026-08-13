/**
 * Custom Overlay - DX12 Backend
 *
 * Renders overlay using Direct3D 12.
 * More complex than DX11 due to explicit resource management.
 */

#pragma once

#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <atomic>
#include <vector>
#include "custom_overlay.h"
#include "dx12_overlay_policy/upload_slot_guard.h"

namespace CustomOverlay {

using Microsoft::WRL::ComPtr;

enum class DX12RenderProbeMode : int {
    kNone = 0,
    kStateSetupOnly = 1,
};

void SetDX12RenderProbeMode(DX12RenderProbeMode mode);
DX12RenderProbeMode GetDX12RenderProbeMode();

class DX12Backend : public RendererBackend {
public:
    DX12Backend(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT rtvFormat);
    virtual ~DX12Backend();

    bool Initialize(int fontTextureWidth, int fontTextureHeight, const uint8_t* fontTextureData) override;
    void Shutdown() override;

    void Render(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) override;
    bool PreferSolidTextGeometry() const override;

    // Override to detect HDR10/PQ vs scRGB from render target format
    void SetHDRParams(int mode, float nits) override {
        if (mode > 0 && rtvFormat == DXGI_FORMAT_R10G10B10A2_UNORM)
            mode = 2;  // HDR10/PQ
        RendererBackend::SetHDRParams(mode, nits);
    }

    // DX12-specific: Set render target before rendering
    void SetRenderTarget(ID3D12GraphicsCommandList* cmdList, D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle);
    void SetUploadSlotFence(ID3D12Fence* fence, uint64_t guardValue);
    // Force the next Render call to use the upload slot associated with an externally owned backbuffer index.
    // Used by FFX proxy rendering so AMD's buffer-reuse fence and CE's VB/IB slot have identical lifetimes.
    void SetNextUploadSlot(int slot);
    bool PrimeResources(ID3D12GraphicsCommandList* cmdList);
    bool HasPendingResources() const {
        return !fontUploaded.load(std::memory_order_acquire) && uploadBuffer && fontTexture;
    }

private:
    bool CreateRootSignature();
    bool CreatePipelineState();
    bool CreateBuffers();
    bool CreateFontTexture(int width, int height, const uint8_t* data);
    bool UploadFontTextureIfNeeded(ID3D12GraphicsCommandList* cmdList);
    bool ResizeVertexBuffer(int slot, size_t requiredBytes);
    bool ResizeIndexBuffer(int slot, size_t requiredBytes);
    bool WaitForSlotGpuComplete(int slot);

    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* commandQueue = nullptr;
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12PipelineState> pipelineStateTexturedSdr;
    ComPtr<ID3D12PipelineState> pipelineStateSolid;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    ComPtr<ID3D12Resource> fontTexture;
    ComPtr<ID3D12Resource> uploadBuffer;  // For texture upload

    // Per-frame buffer pool — prevents CPU/GPU data race on upload-heap buffers.
    // Pool size matches the command allocator pool in dx12_hook.cpp so fence
    // guarantees that slot N is GPU-idle before the CPU reuses it.
    static constexpr int kFramePoolSize = 16;
    ComPtr<ID3D12Resource> vertexBuffer[kFramePoolSize];
    ComPtr<ID3D12Resource> indexBuffer[kFramePoolSize];
    void* vertexBufferPtr[kFramePoolSize] = {};
    void* indexBufferPtr[kFramePoolSize] = {};
    size_t vertexBufferSize[kFramePoolSize] = {};
    size_t indexBufferSize[kFramePoolSize] = {};
    ce::dx12_overlay_policy::UploadSlotGuardFenceBinding slotGuardBinding;
    uint64_t slotFenceValue[kFramePoolSize] = {};
    uint64_t nextSlotFenceValue = 0;
    std::atomic<int> frameIdx{0};
    std::atomic<int> nextForcedUploadSlot{-1};

    ID3D12GraphicsCommandList* currentCmdList = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE currentRTV = {};

    std::atomic<bool> fontUploaded{false};
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fontTextureFootprint = {};
    D3D12_RESOURCE_DESC fontTextureDesc = {};

    bool initialized = false;
};

}  // namespace CustomOverlay
