// Shared scaffolding for the Vulkan CAS and RCAS fragment shaders. The
// HLSL counterpart is hook/shaders/sharpen_hlsl_common.hlsli; the two must
// agree on the constant block, which is ce::sharpen::ShaderConstants.

layout(push_constant) uniform SharpenConstantsBlock {
    FfxUInt32x4 ceConst0;
    FfxUInt32x4 ceConst1;
    FfxInt32x2 ceMaxCoord;
    FfxUInt32 ceFilterSpace;
    FfxFloat32 ceIntensity;
}
ceConstants;

layout(set = 0, binding = 0) uniform sampler2D ceSource;

layout(location = 0) out FfxFloat32x4 ceOutColor;

// ce::sharpen::FilterSpace values:
#define CE_FILTER_SPACE_LINEAR_TO_GAMMA 1u
#define CE_FILTER_SPACE_SCRGB_TO_PQ     2u
#define CE_GAMMA_EXPONENT 2.2

#define CE_PQ_M1 0.1593017578125
#define CE_PQ_M2 78.84375
#define CE_PQ_C1 0.8359375
#define CE_PQ_C2 18.8515625
#define CE_PQ_C3 18.6875
#define CE_PQ_RCP_M1 6.2773946360153255
#define CE_PQ_RCP_M2 0.012683313515655965

// Out-of-range texelFetch is undefined, so the neighbourhood taps clamp to the
// edge texel rather than relying on the sampler's address mode.
FfxFloat32x4 ceLoadSource(FfxInt32x2 position) {
    FfxInt32x2 clamped = clamp(position, FfxInt32x2(0, 0), ceConstants.ceMaxCoord);
    return texelFetch(ceSource, clamped, 0);
}

// scRGB is linear light and legally negative outside Rec.709, so the curve is
// applied to the magnitude with the sign restored. The exponents are exactly
// reciprocal, making the round trip lossless apart from float rounding.
FfxFloat32 ceEncodeGammaChannel(FfxFloat32 value) {
    return sign(value) * pow(abs(value), FfxFloat32(1.0 / CE_GAMMA_EXPONENT));
}

FfxFloat32 ceDecodeGammaChannel(FfxFloat32 value) {
    return sign(value) * pow(abs(value), FfxFloat32(CE_GAMMA_EXPONENT));
}

// ST 2084 (PQ) transfer curve for scRGB HDR frames. In scRGB, 1.0 represents
// 80 nits SDR white, with HDR highlights scaling up to 125.0 (10,000 nits).
// Transforming to PQ maps the entire [0, 10000] nits range strictly into [0, 1],
// preventing CAS/RCAS limiter and saturate clamps from crushing highlights.
FfxFloat32 ceLinearToPqChannel(FfxFloat32 value) {
    if (value <= 0.0) {
        return 0.0;
    }
    FfxFloat32 y = clamp(value * (80.0 / 10000.0), 0.0, 1.0);
    FfxFloat32 p = pow(y, FfxFloat32(CE_PQ_M1));
    return pow((FfxFloat32(CE_PQ_C1) + FfxFloat32(CE_PQ_C2) * p) / (1.0 + FfxFloat32(CE_PQ_C3) * p), FfxFloat32(CE_PQ_M2));
}

FfxFloat32 cePqToLinearChannel(FfxFloat32 pq) {
    if (pq <= 0.0) {
        return 0.0;
    }
    FfxFloat32 powered = pow(clamp(pq, 0.0, 1.0), FfxFloat32(CE_PQ_RCP_M2));
    FfxFloat32 denominator = max(FfxFloat32(CE_PQ_C2) - FfxFloat32(CE_PQ_C3) * powered, 1e-6);
    FfxFloat32 y = pow(max(powered - FfxFloat32(CE_PQ_C1), 0.0) / denominator, FfxFloat32(CE_PQ_RCP_M1));
    return y * (10000.0 / 80.0);
}

void ceToFilterSpace(inout FfxFloat32 red, inout FfxFloat32 green, inout FfxFloat32 blue) {
    if (ceConstants.ceFilterSpace == CE_FILTER_SPACE_LINEAR_TO_GAMMA) {
        red = ceEncodeGammaChannel(red);
        green = ceEncodeGammaChannel(green);
        blue = ceEncodeGammaChannel(blue);
    } else if (ceConstants.ceFilterSpace == CE_FILTER_SPACE_SCRGB_TO_PQ) {
        red = ceLinearToPqChannel(red);
        green = ceLinearToPqChannel(green);
        blue = ceLinearToPqChannel(blue);
    }
}

// The mix against the original happens here, in the frame's own stored space,
// rather than inside the kernel: that is what makes intensity mean "how much
// sharpening is visible" independently of how the kernel reacted to local
// contrast. Alpha is never filtered or mixed - a composition swapchain needs the
// exact value the game wrote, and sharpening coverage produces halos of its own.
FfxFloat32x4 ceResolveOutput(FfxFloat32x3 filtered, FfxFloat32x4 original) {
    if (ceConstants.ceFilterSpace == CE_FILTER_SPACE_LINEAR_TO_GAMMA) {
        filtered.r = ceDecodeGammaChannel(filtered.r);
        filtered.g = ceDecodeGammaChannel(filtered.g);
        filtered.b = ceDecodeGammaChannel(filtered.b);
    } else if (ceConstants.ceFilterSpace == CE_FILTER_SPACE_SCRGB_TO_PQ) {
        filtered.r = cePqToLinearChannel(filtered.r);
        filtered.g = cePqToLinearChannel(filtered.g);
        filtered.b = cePqToLinearChannel(filtered.b);
    }
    return FfxFloat32x4(mix(original.rgb, filtered, ceConstants.ceIntensity), original.a);
}
