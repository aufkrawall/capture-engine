#version 450

layout(location = 0) in vec2 inUv;
layout(location = 0) out vec4 outColor;
layout(set = 0, binding = 0) uniform sampler2D hudlessColor;
layout(set = 0, binding = 1) uniform sampler2D uiColor;

void main() {
    vec3 scene = texture(hudlessColor, inUv).rgb;
    vec4 ui = texture(uiColor, inUv);
    outColor = vec4(mix(scene, ui.rgb, clamp(ui.a, 0.0, 1.0)), 1.0);
}
