#pragma once

#include <d3d11.h>
#include <windows.h>
#include <array>
#include <cstdint>
#include <vector>

// Composites premultiplied overlay layers (face camera, cursor) into an
// NV12/P010 frame (kYuvOverlayPatchShaderSource). The source frame (NV12,
// P010 or a packed P010 slot) is only read; the caller has already copied or
// unpacked it into `lumaTarget`/`chromaTarget`'s texture, and the patch
// rewrites just the pixels and 4:2:0 samples an overlay touches.
class YuvOverlayPatcher {
public:
    struct Layer {
        ID3D11ShaderResourceView* view = nullptr;  // premultiplied RGBA, texel (0,0) = rect.left/top
        RECT rect = {};                            // frame pixels; empty or null view = no layer
    };
    struct Params {
        bool hdr = false;          // linear scRGB blend, PQ BT.2020 P010 codes
        bool eightBit = false;     // NV12 (SDR only)
        bool bt2020 = false;       // SDR matrix
        bool fullRange = false;
        float rgbLevels = 1023.0f;  // SDR: precision of the RGB surface overlays used to be drawn into
    };

    YuvOverlayPatcher() = default;
    ~YuvOverlayPatcher();
    YuvOverlayPatcher(const YuvOverlayPatcher&) = delete;
    YuvOverlayPatcher& operator=(const YuvOverlayPatcher&) = delete;

    // Call during configuration, before the encoder thread starts. Init never compiles HLSL.
    bool PrepareShaders();
    bool Init(ID3D11Device* device, ID3D11DeviceContext* context);
    void Release();
    bool Apply(ID3D11Texture2D* source, ID3D11RenderTargetView* lumaTarget, ID3D11RenderTargetView* chromaTarget,
               UINT width, UINT height, const Layer& camera, const Layer& cursor, const Params& params);

    // Chroma samples whose 4:2:0 footprint can touch a luma rectangle
    // (left/top inclusive, right/bottom exclusive), clamped to the plane.
    static RECT ChromaFootprint(const RECT& lumaRect, UINT width, UINT height);
    static RECT ClipToFrame(const RECT& rect, UINT width, UINT height);

private:
    // VS_Main, planar PS_PatchLuma/Chroma, packed P010 PS_PatchLuma/Chroma.
    static constexpr size_t kShaderCount = 5;
    std::array<std::vector<uint8_t>, kShaderCount> bytecode_;
    ID3D11Device* device_ = nullptr;
    ID3D11DeviceContext* context_ = nullptr;
    ID3D11VertexShader* vertexShader_ = nullptr;
    ID3D11PixelShader* lumaShader_ = nullptr;
    ID3D11PixelShader* chromaShader_ = nullptr;
    ID3D11PixelShader* packedLumaShader_ = nullptr;
    ID3D11PixelShader* packedChromaShader_ = nullptr;
    ID3D11Buffer* copyConstants_ = nullptr;
    ID3D11Buffer* patchConstants_ = nullptr;
};
