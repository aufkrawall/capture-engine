/**
 * Custom Overlay - Direct3D 7 Backend
 *
 * Draws the overlay with the game's own IDirect3DDevice7, into the surface the
 * game is about to present.
 *
 * The DirectDraw compatibility route composites through a private D3D9Ex device
 * instead, which costs a CPU round trip per present: the frame's pixels up, the
 * composite back down, and a GetRenderTargetData in between that blocks the
 * render thread until the GPU has caught up. That readback is a hard CPU/GPU
 * serialization point on the game's own present path. A DX6/DX7 title already
 * owns a device that can draw transformed, alpha-blended, textured triangles -
 * which is exactly what the overlay's draw list is - so nothing has to leave
 * the GPU at all.
 *
 * The legacy Direct3D headers (`d3d.h` / `d3dtypes.h`) redefine enumerators and
 * typedefs that `d3d9.h` also defines, so they cannot appear in any header a
 * DX9-or-newer translation unit includes. Every legacy type therefore stays
 * inside the implementation and this interface is expressed in `void*`.
 */

#pragma once

#include <cstdint>
#include <vector>

#include "custom_overlay.h"

namespace CustomOverlay {

class D3D7Backend : public RendererBackend {
public:
    // device: IDirect3DDevice7*. A reference is taken for the backend lifetime.
    explicit D3D7Backend(void* device);
    ~D3D7Backend() override;

    bool Initialize(int fontTextureWidth, int fontTextureHeight, const uint8_t* fontTextureData) override;
    void Shutdown() override;

    void Render(const std::vector<DrawVertex>& vertices, const std::vector<uint16_t>& indices,
                const std::vector<DrawCommand>& commands, int viewportWidth, int viewportHeight) override;

    // IDirect3DDevice7* the backend is bound to, for identity checks against a
    // device the application may have recreated.
    void* GetDevice() const {
        return device;
    }

private:
    bool CreateFontSurface(int width, int height, const uint8_t* pixels);
    void ApplyStageMode(bool useTexture);

    void* device = nullptr;       // IDirect3DDevice7*
    void* directDraw = nullptr;   // IDirectDraw7*
    void* fontSurface = nullptr;  // IDirectDrawSurface7*

    // Transformed-and-lit vertices are handed to DrawIndexedPrimitive straight
    // from this scratch buffer; D3D7 takes user memory, so there is no vertex
    // or index buffer to manage or to stall on.
    std::vector<uint8_t> scratchVertices;

    uint32_t stateBlock = 0;
    bool stateBlockUsable = true;
    bool lastUseTexture = false;
    bool initialized = false;
};

}  // namespace CustomOverlay
