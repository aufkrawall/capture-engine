/**
 * Custom Overlay - DX9 Backend
 *
 * Renders overlay using Direct3D 9.
 * Uses fixed-function pipeline for maximum compatibility.
 *
 * Optimizations:
 * - Texture caching to avoid redundant SetTexture calls
 * - Minimal state block usage
 */

#pragma once

#include <d3d9.h>
#include "custom_overlay.h"

namespace CustomOverlay {

// Buffer size constants
constexpr size_t DX9_VERTEX_BUFFER_SIZE = 65536 * sizeof(float) * 8;  // DX9Vertex is 8 floats
constexpr size_t DX9_INDEX_BUFFER_SIZE = 131072 * sizeof(uint16_t);

class DX9Backend : public RendererBackend {
public:
    DX9Backend(IDirect3DDevice9* device);
    virtual ~DX9Backend();

    bool Initialize(int fontTextureWidth, int fontTextureHeight, const uint8_t* fontTextureData) override;
    void Shutdown() override;

    void Render(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) override;

private:
    bool ResizeVertexBuffer(size_t requiredBytes);
    bool ResizeIndexBuffer(size_t requiredBytes);

    IDirect3DDevice9* device = nullptr;
    IDirect3DTexture9* fontTexture = nullptr;
    IDirect3DVertexBuffer9* vertexBuffer = nullptr;
    IDirect3DIndexBuffer9* indexBuffer = nullptr;

    size_t vertexBufferSize = 0;
    size_t indexBufferSize = 0;

    // Texture caching
    IDirect3DBaseTexture9* lastTexture = nullptr;

    bool initialized = false;
};

}  // namespace CustomOverlay
