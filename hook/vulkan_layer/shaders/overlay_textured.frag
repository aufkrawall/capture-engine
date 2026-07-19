#version 450

layout(push_constant) uniform PushConstants {
    vec2 viewportSize;
    float hdrMode;        // 0=SDR, 1=scRGB/FP16, 2=HDR10/PQ
    float paperWhiteNits; // SDR reference white in nits (default 200)
} pc;

layout(binding = 1) uniform sampler2D fontTexture;

layout(location = 0) in vec4 inColor;
layout(location = 1) in vec2 inTexCoord;

layout(location = 0) out vec4 outColor;

// sRGB to linear conversion (single component)
float sRGBToLinear(float s) {
    return (s <= 0.04045) ? (s / 12.92) : pow((s + 0.055) / 1.055, 2.4);
}

// PQ (ST 2084) OETF: linear nits to PQ signal
float linearToPQ(float L) {
    float Lp = pow(clamp(L, 0.0, 10000.0) / 10000.0, 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * Lp) / (1.0 + 18.6875 * Lp), 78.84375);
}

vec3 rec709ToRec2020(vec3 color) {
    return vec3(
        dot(vec3(0.6274038959, 0.3292830384, 0.0433130657), color),
        dot(vec3(0.0690972894, 0.9195403951, 0.0113623155), color),
        dot(vec3(0.0163914389, 0.0880133079, 0.8955952532), color));
}

vec3 applyHDR(vec3 srgb) {
    vec3 lin = vec3(sRGBToLinear(srgb.r), sRGBToLinear(srgb.g), sRGBToLinear(srgb.b));
    if (pc.hdrMode < 1.5) {
        // scRGB: 1.0 = 80 nits reference
        return lin * (pc.paperWhiteNits / 80.0);
    } else {
        // HDR10/PQ: convert linear to nits, then PQ encode
        vec3 nits = rec709ToRec2020(lin) * pc.paperWhiteNits;
        return vec3(linearToPQ(nits.r), linearToPQ(nits.g), linearToPQ(nits.b));
    }
}

void main() {
    float alpha = texture(fontTexture, inTexCoord).r;
    vec3 color = inColor.rgb;
    if (pc.hdrMode > 0.5) {
        color = applyHDR(color);
    }
    outColor = vec4(color, inColor.a * alpha);
}
