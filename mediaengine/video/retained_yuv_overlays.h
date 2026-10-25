#pragma once

#include <d3d11.h>
#include <atomic>
#include <cstdint>

#include "packed_p010_unpacker.h"
#include "yuv_overlay_patch.h"

// Encoder-side state for retained NV12/P010/packed P010 input
// (video_encoder_retained_yuv.cpp): the packed P010 unpacker, the overlay
// patcher and the two small premultiplied layer textures the cursor and
// face-camera renderers draw into before the patch.
struct RetainedYuvOverlayLayer {
    ID3D11Texture2D* texture = nullptr;
    ID3D11RenderTargetView* renderTarget = nullptr;
    ID3D11ShaderResourceView* view = nullptr;
    UINT width = 0;
    UINT height = 0;
    // Grows the kRetainedYuvOverlayLayerFormat texture to cover `rect` and
    // clears it to transparent, ready for one renderer's draw.
    bool Prepare(ID3D11Device* device, ID3D11DeviceContext* context, const RECT& rect);
    void Release();
};

struct RetainedYuvOverlays {
    PackedP010Unpacker unpacker;
    YuvOverlayPatcher patcher;
    RetainedYuvOverlayLayer camera;
    RetainedYuvOverlayLayer cursor;
    // Precision of the RGB surface overlays used to blend into (route query).
    std::atomic<uint32_t> rgbLevels{1023u};
    uint64_t frames = 0;
    uint64_t patchedFrames = 0;
    uint64_t patchFailures = 0;
    uint64_t layerFailures = 0;  // a visible overlay its renderer could not draw into its layer
    uint64_t packedUnpacks = 0;   // packed P010 slots rebuilt into the encoder frame
    uint64_t unpackFailures = 0;  // packed P010 slots refused because the unpack failed
    uint64_t lastSummaryTick = 0;
    bool firstFrameLogged = false;
    bool sharedSlotLogged = false;  // first keyed-mutex read of a split-device slot

    void Release();
};
