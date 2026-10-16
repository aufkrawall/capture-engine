#pragma once

#include <d3d11.h>

// A separate overlay (cursor, face camera) is composited into the RGB source
// before the single RGB->YUV conversion. Normal capture surfaces are mutated
// only inside this small transactional region and restored before ownership
// returns to WGC/DXGI/inject. Restores unwind in reverse declaration order, so
// overlapping regions recover the exact source pixels.
struct OverlaySourceRestore {
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* target = nullptr;
    ID3D11Texture2D* backup = nullptr;
    UINT destinationX = 0;
    UINT destinationY = 0;
    UINT width = 0;
    UINT height = 0;
    bool active = false;

    OverlaySourceRestore() = default;
    OverlaySourceRestore(const OverlaySourceRestore&) = delete;
    OverlaySourceRestore& operator=(const OverlaySourceRestore&) = delete;
    ~OverlaySourceRestore() {
        if (!active || !context || !target || !backup || width == 0 || height == 0)
            return;
        const D3D11_BOX backupBox = {0, 0, 0, width, height, 1};
        context->CopySubresourceRegion(target, 0, destinationX, destinationY, 0, backup, 0, &backupBox);
    }
};
