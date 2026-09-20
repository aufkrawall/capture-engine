#pragma once

#include <cstdint>

#include "../../common/sharpen_policy.h"

// CPU side of the sharpen pass: the constant buffer the CAS and RCAS shaders
// read.
//
// The values are produced by AMD's own `ffxCasSetup` / `FsrRcasCon` out of
// `external/fidelityfx`, the same headers the shaders compile against, so the
// CPU and GPU halves can never drift into two different interpretations of the
// packed constants.
namespace ce::sharpen {

// Mirrors the `SharpenConstants` cbuffer in hook/shaders/sharpen_*.hlsl and the
// push-constant block in hook/vulkan_layer/shaders/sharpen_*.frag. 16-byte
// aligned members only, in declaration order, for both backends.
struct alignas(16) ShaderConstants {
    uint32_t const0[4] = {0, 0, 0, 0};
    uint32_t const1[4] = {0, 0, 0, 0};
    // Highest valid texel coordinate, so the kernel's neighbourhood taps clamp
    // at the frame edge instead of reading the zero a out-of-range Load returns.
    int32_t maxCoord[2] = {0, 0};
    // ce::sharpen::FilterSpace as a uint.
    uint32_t filterSpace = 0;
    // Weight of the filtered result against the original pixels, 0..1. This
    // consumes the block's former padding DWORD, so the layout and the D3D12
    // root-constant count are unchanged.
    float intensity = 1.0f;
};

static_assert(sizeof(ShaderConstants) == 48, "Shader constant layout must match the HLSL/GLSL blocks");

// `decision` must be one that ran (`Decide(...).run == true`); the caller has
// already refused everything else.
ShaderConstants BuildShaderConstants(Mode mode, const Decision& decision, uint32_t width, uint32_t height);

}  // namespace ce::sharpen
