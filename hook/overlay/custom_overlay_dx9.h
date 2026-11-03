/**
 * Custom Overlay - DX9 Backend
 *
 * Renders overlay using Direct3D 9.
 * Uses fixed-function pipeline for maximum compatibility.
 *
 * Optimizations:
 * - Texture caching to avoid redundant SetTexture calls
 * - Saves and restores only the states the overlay changes
 */

#pragma once

#include <d3d9.h>
#include "custom_overlay.h"
#include "legacy_overlay_cache.h"

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
    void OnDrawDataChanged() override {
        geometryUpload.MarkDrawDataChanged();
    }
    // Fixed-function MODULATE with an opaque white texel is the diffuse color.
    bool DrawsShapesWithFontTexture() const override {
        return true;
    }

    void Render(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) override;

    // Render into a transparent target so the result carries the overlay's own
    // coverage, giving a premultiplied sprite that can be blended anywhere on
    // the CPU. The DirectDraw route needs that: it has no GPU path into a
    // DirectDraw surface, and doing the blend on the GPU costs a readback -
    // with the CPU/GPU synchronization it implies - on every presentation.
    void SetPreserveAlpha(bool preserve) {
        preserveAlpha = preserve;
    }

private:
    bool ResizeVertexBuffer(size_t requiredBytes);
    bool ResizeIndexBuffer(size_t requiredBytes);

    IDirect3DDevice9* device = nullptr;
    IDirect3DTexture9* fontTexture = nullptr;
    IDirect3DVertexBuffer9* vertexBuffer = nullptr;
    IDirect3DIndexBuffer9* indexBuffer = nullptr;
    // A D3DSBT_ALL block, used only on a pure device (see Render).
    IDirect3DStateBlock9* stateBlock = nullptr;
    bool pureDevice = false;
    bool preserveAlpha = false;

    size_t vertexBufferSize = 0;
    size_t indexBufferSize = 0;

    // Texture caching
    IDirect3DBaseTexture9* lastTexture = nullptr;
    LegacyGeometryUploadState geometryUpload;
    LegacyVertexRing vertexRing;
    LegacyIndexUploadState indexUpload;
    // Where the uploaded frame's vertices start in the vertex buffer.
    UINT baseVertex = 0;

    // What each part of a draw cost on the game's thread, logged every 10 s:
    // the overlay's CPU cost against other overlays is in this line.
    struct SubmitCost {
        int64_t upload = 0;
        int64_t save = 0;
        int64_t draw = 0;
        int64_t restore = 0;
        uint32_t frames = 0;
        uint32_t uploads = 0;
        uint32_t discards = 0;
        uint32_t indexUploads = 0;
        DWORD windowStart = 0;
    };
    void NoteSubmitCost(const LARGE_INTEGER (&marks)[5]);
    SubmitCost submitCost;

    bool initialized = false;
};

}  // namespace CustomOverlay
