cbuffer Constants : register(b0) {
    float2 viewportSize;
    float hdrMode;       // 0=SDR, 1=scRGB/FP16, 2=HDR10/PQ
    float paperWhiteNits; // SDR reference white in nits (default 200)
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
