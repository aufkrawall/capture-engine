#version 450

layout(binding = 1) uniform sampler2D fontTexture;

layout(location = 0) in vec4 inColor;
layout(location = 1) in vec2 inTexCoord;

layout(location = 0) out vec4 outColor;

void main() {
    float alpha = texture(fontTexture, inTexCoord).r;
    outColor = vec4(inColor.rgb, inColor.a * alpha);
}
