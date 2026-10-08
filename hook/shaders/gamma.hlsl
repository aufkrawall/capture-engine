#include "sharpen_hlsl_common.hlsli"

float4 main(float4 pos : SV_POSITION) : SV_Target {
    FfxInt32x2 position = FfxInt32x2(pos.xy);
    return ceApplyGamma(ceLoadSource(position), position);
}
