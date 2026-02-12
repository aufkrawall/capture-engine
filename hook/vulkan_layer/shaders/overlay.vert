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
    // Simple passthrough - viewport handles Y orientation
    gl_Position = vec4(inPosition.x, inPosition.y, 0.0, 1.0);
    outColor = inColor;
    outTexCoord = inTexCoord;
}
