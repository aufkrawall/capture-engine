#pragma once

namespace ce::video_color {

// Appended to kRgbColorConversionShaderSource + kPackedP010ShaderSource (it
// reuses VS_Main, CopyCB's yuvFlags/sdrWhiteNits, the forward Y'CbCr functions
// and the packed P010 readers there).
//
// Composites the face camera and the cursor into a retained NV12/P010 frame
// (llm-wiki/wgc-reservoir-vram-design.md). The background is the retained
// frame, read from t1/t2 (NV12/P010 planes) or, compiled with
// PATCH_PACKED_SOURCE, from t5 (a packed P010 slot); the result is written to
// the encoder's copy of it.
// Each overlay is a premultiplied RGBA layer rendered by its own renderer in
// the domain the encoder used to blend it into RGB: gamma-coded SDR, or linear
// scRGB for HDR. Blending repeats that RGB step per pixel (camera, then cursor,
// each quantized like the RGB surface it used to be drawn into), then converts
// with the same forward functions as PS_YuvY/UV and PS_P010Y/UV:
//  - pixels no overlay touches are discarded, so they keep the copied codes;
//  - opaque overlay pixels do not depend on the background at all;
//  - partially covered pixels need the background's R'G'B', reconstructed from
//    its exact luma and its chroma bilinearly upsampled at the format's siting.
// Chroma is the stored value plus the weighted change of the touched pixels in
// the encoder's own 4:2:0 footprint (SDR 3x2 left-sited, HDR 3x3 top-left
// sited). Inside one footprint the upsampled background chroma is shifted so
// its weighted sum equals the stored sample, so a fully covered sample equals
// the encoder's own average of the overlay exactly.
// Prepended to compile the packed-background variant; the build's shader
// manifest (tools/build/build_shaders.py) prepends this same text.
inline constexpr char kYuvOverlayPatchPackedDefine[] = R"(#define PATCH_PACKED_SOURCE 1
)";

inline constexpr char kYuvOverlayPatchShaderSource[] = R"(
Texture2D<float> patchSourceLuma : register(t1);
Texture2D<float2> patchSourceChroma : register(t2);
Texture2D<float4> patchCameraLayer : register(t3);
Texture2D<float4> patchCursorLayer : register(t4);

cbuffer PatchCB : register(b1)
{
    int4 cameraRect;   // left, top, right, bottom in frame pixels; empty when right <= left
    int4 cursorRect;
    int2 frameSize;
    uint patchHdr;     // 0: gamma-coded SDR blend, 1: linear scRGB blend into PQ BT.2020
    float rgbLevels;   // quantization of the RGB surface the overlays used to be drawn into
    float storedToCode;  // UNORM plane value -> code (255 NV12, 65535/64 P010)
    float3 _patchPad;
};

float PatchCode(float stored)
{
    return floor(stored * storedToCode + 0.5);
}

float PatchUnormFromCode(float code)
{
    return patchHdr != 0u ? P010UnormFromCode(code) : YuvUnormFromCode(code);
}

float3 SdrCodesToRgb(float3 codes)
{
    bool bt2020 = (yuvFlags & 2u) != 0u;
    float kr = bt2020 ? 0.2627 : 0.2126;
    float kb = bt2020 ? 0.0593 : 0.0722;
    bool eightBit = (yuvFlags & 1u) != 0u;
    float maxCode = eightBit ? 255.0 : 1023.0;
    float neutral = eightBit ? 128.0 : 512.0;
    float y;
    float cb;
    float cr;
    if ((yuvFlags & 4u) != 0u) {
        y = codes.x / maxCode;
        cb = (codes.y - neutral) / maxCode;
        cr = (codes.z - neutral) / maxCode;
    } else {
        float scale = eightBit ? 1.0 : 4.0;
        y = (codes.x / scale - 16.0) / 219.0;
        cb = (codes.y - neutral) / (scale * 224.0);
        cr = (codes.z - neutral) / (scale * 224.0);
    }
    float r = y + 2.0 * (1.0 - kr) * cr;
    float b = y + 2.0 * (1.0 - kb) * cb;
    float g = (y - kr * r - kb * b) / (1.0 - kr - kb);
    return float3(r, g, b);
}

float3 PqCodesToRgb(float3 codes)
{
    float y;
    float cb;
    float cr;
    if (sdrWhiteNits >= 1.0) {
        y = codes.x / 1023.0;
        cb = (codes.y - 512.0) / 1023.0;
        cr = (codes.z - 512.0) / 1023.0;
    } else {
        y = (codes.x - 64.0) / 876.0;
        cb = (codes.y - 512.0) / 896.0;
        cr = (codes.z - 512.0) / 896.0;
    }
    float r = y + 1.4746 * cr;
    float b = y + 1.8814 * cb;
    float g = (y - 0.2627 * r - 0.0593 * b) / 0.6780;
    return float3(r, g, b);
}

// Retained codes -> the RGB domain the overlays blend in.
float3 PatchBlendDomain(float3 codes)
{
    if (patchHdr == 0u)
        return SdrCodesToRgb(codes);
    float3 pq = saturate(PqCodesToRgb(codes));
    float3 nits = float3(PQToLinearNits(pq.r), PQToLinearNits(pq.g), PQToLinearNits(pq.b));
    return Rec2020ToRec709(nits) / 80.0;
}

// One overlay blend, stored at the precision of the surface it was drawn into
// (UNORM RGB for SDR, FP16 scRGB for HDR).
float3 PatchBlend(float4 layer, float3 below)
{
    float3 blended = layer.rgb + (1.0 - layer.a) * below;
    if (patchHdr != 0u)
        return f16tof32(f32tof16(blended));
    return floor(saturate(blended) * rgbLevels + 0.5) / rgbLevels;
}

bool PatchInside(int2 p, int4 rect)
{
    return p.x >= rect.x && p.y >= rect.y && p.x < rect.z && p.y < rect.w;
}

// Background in the blend domain -> composited R'G'B' as the encoder converts it.
float3 PatchComposite(int2 p, float3 below, out bool touched)
{
    touched = false;
    float3 c = below;
    if (PatchInside(p, cameraRect)) {
        float4 layer = patchCameraLayer.Load(int3(p - cameraRect.xy, 0));
        if (layer.a > 0.0) {
            c = PatchBlend(layer, c);
            touched = true;
        }
    }
    if (PatchInside(p, cursorRect)) {
        float4 layer = patchCursorLayer.Load(int3(p - cursorRect.xy, 0));
        if (layer.a > 0.0) {
            c = PatchBlend(layer, c);
            touched = true;
        }
    }
    if (patchHdr == 0u)
        return c;
    // scRGB -> the RGB10 PQ intermediate the encoder converts from.
    return floor(saturate(ScRgbToHdr10(c)) * 1023.0 + 0.5) / 1023.0;
}

float3 PatchForwardCodes(float3 rgb)
{
    return patchHdr != 0u ? PqP2020ToYcbcr(saturate(rgb)) : SdrRgbToYcbcrCodes(saturate(rgb));
}

float PatchLumaCode(int2 p)
{
#ifdef PATCH_PACKED_SOURCE
    return float(PackedP010Luma(p));
#else
    return PatchCode(patchSourceLuma.Load(int3(p, 0)));
#endif
}

float2 PatchChromaCodes(int2 c)
{
#ifdef PATCH_PACKED_SOURCE
    return float2(PackedP010Chroma(c));
#else
    float2 stored = patchSourceChroma.Load(int3(c, 0));
    return float2(PatchCode(stored.x), PatchCode(stored.y));
#endif
}

// Background chroma at luma pixel p: bilinear between the 4:2:0 samples, each
// sited at luma column 2c and row 2c (HDR top-left) or 2c + 0.5 (SDR left).
float2 PatchUpsampledChroma(int2 p)
{
    int2 chromaMax = frameSize / 2 - 1;
    int cx = p.x >> 1;
    float fx = (p.x & 1) != 0 ? 0.5 : 0.0;
    int cy;
    float fy;
    if (patchHdr != 0u) {
        cy = p.y >> 1;
        fy = (p.y & 1) != 0 ? 0.5 : 0.0;
    } else {
        cy = (p.y & 1) != 0 ? (p.y >> 1) : (p.y >> 1) - 1;
        fy = (p.y & 1) != 0 ? 0.25 : 0.75;
    }
    int x0 = clamp(cx, 0, chromaMax.x);
    int x1 = clamp(cx + 1, 0, chromaMax.x);
    int y0 = clamp(cy, 0, chromaMax.y);
    int y1 = clamp(cy + 1, 0, chromaMax.y);
    float2 top = lerp(PatchChromaCodes(int2(x0, y0)), PatchChromaCodes(int2(x1, y0)), fx);
    float2 bottom = lerp(PatchChromaCodes(int2(x0, y1)), PatchChromaCodes(int2(x1, y1)), fx);
    return lerp(top, bottom, fy);
}

float4 PS_PatchLuma(VS_OUT input) : SV_TARGET
{
    int2 p = int2(input.pos.xy);
    float3 codes = float3(PatchLumaCode(p), PatchUpsampledChroma(p));
    bool touched;
    float3 rgb = PatchComposite(p, PatchBlendDomain(codes), touched);
    if (!touched)
        discard;
    return float4(PatchUnormFromCode(PatchForwardCodes(rgb).x), 0.0, 0.0, 1.0);
}

// Unrolled, the packed variant inlines its four-texel chroma reads 36 times and
// compiles in ~0.9 s instead of ~0.12 s rolled. The arithmetic is the same:
// PackedP010GpuTest pins the packed variant to the planar one bit for bit.
#ifdef PATCH_PACKED_SOURCE
#define PATCH_FOOTPRINT_LOOP [loop]
#else
#define PATCH_FOOTPRINT_LOOP [unroll]
#endif

float4 PS_PatchChroma(VS_OUT input) : SV_TARGET
{
    int2 c = int2(input.pos.xy);
    float2 stored = PatchChromaCodes(c);
    const float weights[3] = {0.25, 0.5, 0.25};
    // HDR rows 2y-1..2y+1 weigh 1-2-1; SDR averages rows 2y and 2y+1.
    float footprintWeight[9];
    int2 footprintPixel[9];
    float2 footprintChroma[9];
    float2 upsampledSum = float2(0.0, 0.0);
    PATCH_FOOTPRINT_LOOP for (int i = 0; i < 9; ++i) {
        int dy = i / 3 - 1;
        int dx = i % 3 - 1;
        float rowWeight = patchHdr != 0u ? weights[dy + 1] : (dy < 0 ? 0.0 : 0.5);
        footprintWeight[i] = rowWeight * weights[dx + 1];
        footprintPixel[i] = int2(clamp(c.x * 2 + dx, 0, frameSize.x - 1), clamp(c.y * 2 + dy, 0, frameSize.y - 1));
        footprintChroma[i] = PatchUpsampledChroma(footprintPixel[i]);
        upsampledSum += footprintWeight[i] * footprintChroma[i];
    }
    float2 correction = stored - upsampledSum;
    float2 delta = float2(0.0, 0.0);
    bool any = false;
    PATCH_FOOTPRINT_LOOP for (int j = 0; j < 9; ++j) {
        if (footprintWeight[j] <= 0.0)
            continue;
        int2 q = footprintPixel[j];
        float2 background = footprintChroma[j] + correction;
        bool touched;
        float3 rgb = PatchComposite(q, PatchBlendDomain(float3(PatchLumaCode(q), background)), touched);
        if (touched) {
            any = true;
            delta += footprintWeight[j] * (PatchForwardCodes(rgb).yz - background);
        }
    }
    if (!any)
        discard;
    float2 code = stored + delta;
    return float4(PatchUnormFromCode(code.x), PatchUnormFromCode(code.y), 0.0, 1.0);
}
)";

}  // namespace ce::video_color
