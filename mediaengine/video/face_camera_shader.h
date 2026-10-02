#pragma once

namespace ce::face_camera {

inline constexpr char kShaderSource[] = R"(
cbuffer FaceCameraCB : register(b0) {
    float4 destinationRect;
    float4 sourceRect;
    float4 shapeParams;    // shape, corner radius fraction, border fraction, opacity
    float4 colorParams;    // color mode, paper white, mirror, unused
    float4 borderColor;
    float4 displayedSize;  // final displayed pixel width/height, unused, unused
};

Texture2D cameraTex : register(t0);
SamplerState cameraSampler : register(s0);

struct VS_OUTPUT {
    float4 pos : SV_POSITION;
    float2 localUv : TEXCOORD0;
    float2 sampleUv : TEXCOORD1;
};

VS_OUTPUT VS_Main(uint vertexId : SV_VertexID) {
    float2 corners[4] = {float2(0, 0), float2(1, 0), float2(0, 1), float2(1, 1)};
    float2 corner = corners[vertexId];
    float2 normalizedPosition = destinationRect.xy + corner * destinationRect.zw;
    float2 clipPosition = normalizedPosition * 2.0 - 1.0;
    clipPosition.y = -clipPosition.y;

    VS_OUTPUT output;
    output.pos = float4(clipPosition, 0, 1);
    output.localUv = corner;
    output.sampleUv = lerp(sourceRect.xy, sourceRect.zw, corner);
    return output;
}

float sRGBToLinear(float value) {
    return value <= 0.04045 ? value / 12.92 : pow((value + 0.055) / 1.055, 2.4);
}

float linearToPQ(float nits) {
    float lp = pow(max(nits, 0.0) / 10000.0, 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * lp) / (1.0 + 18.6875 * lp), 78.84375);
}

float3 rec709ToRec2020(float3 color) {
    return float3(0.6274040 * color.r + 0.3292820 * color.g + 0.0433136 * color.b,
                  0.0690970 * color.r + 0.9195400 * color.g + 0.0113612 * color.b,
                  0.0163916 * color.r + 0.0880132 * color.g + 0.8955950 * color.b);
}

float3 mapSdrToTarget(float3 srgb) {
    if (colorParams.x < 0.5)
        return srgb;
    float3 linear709 = float3(sRGBToLinear(srgb.r), sRGBToLinear(srgb.g), sRGBToLinear(srgb.b));
    if (colorParams.x < 1.5)
        return linear709 * (colorParams.y / 80.0);
    float3 linear2020 = rec709ToRec2020(linear709);
    return float3(linearToPQ(linear2020.r * colorParams.y), linearToPQ(linear2020.g * colorParams.y),
                  linearToPQ(linear2020.b * colorParams.y));
}

float roundedBoxDistance(float2 localUv, float radiusPixels) {
    float2 halfSize = displayedSize.xy * 0.5;
    float2 pixel = (localUv - 0.5) * displayedSize.xy;
    float2 q = abs(pixel) - max(halfSize - radiusPixels, 0.0);
    return length(max(q, 0.0)) + min(max(q.x, q.y), 0.0) - radiusPixels;
}

float4 PS_Main(VS_OUTPUT input) : SV_TARGET {
    float2 sampleUv = input.sampleUv;
    if (colorParams.z > 0.5)
        sampleUv.x = sourceRect.x + sourceRect.z - sampleUv.x;

    float shorterEdge = max(1.0, min(displayedSize.x, displayedSize.y));
    float radius = 0.0;
    if (shapeParams.x > 1.5)
        radius = shorterEdge * 0.5;
    else if (shapeParams.x > 0.5)
        radius = shorterEdge * saturate(shapeParams.y);

    float distanceToEdge = roundedBoxDistance(input.localUv, radius);
    float antialiasWidth = max(fwidth(distanceToEdge), 0.75);
    float outerCoverage = smoothstep(antialiasWidth, -antialiasWidth, distanceToEdge);
    float borderPixels = shorterEdge * max(shapeParams.z, 0.0);
    float innerCoverage = smoothstep(antialiasWidth, -antialiasWidth, distanceToEdge + borderPixels);

    float3 cameraRgb = mapSdrToTarget(cameraTex.Sample(cameraSampler, sampleUv).rgb);
    float3 borderRgb = mapSdrToTarget(borderColor.rgb);
    float borderAmount = outerCoverage > 0.0001 ? saturate((outerCoverage - innerCoverage) / outerCoverage) : 0.0;
    float3 outputRgb = lerp(cameraRgb, borderRgb, borderAmount);
    return float4(outputRgb, outerCoverage * saturate(shapeParams.w));
}
)";

}  // namespace ce::face_camera
