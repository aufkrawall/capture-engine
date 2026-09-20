// Shared scaffolding for the CAS and RCAS pixel shaders.
//
// The pass reads an untouched copy of the frame and writes the presentation
// target, so the source is always a plain non-arrayed 2D texture at the same
// extent as the target and one texel maps to one pixel.

cbuffer SharpenConstants : register(b0) {
    // ce::sharpen::ShaderConstants, in declaration order.
    uint4 ceConst0;
    uint4 ceConst1;
    int2 ceMaxCoord;
    uint ceFilterSpace;
    float ceIntensity;
};

Texture2D<float4> ceSource : register(t0);

#define FFX_GPU 1
#define FFX_HLSL 1
#define FFX_HALF 0
#include "../../external/fidelityfx/gpu/ffx_core.h"

// ce::sharpen::FilterSpace values:
#define CE_FILTER_SPACE_LINEAR_TO_GAMMA 1u
#define CE_FILTER_SPACE_SCRGB_TO_PQ     2u

// A neighbourhood tap that leaves the frame reads the zero an out-of-range
// Load returns, which would draw a dark rim one pixel wide around the image.
// Clamping to the edge texel is the standard extension for a sharpening kernel.
FfxFloat32x4 ceLoadSource(FfxInt32x2 position) {
    FfxInt32x2 clamped = clamp(position, FfxInt32x2(0, 0), ceMaxCoord);
    return ceSource.Load(FfxInt32x3(clamped, 0));
}

// scRGB carries linear light and legally goes below 0 (outside Rec.709) and
// above 1 (above paper white). A plain pow() would return NaN for the negative
// components, so the curve is applied to the magnitude and the sign restored.
// The exponent pair is exactly reciprocal, which makes the round trip lossless
// apart from float rounding.
#define CE_GAMMA_EXPONENT 2.2

#define CE_PQ_M1 0.1593017578125
#define CE_PQ_M2 78.84375
#define CE_PQ_C1 0.8359375
#define CE_PQ_C2 18.8515625
#define CE_PQ_C3 18.6875
#define CE_PQ_RCP_M1 6.2773946360153255
#define CE_PQ_RCP_M2 0.012683313515655965

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

// AMD's input callback: the point where loaded samples are moved into the space
// the filter should reason about. SDR, sRGB and PQ targets are already
// perceptual and pass through untouched.
void ceToFilterSpace(inout FfxFloat32 red, inout FfxFloat32 green, inout FfxFloat32 blue) {
    if (ceFilterSpace == CE_FILTER_SPACE_LINEAR_TO_GAMMA) {
        red = ceEncodeGammaChannel(red);
        green = ceEncodeGammaChannel(green);
        blue = ceEncodeGammaChannel(blue);
    } else if (ceFilterSpace == CE_FILTER_SPACE_SCRGB_TO_PQ) {
        red = ceLinearToPqChannel(red);
        green = ceLinearToPqChannel(green);
        blue = ceLinearToPqChannel(blue);
    }
}

// Undo the working-space transform, weight the result against the original
// pixel, and restore the source alpha.
//
// The mix happens here, in the frame's own stored space, rather than inside the
// kernel: that is what makes intensity mean "how much sharpening is visible"
// independently of how the kernel reacted to local contrast. The alpha channel
// is never filtered or mixed - a premultiplied or composition swapchain needs
// the exact value the game wrote, and sharpening coverage produces halos of its
// own.
FfxFloat32x4 ceResolveOutput(FfxFloat32x3 filtered, FfxFloat32x4 original) {
    if (ceFilterSpace == CE_FILTER_SPACE_LINEAR_TO_GAMMA) {
        filtered.r = ceDecodeGammaChannel(filtered.r);
        filtered.g = ceDecodeGammaChannel(filtered.g);
        filtered.b = ceDecodeGammaChannel(filtered.b);
    } else if (ceFilterSpace == CE_FILTER_SPACE_SCRGB_TO_PQ) {
        filtered.r = cePqToLinearChannel(filtered.r);
        filtered.g = cePqToLinearChannel(filtered.g);
        filtered.b = cePqToLinearChannel(filtered.b);
    }
    return FfxFloat32x4(lerp(original.rgb, filtered, ceIntensity), original.a);
}
