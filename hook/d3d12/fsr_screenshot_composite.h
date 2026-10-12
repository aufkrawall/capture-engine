#pragma once

#include <cstdint>

namespace ce::fsr_screenshot {
struct CompositeConstants {
    uint32_t hasUi = 0;
    uint32_t premultipliedUi = 0;
    float uiScaleX = 1.0f;
    float uiScaleY = 1.0f;
};
static_assert(sizeof(CompositeConstants) == 4 * sizeof(uint32_t));

inline constexpr char kCompositeShaderSource[] = R"HLSL(
cbuffer CompositeConstants : register(b0) {
    uint hasUi;
    uint premultipliedUi;
    float2 uiScale;
};
Texture2D<float4> SceneTexture : register(t0);
Texture2D<float4> UiTexture : register(t1);

float4 VSMain(uint vertexId : SV_VertexID) : SV_Position {
    const float2 uv = float2((vertexId << 1) & 2, vertexId & 2);
    return float4(uv * float2(2.0f, -2.0f) + float2(-1.0f, 1.0f), 0.0f, 1.0f);
}

float4 PSMain(float4 position : SV_Position) : SV_Target {
    const float4 scene = SceneTexture.Load(int3(int2(position.xy), 0));
    float4 ui = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (hasUi != 0) {
        ui = UiTexture.Load(int3(int2(position.xy * uiScale), 0));
    }
    const float3 uiColor = premultipliedUi != 0 ? ui.rgb : ui.rgb * ui.a;
    return float4(uiColor + scene.rgb * (1.0f - ui.a), 1.0f);
}
)HLSL";
}  // namespace ce::fsr_screenshot
