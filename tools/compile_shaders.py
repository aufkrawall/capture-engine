"""
Offline shader compiler for overlay shaders.
Compiles HLSL to DXBC bytecode and outputs a C header file.
Uses D3DCompile via ctypes - no MSVC needed.
"""

import ctypes
import ctypes.wintypes
import sys
import os

# Load d3dcompiler
d3dcompiler = ctypes.windll.d3dcompiler_47

# D3DCompile signature
class ID3DBlob(ctypes.Structure):
    pass

ID3DBlobPtr = ctypes.POINTER(ID3DBlob)

# Use COM interface manually
class ID3DBlobVtbl(ctypes.Structure):
    _fields_ = [
        ("QueryInterface", ctypes.c_void_p),
        ("AddRef", ctypes.c_void_p),
        ("Release", ctypes.CFUNCTYPE(ctypes.c_ulong, ctypes.c_void_p)),
        ("GetBufferPointer", ctypes.CFUNCTYPE(ctypes.c_void_p, ctypes.c_void_p)),
        ("GetBufferSize", ctypes.CFUNCTYPE(ctypes.c_size_t, ctypes.c_void_p)),
    ]

ID3DBlob._fields_ = [("lpVtbl", ctypes.POINTER(ID3DBlobVtbl))]

def blob_get_data(blob_ptr):
    blob = blob_ptr.contents
    vtbl = blob.lpVtbl.contents
    ptr = vtbl.GetBufferPointer(ctypes.cast(blob_ptr, ctypes.c_void_p))
    size = vtbl.GetBufferSize(ctypes.cast(blob_ptr, ctypes.c_void_p))
    return (ctypes.c_uint8 * size).from_address(ptr)

def blob_release(blob_ptr):
    blob = blob_ptr.contents
    vtbl = blob.lpVtbl.contents
    vtbl.Release(ctypes.cast(blob_ptr, ctypes.c_void_p))

D3DCompile = d3dcompiler.D3DCompile
D3DCompile.restype = ctypes.c_long
D3DCompile.argtypes = [
    ctypes.c_void_p,    # pSrcData
    ctypes.c_size_t,    # SrcDataSize
    ctypes.c_char_p,    # pSourceName
    ctypes.c_void_p,    # pDefines
    ctypes.c_void_p,    # pInclude
    ctypes.c_char_p,    # pEntrypoint
    ctypes.c_char_p,    # pTarget
    ctypes.c_uint,      # Flags1
    ctypes.c_uint,      # Flags2
    ctypes.POINTER(ID3DBlobPtr),  # ppCode
    ctypes.POINTER(ID3DBlobPtr),  # ppErrorMsgs
]

D3DCOMPILE_OPTIMIZATION_LEVEL3 = (1 << 15)

VS_SRC = b"""
cbuffer Constants : register(b0) {
    float2 viewportSize;
    float hdrMode;
    float paperWhiteNits;
};
struct VS_INPUT {
    float2 pos : POSITION;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};
struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};
PS_INPUT main(VS_INPUT input) {
    PS_INPUT output;
    output.pos.x = (input.pos.x / viewportSize.x) * 2.0 - 1.0;
    output.pos.y = 1.0 - (input.pos.y / viewportSize.y) * 2.0;
    output.pos.z = 0.0;
    output.pos.w = 1.0;
    output.uv = input.uv;
    output.col = input.col;
    return output;
}
"""

PS_TEXTURED_SRC = b"""
cbuffer Constants : register(b0) {
    float2 viewportSize;
    float hdrMode;
    float paperWhiteNits;
};
Texture2D fontTexture : register(t0);
SamplerState fontSampler : register(s0);
struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};
float SRGBToLinear(float s) {
    return (s <= 0.04045) ? (s / 12.92) : pow((s + 0.055) / 1.055, 2.4);
}
float LinearToPQ(float L) {
    float Lp = pow(L / 10000.0, 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * Lp) / (1.0 + 18.6875 * Lp), 78.84375);
}
float3 ApplyHDR(float3 srgb) {
    float3 lin = float3(SRGBToLinear(srgb.r), SRGBToLinear(srgb.g), SRGBToLinear(srgb.b));
    if (hdrMode < 1.5) {
        return lin * (paperWhiteNits / 80.0);
    } else {
        float3 nits = lin * paperWhiteNits;
        return float3(LinearToPQ(nits.r), LinearToPQ(nits.g), LinearToPQ(nits.b));
    }
}
float4 main(PS_INPUT input) : SV_Target {
    float4 texColor = fontTexture.Sample(fontSampler, input.uv);
    float3 color = input.col.rgb;
    if (hdrMode > 0.5) {
        color = ApplyHDR(color);
    }
    return float4(color, input.col.a * texColor.a);
}
"""

PS_TEXTURED_SDR_SRC = b"""
cbuffer Constants : register(b0) {
    float2 viewportSize;
    float hdrMode;
    float paperWhiteNits;
};
Texture2D fontTexture : register(t0);
SamplerState fontSampler : register(s0);
struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};
float4 main(PS_INPUT input) : SV_Target {
    float alpha = fontTexture.SampleLevel(fontSampler, input.uv, 0.0).a;
    return float4(input.col.rgb, input.col.a * alpha);
}
"""

PS_SOLID_SRC = b"""
cbuffer Constants : register(b0) {
    float2 viewportSize;
    float hdrMode;
    float paperWhiteNits;
};
struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};
float SRGBToLinear(float s) {
    return (s <= 0.04045) ? (s / 12.92) : pow((s + 0.055) / 1.055, 2.4);
}
float LinearToPQ(float L) {
    float Lp = pow(L / 10000.0, 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * Lp) / (1.0 + 18.6875 * Lp), 78.84375);
}
float3 ApplyHDR(float3 srgb) {
    float3 lin = float3(SRGBToLinear(srgb.r), SRGBToLinear(srgb.g), SRGBToLinear(srgb.b));
    if (hdrMode < 1.5) {
        return lin * (paperWhiteNits / 80.0);
    } else {
        float3 nits = lin * paperWhiteNits;
        return float3(LinearToPQ(nits.r), LinearToPQ(nits.g), LinearToPQ(nits.b));
    }
}
float4 main(PS_INPUT input) : SV_Target {
    float3 color = input.col.rgb;
    if (hdrMode > 0.5) {
        color = ApplyHDR(color);
    }
    return float4(color, input.col.a);
}
"""

# Descriptor-free textured pixel shader:
# Uses StructuredBuffer<uint> (root SRV at t0) instead of Texture2D, so
# SetDescriptorHeaps is never needed.  Avoids the NVIDIA driver stall
# caused by SetDescriptorHeaps + OMSetRenderTargets(swapchain) in the
# same command list.  The structured uint load keeps element addressing
# explicit; x86 primary text rendering routes through the Texture2D backend.
# Manual bilinear filtering matches the static sampler quality of the standard
# textured PS.
PS_TEXTURED_DESCFREE_SRC = b"""
cbuffer Constants : register(b0) {
    float2 viewportSize;
    float hdrMode;
    float paperWhiteNits;
    float2 fontTexSize;
};
StructuredBuffer<uint> fontPixels : register(t0);
struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};
float SRGBToLinear(float s) {
    return (s <= 0.04045) ? (s / 12.92) : pow((s + 0.055) / 1.055, 2.4);
}
float LinearToPQ(float L) {
    float Lp = pow(L / 10000.0, 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * Lp) / (1.0 + 18.6875 * Lp), 78.84375);
}
float3 ApplyHDR(float3 srgb) {
    float3 lin = float3(SRGBToLinear(srgb.r), SRGBToLinear(srgb.g), SRGBToLinear(srgb.b));
    if (hdrMode < 1.5) {
        return lin * (paperWhiteNits / 80.0);
    } else {
        float3 nits = lin * paperWhiteNits;
        return float3(LinearToPQ(nits.r), LinearToPQ(nits.g), LinearToPQ(nits.b));
    }
}
float4 LoadTexel(int2 coord) {
    coord = clamp(coord, int2(0, 0), int2(fontTexSize) - 1);
    uint index = uint(coord.y) * uint(fontTexSize.x) + uint(coord.x);
    uint packed = fontPixels[index];
    return float4(
        float((packed      ) & 0xFFu) / 255.0,
        float((packed >>  8) & 0xFFu) / 255.0,
        float((packed >> 16) & 0xFFu) / 255.0,
        float((packed >> 24) & 0xFFu) / 255.0
    );
}
float4 SampleBilinear(float2 uv) {
    float2 texel = uv * fontTexSize - 0.5;
    int2 t0 = int2(floor(texel));
    float2 f = frac(texel);
    float4 tl = LoadTexel(t0);
    float4 tr = LoadTexel(t0 + int2(1, 0));
    float4 bl = LoadTexel(t0 + int2(0, 1));
    float4 br = LoadTexel(t0 + int2(1, 1));
    return lerp(lerp(tl, tr, f.x), lerp(bl, br, f.x), f.y);
}
float4 main(PS_INPUT input) : SV_Target {
    float4 texColor = SampleBilinear(input.uv);
    float3 color = input.col.rgb;
    if (hdrMode > 0.5) {
        color = ApplyHDR(color);
    }
    return float4(color, input.col.a * texColor.a);
}
"""

def compile_shader(source, target, name):
    code = ID3DBlobPtr()
    errors = ID3DBlobPtr()

    hr = D3DCompile(
        source, len(source),
        None, None, None,
        b"main", target.encode(),
        D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        ctypes.byref(code), ctypes.byref(errors)
    )

    if hr < 0:
        if errors:
            err_data = blob_get_data(errors)
            print(f"ERROR compiling {name}: {bytes(err_data).decode()}", file=sys.stderr)
            blob_release(errors)
        raise RuntimeError(f"Failed to compile {name}, hr=0x{hr & 0xFFFFFFFF:08x}")

    data = bytes(blob_get_data(code))
    blob_release(code)
    if errors:
        blob_release(errors)

    print(f"  Compiled {name}: {len(data)} bytes", file=sys.stderr)
    return data

def format_array(name, data):
    lines = [f"static const uint8_t {name}[] = {{"]
    for i in range(0, len(data), 16):
        chunk = data[i:i+16]
        hex_vals = ", ".join(f"0x{b:02x}" for b in chunk)
        if i + 16 < len(data):
            hex_vals += ","
        lines.append(f"    {hex_vals}")
    lines.append("};")
    lines.append("")
    return "\n".join(lines)

def main():
    shaders = [
        (VS_SRC, "vs_4_0", "g_VS_4_0"),
        (VS_SRC, "vs_5_0", "g_VS_5_0"),
        (PS_TEXTURED_SRC, "ps_4_0", "g_PS_Textured_4_0"),
        (PS_TEXTURED_SRC, "ps_5_0", "g_PS_Textured_5_0"),
        (PS_TEXTURED_SDR_SRC, "ps_5_0", "g_PS_Textured_SDR_5_0"),
        (PS_SOLID_SRC, "ps_4_0", "g_PS_Solid_4_0"),
        (PS_SOLID_SRC, "ps_5_0", "g_PS_Solid_5_0"),
        (PS_TEXTURED_DESCFREE_SRC, "ps_5_0", "g_PS_Textured_DescFree_5_0"),
    ]

    output = []
    output.append("// Auto-generated shader bytecode - do not edit manually")
    output.append("// Generated by tools/compile_shaders.py")
    output.append("// Compiled with D3DCOMPILE_OPTIMIZATION_LEVEL3")
    output.append("")
    output.append("#pragma once")
    output.append("#include <cstdint>")
    output.append("")

    print("Compiling overlay shaders...", file=sys.stderr)
    for source, target, name in shaders:
        data = compile_shader(source, target, name)
        output.append(format_array(name, data))

    # Write output
    out_path = os.path.join(os.path.dirname(os.path.dirname(__file__)),
                           "hook", "common", "overlay_shader_bytecode.h")
    with open(out_path, "w") as f:
        f.write("\n".join(output))

    print(f"Written to {out_path}", file=sys.stderr)

if __name__ == "__main__":
    main()
