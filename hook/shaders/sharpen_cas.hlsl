// FidelityFX Contrast Adaptive Sharpening, sharpen-only (no scaling).
//
// The callbacks must be defined before ffx_cas.h is included: the header calls
// them directly and, unlike the FSR1 one, declares no prototypes of its own.

#include "sharpen_hlsl_common.hlsli"

FfxFloat32x3 casLoad(FfxInt32x2 position) {
    return ceLoadSource(position).rgb;
}

void casInput(inout FfxFloat32 red, inout FfxFloat32 green, inout FfxFloat32 blue) {
    ceToFilterSpace(red, green, blue);
}

#include "../../external/fidelityfx/gpu/cas/ffx_cas.h"

float4 main(float4 pos : SV_POSITION) : SV_Target {
    FfxUInt32x2 gxy = FfxUInt32x2(pos.xy);
    FfxFloat32x4 original = ceLoadSource(FfxInt32x2(gxy));
    FfxFloat32x3 filtered;
    // noScaling: the constants were built with identical input and output
    // extents, which is the only configuration this pass ever runs in.
    ffxCasFilter(filtered.r, filtered.g, filtered.b, gxy, ceConst0, ceConst1, true);
    return ceResolveOutput(filtered, original);
}
