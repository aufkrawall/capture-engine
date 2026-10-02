/**
 * Headless overlay backend for the DirectDraw/DX6/DX7 CPU composite.
 *
 * The shared Renderer builds the same draw list here as it does for every GPU
 * backend; overlay_cpu_raster turns that list into pixels. No device, no
 * swapchain and no GPU draw is needed, so the DirectDraw route no longer stands
 * up a D3D9Ex helper device whose only purpose was the GPU blend - and the
 * per-presentation GetRenderTargetData readback that came with it.
 */

#pragma once

#include "custom_overlay.h"

namespace CustomOverlay {

class CpuRasterBackend : public RendererBackend {
public:
    bool Initialize(int, int, const uint8_t*) override {
        return true;
    }
    void Shutdown() override {}
    void Render(const std::vector<DrawVertex>&, const std::vector<uint16_t>&, const std::vector<DrawCommand>&, int,
                int) override {}
};

}  // namespace CustomOverlay