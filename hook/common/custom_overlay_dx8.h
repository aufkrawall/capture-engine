/**
 * Custom Overlay - DX8 Backend
 *
 * Renders overlay using Direct3D 8.
 * Uses the fixed-function pipeline for compatibility with legacy titles.
 */

#pragma once

#include "custom_overlay.h"

struct IDirect3DDevice8;
struct IDirect3DTexture8;
struct IDirect3DVertexBuffer8;
struct IDirect3DIndexBuffer8;
struct IDirect3DBaseTexture8;

namespace CustomOverlay {

constexpr size_t DX8_VERTEX_BUFFER_SIZE = 65536 * sizeof(float) * 8;
constexpr size_t DX8_INDEX_BUFFER_SIZE = 131072 * sizeof(uint16_t);

class DX8Backend : public RendererBackend {
public:
    DX8Backend(IDirect3DDevice8* device);
    virtual ~DX8Backend();

    bool Initialize(int fontTextureWidth, int fontTextureHeight, const uint8_t* fontTextureData) override;
    void Shutdown() override;

    void Render(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) override;

private:
    bool ResizeVertexBuffer(size_t requiredBytes);
    bool ResizeIndexBuffer(size_t requiredBytes);

    IDirect3DDevice8* device = nullptr;
    IDirect3DTexture8* fontTexture = nullptr;
    IDirect3DVertexBuffer8* vertexBuffer = nullptr;
    IDirect3DIndexBuffer8* indexBuffer = nullptr;

    size_t vertexBufferSize = 0;
    size_t indexBufferSize = 0;

    IDirect3DBaseTexture8* lastTexture = nullptr;
    bool initialized = false;
};

}  // namespace CustomOverlay
