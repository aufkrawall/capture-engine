/**
 * Custom Overlay - DX11 Backend
 *
 * Renders overlay using Direct3D 11.
 * Creates shaders, textures, and buffers for overlay rendering.
 */

#pragma once

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>
#include "custom_overlay.h"

namespace CustomOverlay {

using Microsoft::WRL::ComPtr;

class DX11Backend : public RendererBackend {
public:
    DX11Backend(ID3D11Device* device, ID3D11DeviceContext* context);
    virtual ~DX11Backend();

    bool Initialize(int fontTextureWidth, int fontTextureHeight, const uint8_t* fontTextureData) override;
    void Shutdown() override;

    void Render(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) override;

private:
    bool CreateShaders();
    bool CreateBuffers();
    bool CreateStates();

    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;

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

    size_t vertexBufferSize = 0;
    size_t indexBufferSize = 0;
    bool initialized = false;
};

}  // namespace CustomOverlay
