#version 450
#extension GL_GOOGLE_include_directive : require

#define FFX_GPU 1
#define FFX_GLSL 1
#define FFX_HALF 0

#include "ffx_core.h"
#include "sharpen_common.glsl"

#define FSR_RCAS_F 1
#include "fsr1/ffx_fsr1.h"

// ffx_fsr1.h declares these itself, so they are defined after the include.
FfxFloat32x4 FsrRcasLoadF(FfxInt32x2 position) {
    return ceLoadSource(position);
}

void FsrRcasInputF(inout FfxFloat32 red, inout FfxFloat32 green, inout FfxFloat32 blue) {
    ceToFilterSpace(red, green, blue);
}

void main() {
    FfxUInt32x2 gxy = FfxUInt32x2(gl_FragCoord.xy);
    FfxFloat32x3 filtered;
    FsrRcasF(filtered.r, filtered.g, filtered.b, gxy, ceConstants.ceConst0);
    ceOutColor = ceResolveOutput(filtered, FfxInt32x2(gxy));
}
