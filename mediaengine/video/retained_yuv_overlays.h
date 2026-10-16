#pragma once

#include <d3d11.h>
#include <atomic>
#include <cstdint>

#include "yuv_overlay_patch.h"

// Encoder-side state for retained NV12/P010 input (video_encoder_retained_yuv.cpp):
// the overlay patcher and the two small premultiplied layer textures the
// cursor and face-camera renderers draw into before the patch.
struct RetainedYuvOverlayLayer {
    ID3D11Texture2D* texture = nullptr;
    ID3D11RenderTargetView* renderTarget = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    UINT width = 0;
    UINT height = 0;
    void Release();
};

struct RetainedYuvOverlays {
    YuvOverlayPatcher patcher;
    RetainedYuvOverlayLayer camera;
    RetainedYuvOverlayLayer cursor;
    // Precision of the RGB surface overlays used to blend into (route query).
    std::atomic<uint32_t> rgbLevels{1023u};
    uint64_t frames = 0;
    uint64_t patchedFrames = 0;
    uint64_t patchFailures = 0;
    uint64_t lastSummaryTick = 0;
    bool firstFrameLogged = false;

    void Release();
};
