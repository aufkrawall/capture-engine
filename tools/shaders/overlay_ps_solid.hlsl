cbuffer Constants : register(b0) {
    float2 viewportSize;
    float hdrMode;        // 0=SDR, 1=scRGB/FP16, 2=HDR10/PQ
    float paperWhiteNits; // SDR reference white in nits (default 200)
};

struct PS_INPUT {
    float4 pos : SV_POSITION;
    float2 uv : TEXCOORD0;
    float4 col : COLOR0;
};

// sRGB to linear conversion (single component)
float SRGBToLinear(float s) {
    return (s <= 0.04045) ? (s / 12.92) : pow((s + 0.055) / 1.055, 2.4);
}

// PQ (ST 2084) OETF: linear nits to PQ signal
float LinearToPQ(float L) {
    float Lp = pow(L / 10000.0, 0.1593017578125);
    return pow((0.8359375 + 18.8515625 * Lp) / (1.0 + 18.6875 * Lp), 78.84375);
}

float3 ApplyHDR(float3 srgb) {
    float3 lin = float3(SRGBToLinear(srgb.r), SRGBToLinear(srgb.g), SRGBToLinear(srgb.b));
    if (hdrMode < 1.5) {
        // scRGB: 1.0 = 80 nits reference
        return lin * (paperWhiteNits / 80.0);
    } else {
        // HDR10/PQ: convert linear to nits, then PQ encode
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
