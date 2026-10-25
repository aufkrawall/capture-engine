#pragma once

namespace ce::video_color {

// Appended to kRgbColorConversionShaderSource (it reuses VS_Main, CopyCB, the
// luma functions of PS_YuvY/PS_P010Y and P010UnormFromCode there). The packed
// P010 slot layout of common/capture/packed_p010_layout.h in HLSL:
//  - PS_PackSdrP010/PS_PackHdrP010 replace the luma pass of the retained
//    ingress conversion: the same luma function at the same full-frame
//    position, plus the block's chroma the chroma pass just stored, written as
//    one R16_UINT texel per pixel;
//  - PS_UnpackP010Y/UV rebuild the P010 planes in the encoder's input frame;
//  - PackedP010Luma/Chroma read codes for the retained-YUV overlay patch.
// Codes are integers throughout; the only conversions are P010's own UNORM
// mapping, which P010UnormFromCode writes and PackedP010StoredCode inverts
// exactly (code * 64 is a representable 16-bit UNORM value).
inline constexpr char kPackedP010ShaderSource[] = R"(
Texture2D<uint> packedP010Slot : register(t5);
Texture2D<float2> packP010Chroma : register(t7);

uint PackedP010StoredCode(float stored)
{
    return min((uint)floor(stored * (65535.0 / 64.0) + 0.5), 1023u);
}

uint PackedP010Luma(int2 p)
{
    return packedP010Slot.Load(int3(p, 0)) & 1023u;
}

uint2 PackedP010Chroma(int2 c)
{
    int2 p = c * 2;
    uint word = ((packedP010Slot.Load(int3(p, 0)) >> 10) & 31u) |
                (((packedP010Slot.Load(int3(p + int2(1, 0), 0)) >> 10) & 31u) << 5) |
                (((packedP010Slot.Load(int3(p + int2(0, 1), 0)) >> 10) & 31u) << 10) |
                (((packedP010Slot.Load(int3(p + int2(1, 1), 0)) >> 10) & 31u) << 15);
    return uint2(word & 1023u, (word >> 10) & 1023u) ^ 512u;
}

uint PackP010Texel(int2 p, float lumaUnorm)
{
    float2 chroma = packP010Chroma.Load(int3(p >> 1, 0));
    uint word = (PackedP010StoredCode(chroma.x) ^ 512u) | ((PackedP010StoredCode(chroma.y) ^ 512u) << 10);
    uint piece = uint(p.x & 1) | (uint(p.y & 1) << 1);
    return PackedP010StoredCode(lumaUnorm) | (((word >> (5u * piece)) & 31u) << 10);
}

uint PS_PackSdrP010(VS_OUT input) : SV_TARGET
{
    return PackP010Texel(int2(input.pos.xy), YuvLumaUnorm(input));
}

uint PS_PackHdrP010(VS_OUT input) : SV_TARGET
{
    return PackP010Texel(int2(input.pos.xy), P010LumaUnorm(input));
}

float PS_UnpackP010Y(VS_OUT input) : SV_TARGET
{
    return P010UnormFromCode(float(PackedP010Luma(int2(input.pos.xy))));
}

float2 PS_UnpackP010UV(VS_OUT input) : SV_TARGET
{
    uint2 codes = PackedP010Chroma(int2(input.pos.xy));
    return float2(P010UnormFromCode(float(codes.x)), P010UnormFromCode(float(codes.y)));
}
)";

}  // namespace ce::video_color
