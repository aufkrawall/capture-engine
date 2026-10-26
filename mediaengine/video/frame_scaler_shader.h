#pragma once

namespace ce::video_scale {

// One separable pass of the shader scaler (scale_kernel.h). The table holds,
// per output sample, a float4 header (first source index, main-lobe tap
// range) and the weights. Taps past the source edge read the edge sample.
// The result is clamped to the range of the main lobe's samples, so side
// lobes sharpen without ringing.
inline constexpr char kFrameScalerShaderSource[] = R"(
Texture2D<float4> scaleSource : register(t0);
Buffer<float4> scaleTable : register(t1);

cbuffer ScaleCB : register(b0)
{
    uint tapGroups;  // taps / 4
    int sourceLast;  // last source index along the filtered axis
    uint2 _pad;
};

struct VS_OUT {
    float4 pos : SV_POSITION;
};

VS_OUT VS_Main(uint id : SV_VertexID) {
    VS_OUT o;
    float2 uv = float2((id == 1) ? 2.0f : 0.0f, (id == 2) ? 2.0f : 0.0f);
    o.pos = float4(uv.x * 2.0f - 1.0f, 1.0f - uv.y * 2.0f, 0.0f, 1.0f);
    return o;
}

float4 Resample(uint outIndex, int2 pixel, bool horizontal)
{
    uint base = outIndex * (tapGroups + 1u);
    float4 header = scaleTable.Load(base);
    int first = (int)header.x;
    int lobeBegin = (int)header.y;
    int lobeEnd = (int)header.z;
    float3 sum = 0.0;
    float3 lo = 3.0e38;
    float3 hi = -3.0e38;
    [loop] for (uint g = 0u; g < tapGroups; ++g) {
        float4 w = scaleTable.Load(base + 1u + g);
        [unroll] for (uint k = 0u; k < 4u; ++k) {
            int tap = (int)(g * 4u + k);
            int s = clamp(first + tap, 0, sourceLast);
            float3 c = scaleSource.Load(horizontal ? int3(s, pixel.y, 0) : int3(pixel.x, s, 0)).rgb;
            sum += w[k] * c;
            bool inLobe = tap >= lobeBegin && tap < lobeEnd;
            lo = inLobe ? min(lo, c) : lo;
            hi = inLobe ? max(hi, c) : hi;
        }
    }
    return float4(clamp(sum, lo, hi), 1.0);
}

float4 PS_ScaleH(VS_OUT input) : SV_TARGET
{
    int2 pixel = int2(input.pos.xy);
    return Resample((uint)pixel.x, pixel, true);
}

float4 PS_ScaleV(VS_OUT input) : SV_TARGET
{
    int2 pixel = int2(input.pos.xy);
    return Resample((uint)pixel.y, pixel, false);
}
)";

}  // namespace ce::video_scale
