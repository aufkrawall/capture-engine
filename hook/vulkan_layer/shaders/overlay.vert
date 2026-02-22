#version 450

layout(push_constant) uniform PushConstants {
    vec2 viewportSize;
    float hdrMode;        // 0=SDR, 1=scRGB/FP16, 2=HDR10/PQ
    float paperWhiteNits; // SDR reference white in nits (default 200)
} pc;

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outTexCoord;

void main() {
    // Convert screen coordinates to NDC (-1 to 1)
    // Vulkan NDC: Y=-1 is TOP, Y=+1 is BOTTOM (opposite of DirectX)
    vec2 ndc;
    ndc.x = (inPosition.x / pc.viewportSize.x) * 2.0 - 1.0;
    ndc.y = (inPosition.y / pc.viewportSize.y) * 2.0 - 1.0;
    gl_Position = vec4(ndc, 0.0, 1.0);
    outColor = inColor;
    outTexCoord = inTexCoord;
}
