#version 450

layout(push_constant) uniform PushConstants {
    vec2 viewportSize;
} pc;

layout(location = 0) in vec2 inPosition;
layout(location = 1) in vec2 inTexCoord;
layout(location = 2) in vec4 inColor;

layout(location = 0) out vec4 outColor;
layout(location = 1) out vec2 outTexCoord;

void main() {
    // Convert screen coordinates to NDC: [0, width/height] -> [-1, 1]
    // Flip Y because screen Y=0 is top, NDC Y=+1 is top
    vec2 ndc = vec2(
        (inPosition.x / pc.viewportSize.x) * 2.0 - 1.0,
        1.0 - (inPosition.y / pc.viewportSize.y) * 2.0
    );
    gl_Position = vec4(ndc.x, ndc.y, 0.0, 1.0);
    outColor = inColor;
    outTexCoord = vec2(1.0 - inTexCoord.x, inTexCoord.y);  // Flip X for font
}
