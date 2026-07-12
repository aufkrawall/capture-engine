#pragma once

#include <d3d11.h>
#include <windows.h>
#include <cstdint>
#include <memory>

// Scale a BGRA bitmap using nearest-neighbor (point) filtering.
// Used to pre-scale cursor bitmaps to DPI-correct display size before GPU upload,
// so the D3D11 Video Processor does a 1:1 blit with no bilinear blur.
std::unique_ptr<uint8_t[]> ScaleBitmapNearestNeighbor(const uint8_t* src, uint32_t srcW, uint32_t srcH, uint32_t dstW,
                                                      uint32_t dstH);

// GPU-accelerated cursor renderer using D3D11 pixel shader compositing
// Design: Zero overhead when cursor is hidden - only GetCursorInfo() check
class CursorRenderer {
public:
    CursorRenderer();
    ~CursorRenderer();

    // Initialize with D3D11 device/context (call once)
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context);

    // Cleanup resources
    void Cleanup();

    // Composite cursor onto target texture if cursor is visible.
    // captureOrigin is the top-left screen-space origin of the captured content.
    // Returns true if cursor was drawn, false if cursor hidden or error
    // This is optimized to be very fast when cursor is hidden (single API call)
    bool CompositeOntoFrame(ID3D11Texture2D* targetTexture, int frameWidth, int frameHeight, int captureOriginX = 0,
                            int captureOriginY = 0, bool allowHandleVisibilityFallback = false);

    // Extract cursor bitmap to CPU memory (public for VP overlay use)
    bool ExtractCursorBitmap(HICON icon, uint8_t** outBitmap, uint32_t* outWidth, uint32_t* outHeight,
                             bool* outIsMonochrome);

private:
    // Check cursor state and update texture if shape changed
    bool UpdateCursorTexture(bool allowHandleVisibilityFallback = false);

    // Create D3D11 resources for rendering
    bool CreateRenderingResources();

    // D3D11 resources
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* cursorTexture = nullptr;
    ID3D11ShaderResourceView* cursorSRV = nullptr;
    ID3D11RenderTargetView* targetRTV = nullptr;
    ID3D11VertexShader* vertexShader = nullptr;
    ID3D11PixelShader* pixelShader = nullptr;
    ID3D11Buffer* vertexBuffer = nullptr;
    ID3D11Buffer* constantBuffer = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11BlendState* blendState = nullptr;
    ID3D11RasterizerState* rasterizerState = nullptr;

    // Cursor state cache
    HCURSOR lastCursor = nullptr;
    int hotspotX = 0;
    int hotspotY = 0;
    uint32_t cursorWidth = 0;
    uint32_t cursorHeight = 0;
    bool isMonochrome = false;
    bool resourcesCreated = false;

    // Constant buffer data for shader
    struct CursorConstants {
        float cursorX;       // Normalized X position (0-1)
        float cursorY;       // Normalized Y position (0-1)
        float cursorWidth;   // Normalized width
        float cursorHeight;  // Normalized height
    };
};
