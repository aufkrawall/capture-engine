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
    uint ceReserved;
};

Texture2D<float4> ceSource : register(t0);

#define FFX_GPU 1
#define FFX_HLSL 1
#define FFX_HALF 0
#include "../../external/fidelityfx/gpu/ffx_core.h"

// ce::sharpen::FilterSpace::LinearToGamma.
#define CE_FILTER_SPACE_LINEAR_TO_GAMMA 1u

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

FfxFloat32 ceEncodeGammaChannel(FfxFloat32 value) {
    return sign(value) * pow(abs(value), FfxFloat32(1.0 / CE_GAMMA_EXPONENT));
}

FfxFloat32 ceDecodeGammaChannel(FfxFloat32 value) {
    return sign(value) * pow(abs(value), FfxFloat32(CE_GAMMA_EXPONENT));
}

// AMD's input callback: the point where loaded samples are moved into the space
// the filter should reason about. SDR, sRGB and PQ targets are already
// perceptual and pass through untouched.
void ceToFilterSpace(inout FfxFloat32 red, inout FfxFloat32 green, inout FfxFloat32 blue) {
    if (ceFilterSpace == CE_FILTER_SPACE_LINEAR_TO_GAMMA) {
        red = ceEncodeGammaChannel(red);
        green = ceEncodeGammaChannel(green);
        blue = ceEncodeGammaChannel(blue);
    }
}

// Undo the working-space transform and restore the source alpha. The alpha
// channel is never filtered: a premultiplied or composition swapchain needs the
// exact value the game wrote, and sharpening coverage produces halos of its own.
FfxFloat32x4 ceResolveOutput(FfxFloat32x3 filtered, FfxInt32x2 position) {
    if (ceFilterSpace == CE_FILTER_SPACE_LINEAR_TO_GAMMA) {
        filtered.r = ceDecodeGammaChannel(filtered.r);
        filtered.g = ceDecodeGammaChannel(filtered.g);
        filtered.b = ceDecodeGammaChannel(filtered.b);
    }
    return FfxFloat32x4(filtered, ceLoadSource(position).a);
}
