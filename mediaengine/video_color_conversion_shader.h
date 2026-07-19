#pragma once

namespace ce::video_color {

inline constexpr char kRgbColorConversionShaderSource[] = R"(
Texture2D texIn : register(t0);
SamplerState sam : register(s0);

cbuffer CopyCB : register(b0)
{
    uint colorTransform;
    uint _pad0;
    uint _pad1;
    uint _pad2;
};

struct VS_OUT {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD;
};

VS_OUT VS_Main(uint id : SV_VertexID) {
    VS_OUT o;
    o.uv  = float2((id == 1) ? 2.0f : 0.0f, (id == 2) ? 2.0f : 0.0f);
    o.pos = float4(o.uv.x * 2.0f - 1.0f, 1.0f - o.uv.y * 2.0f, 0.0f, 1.0f);
    return o;
}

float3 LinearToSRGB(float3 c)
{
    float3 lo = c * 12.92;
    float3 hi = 1.055 * pow(max(c, 0.0), 1.0 / 2.4) - 0.055;
    return float3(c.r < 0.0031308 ? lo.r : hi.r,
                  c.g < 0.0031308 ? lo.g : hi.g,
                  c.b < 0.0031308 ? lo.b : hi.b);
}

float LinearNitsToPQ(float nits)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float normalized = saturate(max(nits, 0.0) / 10000.0);
    float p = pow(normalized, m1);
    return pow((c1 + c2 * p) / (1.0 + c3 * p), m2);
}

float3 ScRgbToHdr10(float3 scRgb)
{
    // Desktop Duplication/WGC FP16 is linear scRGB: Rec.709 primaries and
    // 1.0 == 80 nits. Preserve extended highlights through the primary
    // conversion, then encode absolute luminance with ST 2084.
    float3 rec2020 = float3(0.6274040 * scRgb.r + 0.3292820 * scRgb.g + 0.0433136 * scRgb.b,
                            0.0690970 * scRgb.r + 0.9195400 * scRgb.g + 0.0113612 * scRgb.b,
                            0.0163916 * scRgb.r + 0.0880132 * scRgb.g + 0.8955950 * scRgb.b);
    return float3(LinearNitsToPQ(rec2020.r * 80.0), LinearNitsToPQ(rec2020.g * 80.0),
                  LinearNitsToPQ(rec2020.b * 80.0));
}

float4 PS_Main(VS_OUT input) : SV_TARGET {
    float4 c = texIn.Sample(sam, input.uv);
    if (colorTransform == 1) {
        c.rgb = LinearToSRGB(saturate(c.rgb));
    } else if (colorTransform == 2) {
        c.rgb = ScRgbToHdr10(c.rgb);
    }
    return c;  // Render target format handles packing / channel layout
}
)";

}  // namespace ce::video_color
