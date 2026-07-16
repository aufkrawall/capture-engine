#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D composedColor;

float triangularNoise(vec2 p, uint frameIndex) {
    vec3 seed = fract(vec3(p.xyx + vec2(float(frameIndex & 255u), float((frameIndex >> 8u) & 255u)).xyx) * 0.1031);
    seed += dot(seed, seed.yzx + 33.33);
    float a = fract((seed.x + seed.y) * seed.z);
    float b = fract((seed.y + seed.z) * seed.x * 1.37);
    return a - b;
}

layout(push_constant) uniform PresentPush {
    uint frameIndex;
} pc;

void main() {
    vec3 color = texture(composedColor, inUv).rgb;
    color += triangularNoise(gl_FragCoord.xy, pc.frameIndex) / 255.0;
    outColor = vec4(clamp(color, 0.0, 1.0), 1.0);
}
