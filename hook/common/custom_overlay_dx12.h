/**
 * Custom Overlay - DX12 Backend
 *
 * Renders overlay using Direct3D 12.
 * More complex than DX11 due to explicit resource management.
 */

#pragma once

#include "custom_overlay.h"
#include <atomic>
#include <d3d12.h>
#include <dxgi1_4.h>
#include <vector>
#include <wrl/client.h>

namespace CustomOverlay {

using Microsoft::WRL::ComPtr;

class DX12Backend : public RendererBackend {
public:
  DX12Backend(ID3D12Device *device, ID3D12CommandQueue *queue,
              DXGI_FORMAT rtvFormat);
  virtual ~DX12Backend();

  bool Initialize(int fontTextureWidth, int fontTextureHeight,
                  const uint8_t *fontTextureData) override;
  void Shutdown() override;

  void Render(const std::vector<DrawVertex> &vertices,
              const std::vector<uint16_t> &indices,
              const std::vector<DrawCommand> &commands, int viewportWidth,
              int viewportHeight) override;

  // Override to detect HDR10/PQ vs scRGB from render target format
  void SetHDRParams(int mode, float nits) override {
    if (mode > 0 && rtvFormat == DXGI_FORMAT_R10G10B10A2_UNORM)
      mode = 2; // HDR10/PQ
    RendererBackend::SetHDRParams(mode, nits);
  }

  // DX12-specific: Set render target before rendering
  void SetRenderTarget(ID3D12GraphicsCommandList *cmdList,
                       D3D12_CPU_DESCRIPTOR_HANDLE rtvHandle);

private:
  bool CreateRootSignature();
  bool CreatePipelineState();
  bool CreateBuffers();
  bool CreateFontTexture(int width, int height, const uint8_t *data);
  bool ResizeVertexBuffer(int slot, size_t requiredBytes);
  bool ResizeIndexBuffer(int slot, size_t requiredBytes);

  ID3D12Device *device = nullptr;
  ID3D12CommandQueue *commandQueue = nullptr;
  DXGI_FORMAT rtvFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

  ComPtr<ID3D12RootSignature> rootSignature;
  ComPtr<ID3D12PipelineState> pipelineState;
  ComPtr<ID3D12PipelineState> pipelineStateSolid;
  ComPtr<ID3D12DescriptorHeap> srvHeap;
  ComPtr<ID3D12Resource> fontTexture;
  ComPtr<ID3D12Resource> uploadBuffer; // For texture upload

  // Per-frame buffer pool — prevents CPU/GPU data race on upload-heap buffers.
  // Pool size matches the command allocator pool in dx12_hook.cpp so fence
  // guarantees that slot N is GPU-idle before the CPU reuses it.
  static constexpr int kFramePoolSize = 16;
  ComPtr<ID3D12Resource> vertexBuffer[kFramePoolSize];
  ComPtr<ID3D12Resource> indexBuffer[kFramePoolSize];
  void *vertexBufferPtr[kFramePoolSize] = {};
  void *indexBufferPtr[kFramePoolSize] = {};
  size_t vertexBufferSize[kFramePoolSize] = {};
  size_t indexBufferSize[kFramePoolSize] = {};
  std::atomic<int> frameIdx{0};

  ID3D12GraphicsCommandList *currentCmdList = nullptr;
  D3D12_CPU_DESCRIPTOR_HANDLE currentRTV = {};

  std::atomic<bool> fontUploaded{false};
  D3D12_PLACED_SUBRESOURCE_FOOTPRINT fontTextureFootprint = {};
  D3D12_RESOURCE_DESC fontTextureDesc = {};

  bool initialized = false;
};

} // namespace CustomOverlay
