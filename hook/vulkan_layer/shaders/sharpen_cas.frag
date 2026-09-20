#version 450
#extension GL_GOOGLE_include_directive : require

#define FFX_GPU 1
#define FFX_GLSL 1
#define FFX_HALF 0

#include "ffx_core.h"
#include "sharpen_common.glsl"

// ffx_cas.h calls these directly and declares no prototypes, so they have to be
// defined before it is included.
FfxFloat32x3 casLoad(FfxInt32x2 position) {
    return ceLoadSource(position).rgb;
}

void casInput(inout FfxFloat32 red, inout FfxFloat32 green, inout FfxFloat32 blue) {
    ceToFilterSpace(red, green, blue);
}

#include "cas/ffx_cas.h"

void main() {
    FfxUInt32x2 gxy = FfxUInt32x2(gl_FragCoord.xy);
    FfxFloat32x4 original = ceLoadSource(FfxInt32x2(gxy));
    FfxFloat32x3 filtered;
    // noScaling: the constants were built with identical input and output extents.
    ffxCasFilter(filtered.r, filtered.g, filtered.b, gxy, ceConstants.ceConst0, ceConstants.ceConst1, true);
    ceOutColor = ceResolveOutput(filtered, original);
}
