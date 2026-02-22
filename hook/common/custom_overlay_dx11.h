/**
 * Custom Overlay - DX11 Backend
 *
 * Renders overlay using Direct3D 11.
 * Creates shaders, textures, and buffers for overlay rendering.
 * 
 * Optimizations:
 * - Persistent buffer mapping (ring buffer with NO_OVERWRITE)
 * - Shader/PSO caching to avoid redundant state changes
 * - Minimal state save/restore
 */

#pragma once

#include "custom_overlay.h"
#include <d3d11.h>
#include <wrl/client.h>

namespace CustomOverlay {

using Microsoft::WRL::ComPtr;

// Ring buffer constants
constexpr size_t DX11_VERTEX_BUFFER_SIZE = 65536 * sizeof(DrawVertex);
constexpr size_t DX11_INDEX_BUFFER_SIZE = 131072 * sizeof(uint16_t);
constexpr size_t DX11_RING_BUFFER_FRAMES = 3;

class DX11Backend : public RendererBackend {
public:
  DX11Backend(ID3D11Device *device, ID3D11DeviceContext *context);
  virtual ~DX11Backend();

  bool Initialize(int fontTextureWidth, int fontTextureHeight,
                  const uint8_t *fontTextureData) override;
  void Shutdown() override;

  void SetSkipRelease(bool skip) { skipDeviceRelease = skip; }

  void Render(const std::vector<DrawVertex> &vertices,
              const std::vector<uint16_t> &indices,
              const std::vector<DrawCommand> &commands, int viewportWidth,
              int viewportHeight) override;

private:
  bool CreateShaders();
  bool CreateBuffers();
  bool CreateStates();
  bool ResizeVertexBuffer(size_t requiredBytes);
  bool ResizeIndexBuffer(size_t requiredBytes);

  ID3D11Device *device = nullptr;
  ID3D11DeviceContext *context = nullptr;

  ComPtr<ID3D11VertexShader> vertexShader;
  ComPtr<ID3D11PixelShader> pixelShader;
  ComPtr<ID3D11PixelShader> pixelShaderSolid;
  ComPtr<ID3D11InputLayout> inputLayout;
  ComPtr<ID3D11Buffer> vertexBuffer;
  ComPtr<ID3D11Buffer> indexBuffer;
  ComPtr<ID3D11Buffer> constantBuffer;
  ComPtr<ID3D11Texture2D> fontTexture;
  ComPtr<ID3D11ShaderResourceView> fontTextureSRV;
  ComPtr<ID3D11SamplerState> sampler;
  ComPtr<ID3D11BlendState> blendState;
  ComPtr<ID3D11RasterizerState> rasterState;
  ComPtr<ID3D11DepthStencilState> depthState;

  // Persistent mapped buffer pointers
  void *vertexBufferPtr = nullptr;
  void *indexBufferPtr = nullptr;
  size_t vertexBufferSize = 0;
  size_t indexBufferSize = 0;

  // Shader caching to avoid redundant binds
  ID3D11PixelShader *lastPixelShader = nullptr;
  bool lastUseTexture = false;

  // Ring buffer offset tracking
  size_t vertexBufferOffset = 0;
  size_t indexBufferOffset = 0;
  UINT64 frameCounter = 0;

  bool initialized = false;
  bool skipDeviceRelease =
      false; // When true, Shutdown won't release device refs
};

} // namespace CustomOverlay
