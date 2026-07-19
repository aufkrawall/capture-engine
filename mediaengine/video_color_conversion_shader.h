#pragma once

namespace ce::video_color {

inline constexpr char kRgbColorConversionShaderSource[] = R"(
Texture2D texIn : register(t0);
SamplerState sam : register(s0);

cbuffer CopyCB : register(b0)
{
    uint colorTransform;
    uint _pad0;
    float2 outputInvSize;
    float lumaSharpenStrength;
    float sdrWhiteNits;
    float2 _pad1;
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

float PQToLinearNits(float pq)
{
    const float m1 = 0.1593017578125;
    const float m2 = 78.84375;
    const float c1 = 0.8359375;
    const float c2 = 18.8515625;
    const float c3 = 18.6875;
    float p = pow(saturate(pq), 1.0 / m2);
    float normalized = pow(max(p - c1, 0.0) / max(c2 - c3 * p, 0.000001), 1.0 / m1);
    return normalized * 10000.0;
}

float3 Rec2020ToRec709(float3 rec2020)
{
    return float3(1.6604910 * rec2020.r - 0.5876411 * rec2020.g - 0.0728499 * rec2020.b,
                  -0.1245505 * rec2020.r + 1.1328999 * rec2020.g - 0.0083494 * rec2020.b,
                  -0.0181508 * rec2020.r - 0.1005789 * rec2020.g + 1.1187297 * rec2020.b);
}

float3 CompressRec709Gamut(float3 rgb, float luminance)
{
    // Pull only out-of-gamut chroma toward the equal-luminance neutral axis.
    // This preserves luminance and hue direction much better than clipping
    // individual channels, at negligible cost and without another GPU pass.
    float3 neutral = luminance.xxx;
    float minChannel = min(rgb.r, min(rgb.g, rgb.b));
    if (minChannel < 0.0) {
        float chromaScale = luminance / max(luminance - minChannel, 0.000001);
        rgb = lerp(neutral, rgb, saturate(chromaScale));
    }
    float maxChannel = max(rgb.r, max(rgb.g, rgb.b));
    if (maxChannel > 1.0) {
        float chromaScale = (1.0 - luminance) / max(maxChannel - luminance, 0.000001);
        rgb = lerp(neutral, rgb, saturate(chromaScale));
    }
    return saturate(rgb);
}

float3 AbsoluteNitsToSdr(float3 rec709Nits)
{
    // Preserve linear SDR contrast below the configured Windows SDR white,
    // placing that white at 80% linear output. The remaining headroom uses a
    // smooth luminance knee so native HDR highlights remain distinguishable.
    float3 relative = rec709Nits / max(sdrWhiteNits, 80.0);
    float luminance = max(dot(relative, float3(0.2126, 0.7152, 0.0722)), 0.0);
    float mappedLuminance = luminance * 0.8;
    if (luminance > 1.0) {
        float highlight = luminance - 1.0;
        mappedLuminance = 0.8 + 0.2 * highlight / (highlight + 1.0);
    }
    float scale = luminance > 0.000001 ? mappedLuminance / luminance : 0.0;
    return LinearToSRGB(CompressRec709Gamut(relative * scale, mappedLuminance));
}

float3 ScRgbToSdr(float3 scRgb)
{
    // The Windows advanced-color composition is linear scRGB with 1.0 = 80 nits.
    return AbsoluteNitsToSdr(scRgb * 80.0);
}

float3 Hdr10ToSdr(float3 pqRec2020)
{
    float3 rec2020Nits = float3(PQToLinearNits(pqRec2020.r), PQToLinearNits(pqRec2020.g),
                                PQToLinearNits(pqRec2020.b));
    return AbsoluteNitsToSdr(Rec2020ToRec709(rec2020Nits));
}

float P010UnormFromCode(float code)
{
    // P010 stores each 10-bit code in the high bits of a 16-bit component.
    // Rendering the exact code*64 UNORM value also keeps the low six bits zero.
    return floor(clamp(code, 0.0, 1023.0) + 0.5) * (64.0 / 65535.0);
}

float3 PqP2020ToLimitedYcbcr(float3 rgb)
{
    // BT.2020 non-constant-luminance matrix, followed by the video-range
    // 10-bit code mapping: Y=64..940 and Cb/Cr=64..960 around neutral 512.
    float y = dot(rgb, float3(0.2627, 0.6780, 0.0593));
    float cb = (rgb.b - y) / 1.8814;
    float cr = (rgb.r - y) / 1.4746;
    return float3(64.0 + 876.0 * y, 512.0 + 896.0 * cb, 512.0 + 896.0 * cr);
}

float4 PS_P010Y(VS_OUT input) : SV_TARGET
{
    float3 rgb = texIn.Sample(sam, input.uv).rgb;
    if (lumaSharpenStrength > 0.0) {
        float3 neighbors = texIn.Sample(sam, input.uv + float2(outputInvSize.x, 0.0)).rgb;
        neighbors += texIn.Sample(sam, input.uv - float2(outputInvSize.x, 0.0)).rgb;
        neighbors += texIn.Sample(sam, input.uv + float2(0.0, outputInvSize.y)).rgb;
        neighbors += texIn.Sample(sam, input.uv - float2(0.0, outputInvSize.y)).rgb;
        rgb += (rgb - neighbors * 0.25) * lumaSharpenStrength;
    }
    rgb = saturate(rgb);
    float yCode = PqP2020ToLimitedYcbcr(rgb).x;
    return float4(P010UnormFromCode(yCode), 0.0, 0.0, 1.0);
}

float4 PS_P010UV(VS_OUT input) : SV_TARGET
{
    // The chroma target is half-resolution. Sample the four corresponding
    // output-luma locations so scaling and 4:2:0 downsampling happen in one
    // deterministic shader pass without a CPU/GPU synchronization point.
    float2 halfPixel = outputInvSize * 0.5;
    float3 rgb = texIn.Sample(sam, input.uv + float2(-halfPixel.x, -halfPixel.y)).rgb;
    rgb += texIn.Sample(sam, input.uv + float2(halfPixel.x, -halfPixel.y)).rgb;
    rgb += texIn.Sample(sam, input.uv + float2(-halfPixel.x, halfPixel.y)).rgb;
    rgb += texIn.Sample(sam, input.uv + float2(halfPixel.x, halfPixel.y)).rgb;
    float3 codes = PqP2020ToLimitedYcbcr(saturate(rgb * 0.25));
    return float4(P010UnormFromCode(codes.y), P010UnormFromCode(codes.z), 0.0, 1.0);
}

float4 PS_Main(VS_OUT input) : SV_TARGET {
    float4 c = texIn.Sample(sam, input.uv);
    if (colorTransform == 1) {
        c.rgb = LinearToSRGB(saturate(c.rgb));
    } else if (colorTransform == 2) {
        c.rgb = ScRgbToHdr10(c.rgb);
    } else if (colorTransform == 3) {
        c.rgb = ScRgbToSdr(c.rgb);
    } else if (colorTransform == 4) {
        c.rgb = Hdr10ToSdr(c.rgb);
    }
    return c;  // Render target format handles packing / channel layout
}
)";

}  // namespace ce::video_color
