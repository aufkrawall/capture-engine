// FidelityFX RCAS, the FSR1 second pass, used on its own as a sharpener.
//
// FSR_RCAS_F selects the 32-bit path. ffx_fsr1.h declares the two input
// callbacks itself, so they are defined after the include rather than before.

#include "sharpen_hlsl_common.hlsli"

#define FSR_RCAS_F 1
#include "../../external/fidelityfx/gpu/fsr1/ffx_fsr1.h"

FfxFloat32x4 FsrRcasLoadF(FfxInt32x2 position) {
    return ceLoadSource(position);
}

void FsrRcasInputF(inout FfxFloat32 red, inout FfxFloat32 green, inout FfxFloat32 blue) {
    ceToFilterSpace(red, green, blue);
}

float4 main(float4 pos : SV_POSITION) : SV_Target {
    FfxUInt32x2 gxy = FfxUInt32x2(pos.xy);
    FfxFloat32x3 filtered;
    FsrRcasF(filtered.r, filtered.g, filtered.b, gxy, ceConst0);
    return ceResolveOutput(filtered, FfxInt32x2(gxy));
}
