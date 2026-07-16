#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D sceneColor;
layout(set = 0, binding = 1) uniform sampler2D motionVectors;
layout(set = 0, binding = 2) uniform sampler2D historyColor;

layout(push_constant) uniform TaaPush {
    float historyWeight;
    float resetHistory;
    float renderWidth;
    float renderHeight;
} pc;

void main() {
    vec2 texel = 1.0 / vec2(max(pc.renderWidth, 1.0), max(pc.renderHeight, 1.0));
    vec3 center = texture(sceneColor, inUv).rgb;
    vec3 neighborhoodMin = center;
    vec3 neighborhoodMax = center;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            vec3 sampleColor = texture(sceneColor, inUv + vec2(x, y) * texel).rgb;
            neighborhoodMin = min(neighborhoodMin, sampleColor);
            neighborhoodMax = max(neighborhoodMax, sampleColor);
        }
    }
    vec2 motion = texture(motionVectors, inUv).xy;
    vec3 history = texture(historyColor, clamp(inUv + motion, vec2(0.0), vec2(1.0))).rgb;
    history = clamp(history, neighborhoodMin, neighborhoodMax);
    float weight = pc.resetHistory > 0.5 ? 0.0 : clamp(pc.historyWeight, 0.0, 0.96);
    outColor = vec4(mix(center, history, weight), 1.0);
}
