/**
 * Custom Overlay - DX12 Backend
 * 
 * Renders overlay using Direct3D 12.
 * More complex than DX11 due to explicit resource management.
 */

#pragma once

#include "custom_overlay.h"
#include <d3d12.h>
#include <dxgi1_4.h>
#include <wrl/client.h>
#include <vector>

namespace CustomOverlay {

using Microsoft::WRL::ComPtr;

class DX12Backend : public RendererBackend {
public:
    DX12Backend(ID3D12Device* device, ID3D12CommandQueue* queue, DXGI_FORMAT rtvFormat);
    virtual ~DX12Backend();

    bool Initialize(int fontTextureWidth, int fontTextureHeight,
                   const uint8_t* fontTextureData) override;
    void Shutdown() override;

    void Render(const std::vector<DrawVertex>& vertices,
               const std::vector<uint16_t>& indices,
               const std::vector<DrawCommand>& commands,
               int viewportWidth, int viewportHeight) override;

    // DX12-specific: Set render target before rendering
    void SetRenderTarget(ID3D12GraphicsCommandList* cmdList, 
                        D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle);

private:
    bool CreateRootSignature();
    bool CreatePipelineState();
    bool CreateBuffers();
    bool CreateFontTexture(int width, int height, const uint8_t* data);

    ID3D12Device* device = nullptr;
    ID3D12CommandQueue* commandQueue = nullptr;
    DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

    ComPtr<ID3D12RootSignature> rootSignature;
    ComPtr<ID3D12PipelineState> pipelineState;
    ComPtr<ID3D12PipelineState> pipelineStateSolid;
    ComPtr<ID3D12DescriptorHeap> srvHeap;
    ComPtr<ID3D12Resource> fontTexture;
    ComPtr<ID3D12Resource> vertexBuffer;
    ComPtr<ID3D12Resource> indexBuffer;
    ComPtr<ID3D12Resource> uploadBuffer;  // For texture upload

    void* vertexBufferPtr = nullptr;
    void* indexBufferPtr = nullptr;
    size_t vertexBufferSize = 0;
    size_t indexBufferSize = 0;

    ID3D12GraphicsCommandList* currentCmdList = nullptr;
    D3D12_CPU_DESCRIPTOR_HANDLE currentRTV = {};

    bool initialized = false;
};

} // namespace CustomOverlay
