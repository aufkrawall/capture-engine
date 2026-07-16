#pragma once

#include <d3d11.h>
#include <windows.h>
#include <array>
#include <cstdint>
#include <memory>
#include "../common/cursor_capture_state.h"

enum class CursorColorMode : uint32_t {
    Sdr = 0,
    ScRgb = 1,
    Hdr10Pq = 2,
};

struct CursorBitmapData {
    std::unique_ptr<uint8_t[]> pixels;
    uint32_t width = 0;
    uint32_t height = 0;
    int32_t hotspotX = 0;
    int32_t hotspotY = 0;
    bool isMonochrome = false;
};

// Scale a BGRA bitmap using nearest-neighbor (point) filtering.
// Used to pre-scale cursor bitmaps to DPI-correct display size before GPU upload,
// so cursor composition never needs a video-oriented scaling filter.
std::unique_ptr<uint8_t[]> ScaleBitmapNearestNeighbor(const uint8_t* src, uint32_t srcW, uint32_t srcH, uint32_t dstW,
                                                      uint32_t dstH);

// GPU-accelerated cursor renderer using D3D11 pixel shader compositing.
// Cursor state is supplied by the capture timeline, so this renderer performs
// no live Win32 sampling and hidden cursors take the cheap state-check path.
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
    bool CompositeOntoFrame(ID3D11Texture2D* targetTexture, int frameWidth, int frameHeight,
                            const ce::cursor::CaptureState& state, CursorColorMode colorMode,
                            float paperWhiteNits = 200.0f);

    // Resolves the full, unclipped cursor rectangle and prepares the cached
    // point-sampled cursor texture. Used to back up only the pixels that the
    // subsequent draw can touch.
    bool GetCursorFrameRect(int frameWidth, int frameHeight, const ce::cursor::CaptureState& state, RECT* result);

    // Loads the best Windows cursor resource for the requested monitor-DPI
    // size. Larger custom/accessibility cursors are never downscaled.
    bool LoadCursorBitmap(HCURSOR cursor, uint32_t requestedWidth, uint32_t requestedHeight, CursorBitmapData* result);

private:
    bool ExtractCursorBitmap(HICON icon, uint8_t** outBitmap, uint32_t* outWidth, uint32_t* outHeight,
                             bool* outIsMonochrome);

    // Check cursor state and update texture if shape changed
    bool UpdateCursorTexture(const ce::cursor::CaptureState& state);

    // Create D3D11 resources for rendering
    bool CreateRenderingResources();
    ID3D11RenderTargetView* GetTargetRenderView(ID3D11Texture2D* targetTexture, const D3D11_TEXTURE2D_DESC& targetDesc);
    void ClearTargetRenderViewCache();

    // D3D11 resources
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* context = nullptr;
    ID3D11Texture2D* cursorTexture = nullptr;
    ID3D11ShaderResourceView* cursorSRV = nullptr;
    ID3D11VertexShader* vertexShader = nullptr;
    ID3D11PixelShader* pixelShader = nullptr;
    ID3D11Buffer* vertexBuffer = nullptr;
    ID3D11Buffer* constantBuffer = nullptr;
    ID3D11SamplerState* sampler = nullptr;
    ID3D11BlendState* blendState = nullptr;
    ID3D11RasterizerState* rasterizerState = nullptr;

    // Cursor state cache
    HCURSOR lastCursor = nullptr;
    uint32_t lastRequestedWidth = 0;
    uint32_t lastRequestedHeight = 0;
    int hotspotX = 0;
    int hotspotY = 0;
    uint32_t cursorWidth = 0;
    uint32_t cursorHeight = 0;
    bool isMonochrome = false;
    bool resourcesCreated = false;

    static constexpr std::size_t kTargetRtvCacheSize = 64;
    struct TargetRtvCacheEntry {
        ID3D11Texture2D* texture = nullptr;
        ID3D11RenderTargetView* view = nullptr;
        uint64_t lastUsed = 0;
    };
    std::array<TargetRtvCacheEntry, kTargetRtvCacheSize> targetRtvCache{};
    uint64_t targetRtvUseCounter = 0;
    uint32_t targetRtvWidth = 0;
    uint32_t targetRtvHeight = 0;
    uint32_t targetRtvArraySize = 0;
    DXGI_FORMAT targetRtvFormat = DXGI_FORMAT_UNKNOWN;
    DXGI_SAMPLE_DESC targetRtvSampleDesc = {};

    // Constant buffer data for shader
    struct CursorConstants {
        float cursorX;       // Normalized X position (0-1)
        float cursorY;       // Normalized Y position (0-1)
        float cursorWidth;   // Normalized width
        float cursorHeight;  // Normalized height
        float colorMode;
        float paperWhiteNits;
        float padding0;
        float padding1;
    };
};
