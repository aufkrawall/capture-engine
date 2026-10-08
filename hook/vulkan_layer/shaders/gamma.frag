#version 450
#extension GL_GOOGLE_include_directive : require
#define FFX_GPU 1
#define FFX_GLSL 1
#define FFX_HALF 0
#include "ffx_core.h"
#include "sharpen_common.glsl"

void main() {
    FfxInt32x2 position = FfxInt32x2(gl_FragCoord.xy);
    ceOutColor = ceApplyGamma(ceLoadSource(position), position);
}
