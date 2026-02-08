/**
 * Custom Overlay - DX9 Backend
 *
 * Renders overlay using Direct3D 9.
 * Uses fixed-function pipeline for maximum compatibility.
 */

#pragma once

#include <d3d9.h>
#include "custom_overlay.h"

namespace CustomOverlay {

class DX9Backend : public RendererBackend {
public:
    DX9Backend(IDirect3DDevice9* device);
    virtual ~DX9Backend();

    bool Initialize(int fontTextureWidth, int fontTextureHeight, const uint8_t* fontTextureData) override;
    void Shutdown() override;

    void Render(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) override;

private:
    IDirect3DDevice9* device = nullptr;
    IDirect3DTexture9* fontTexture = nullptr;
    IDirect3DVertexBuffer9* vertexBuffer = nullptr;
    IDirect3DIndexBuffer9* indexBuffer = nullptr;
    IDirect3DStateBlock9* stateBlock = nullptr;

    size_t vertexBufferSize = 0;
    size_t indexBufferSize = 0;
    bool initialized = false;
};

}  // namespace CustomOverlay
