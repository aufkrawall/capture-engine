/**
 * Custom Overlay - DX10 Backend
 *
 * Renders overlay using Direct3D 10.
 * Creates shaders, textures, and buffers for overlay rendering.
 *
 * Optimizations:
 * - Shader caching to avoid redundant state changes
 * - Minimal state save/restore
 * - RAII with ComPtr for safety
 */

#pragma once

#include <d3d10.h>
#include <wrl/client.h>
#include "custom_overlay.h"
#include "legacy_overlay_cache.h"

namespace CustomOverlay {

using Microsoft::WRL::ComPtr;

// Buffer size constants
constexpr size_t DX10_VERTEX_BUFFER_SIZE = 65536 * sizeof(DrawVertex);
constexpr size_t DX10_INDEX_BUFFER_SIZE = 131072 * sizeof(uint16_t);

class DX10Backend : public RendererBackend {
public:
    DX10Backend(ID3D10Device* device);
    virtual ~DX10Backend();

    bool Initialize(int fontTextureWidth, int fontTextureHeight, const uint8_t* fontTextureData) override;
    void Shutdown() override;
    void OnDrawDataChanged() override {
        geometryUpload.MarkDrawDataChanged();
    }

    void Render(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) override;

private:
    bool CreateShaders();
    bool CreateBuffers();
    bool CreateStates();
    bool ResizeVertexBuffer(size_t requiredBytes);
    bool ResizeIndexBuffer(size_t requiredBytes);

    ID3D10Device* device = nullptr;

    ComPtr<ID3D10Texture2D> fontTexture;
    ComPtr<ID3D10ShaderResourceView> fontTextureView;
    ComPtr<ID3D10Buffer> vertexBuffer;
    ComPtr<ID3D10Buffer> indexBuffer;
    ComPtr<ID3D10Buffer> constantBuffer;
    ComPtr<ID3D10InputLayout> inputLayout;
    ComPtr<ID3D10VertexShader> vertexShader;
    ComPtr<ID3D10PixelShader> pixelShader;
    ComPtr<ID3D10PixelShader> pixelShaderSolid;
    ComPtr<ID3D10BlendState> blendState;
    ComPtr<ID3D10SamplerState> samplerState;
    ComPtr<ID3D10RasterizerState> rasterizerState;
    ComPtr<ID3D10DepthStencilState> depthStencilState;

    size_t vertexBufferSize = 0;
    size_t indexBufferSize = 0;

    // Shader caching
    ID3D10PixelShader* lastPixelShader = nullptr;
    LegacyGeometryUploadState geometryUpload;
    DX10ConstantBufferState constantBufferState;

    bool initialized = false;
};

}  // namespace CustomOverlay
