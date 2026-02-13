/**
 * Offline shader compiler for overlay shaders.
 * Compiles HLSL to DXBC and outputs C arrays for embedding.
 *
 * Build: cl /EHsc compile_shaders.cpp /link d3dcompiler.lib
 * Run:   compile_shaders.exe > shader_bytecode.h
 */

#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>

#pragma comment(lib, "d3dcompiler.lib")

static const char *g_VertexShaderSrc = R"(
cbuffer Constants : register(b0) {
    float2 viewportSize;
    float2 padding;
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
)";

static const char *g_PixelShaderSrc = R"(
Texture2D fontTexture : register(t0);
SamplerState fontSampler : register(s0);

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};

float4 main(PS_INPUT input) : SV_Target {
    float4 texColor = fontTexture.Sample(fontSampler, input.uv);
    return float4(input.col.rgb, input.col.a * texColor.a);
}
)";

static const char *g_PixelShaderSolidSrc = R"(
struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};

float4 main(PS_INPUT input) : SV_Target {
    return input.col;
}
)";

void DumpBlob(const char *name, ID3DBlob *blob) {
    const uint8_t *data = (const uint8_t *)blob->GetBufferPointer();
    size_t size = blob->GetBufferSize();

    printf("static const uint8_t %s[] = {\n    ", name);
    for (size_t i = 0; i < size; i++) {
        printf("0x%02x", data[i]);
        if (i + 1 < size) printf(",");
        if ((i + 1) % 16 == 0 && i + 1 < size) printf("\n    ");
        else if (i + 1 < size) printf(" ");
    }
    printf("\n};\n\n");
}

int main() {
    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3;
    ID3DBlob *blob = nullptr, *error = nullptr;
    HRESULT hr;

    printf("// Auto-generated shader bytecode - do not edit\n");
    printf("// Compiled with D3DCOMPILE_OPTIMIZATION_LEVEL3\n\n");
    printf("#pragma once\n#include <cstdint>\n\n");

    // VS 4.0 (DX11)
    hr = D3DCompile(g_VertexShaderSrc, strlen(g_VertexShaderSrc), nullptr,
                    nullptr, nullptr, "main", "vs_4_0", flags, 0, &blob, &error);
    if (FAILED(hr)) { fprintf(stderr, "VS 4.0 failed: %s\n", error ? (char*)error->GetBufferPointer() : "unknown"); return 1; }
    DumpBlob("g_VS_4_0", blob);
    blob->Release(); blob = nullptr;

    // VS 5.0 (DX12)
    hr = D3DCompile(g_VertexShaderSrc, strlen(g_VertexShaderSrc), nullptr,
                    nullptr, nullptr, "main", "vs_5_0", flags, 0, &blob, &error);
    if (FAILED(hr)) { fprintf(stderr, "VS 5.0 failed\n"); return 1; }
    DumpBlob("g_VS_5_0", blob);
    blob->Release(); blob = nullptr;

    // PS textured 4.0
    hr = D3DCompile(g_PixelShaderSrc, strlen(g_PixelShaderSrc), nullptr,
                    nullptr, nullptr, "main", "ps_4_0", flags, 0, &blob, &error);
    if (FAILED(hr)) { fprintf(stderr, "PS 4.0 failed\n"); return 1; }
    DumpBlob("g_PS_Textured_4_0", blob);
    blob->Release(); blob = nullptr;

    // PS textured 5.0
    hr = D3DCompile(g_PixelShaderSrc, strlen(g_PixelShaderSrc), nullptr,
                    nullptr, nullptr, "main", "ps_5_0", flags, 0, &blob, &error);
    if (FAILED(hr)) { fprintf(stderr, "PS 5.0 failed\n"); return 1; }
    DumpBlob("g_PS_Textured_5_0", blob);
    blob->Release(); blob = nullptr;

    // PS solid 4.0
    hr = D3DCompile(g_PixelShaderSolidSrc, strlen(g_PixelShaderSolidSrc), nullptr,
                    nullptr, nullptr, "main", "ps_4_0", flags, 0, &blob, &error);
    if (FAILED(hr)) { fprintf(stderr, "PS solid 4.0 failed\n"); return 1; }
    DumpBlob("g_PS_Solid_4_0", blob);
    blob->Release(); blob = nullptr;

    // PS solid 5.0
    hr = D3DCompile(g_PixelShaderSolidSrc, strlen(g_PixelShaderSolidSrc), nullptr,
                    nullptr, nullptr, "main", "ps_5_0", flags, 0, &blob, &error);
    if (FAILED(hr)) { fprintf(stderr, "PS solid 5.0 failed\n"); return 1; }
    DumpBlob("g_PS_Solid_5_0", blob);
    blob->Release(); blob = nullptr;

    fprintf(stderr, "All shaders compiled successfully.\n");
    return 0;
}
