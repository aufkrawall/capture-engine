/**
 * Custom Overlay - DX10 Backend
 *
 * Renders overlay using Direct3D 10.
 * Creates shaders, textures, and buffers for overlay rendering.
 */

#pragma once

#include "custom_overlay.h"
#include <d3d10.h>

namespace CustomOverlay {

class DX10Backend : public RendererBackend {
public:
  DX10Backend(ID3D10Device *device);
  virtual ~DX10Backend();

  bool Initialize(int fontTextureWidth, int fontTextureHeight,
                  const uint8_t *fontTextureData) override;
  void Shutdown() override;

  void Render(const std::vector<DrawVertex> &vertices,
              const std::vector<uint16_t> &indices,
              const std::vector<DrawCommand> &commands, int viewportWidth,
              int viewportHeight) override;

private:
  bool CreateShaders();
  bool CreateBuffers();
  bool CreateStates();

  ID3D10Device *device = nullptr;

  ID3D10Texture2D *fontTexture = nullptr;
  ID3D10ShaderResourceView *fontTextureView = nullptr;
  ID3D10Buffer *vertexBuffer = nullptr;
  ID3D10Buffer *indexBuffer = nullptr;
  ID3D10Buffer *constantBuffer = nullptr;
  ID3D10InputLayout *inputLayout = nullptr;
  ID3D10VertexShader *vertexShader = nullptr;
  ID3D10PixelShader *pixelShader = nullptr;
  ID3D10PixelShader *pixelShaderSolid = nullptr;
  ID3D10BlendState *blendState = nullptr;
  ID3D10SamplerState *samplerState = nullptr;
  ID3D10RasterizerState *rasterizerState = nullptr;
  ID3D10DepthStencilState *depthStencilState = nullptr;

  size_t vertexBufferSize = 0;
  size_t indexBufferSize = 0;
  bool initialized = false;
};

} // namespace CustomOverlay
