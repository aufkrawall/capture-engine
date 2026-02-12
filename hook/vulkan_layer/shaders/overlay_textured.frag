#version 450

layout(binding = 1) uniform sampler2D fontTexture;

layout(location = 0) in vec4 inColor;
layout(location = 1) in vec2 inTexCoord;

layout(location = 0) out vec4 outColor;

void main() {
    // Flip V coordinate because font atlas is top-down but Vulkan expects bottom-up
    vec2 flippedUV = vec2(inTexCoord.x, 1.0 - inTexCoord.y);
    float alpha = texture(fontTexture, flippedUV).r;
    outColor = vec4(inColor.rgb, inColor.a * alpha);
}
