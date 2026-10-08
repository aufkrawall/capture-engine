// Shared FP32 display-curve math for HLSL and GLSL. Call after sharpening and
// its intensity mix, immediately before the target's one final quantization.
FfxFloat32 ceGammaDecode(FfxFloat32 value, FfxFloat32 exponent) {
    if (value == 0.0 || abs(value) == 1.0) {
        return value;
    }
    FfxFloat32 magnitude = abs(value);
    FfxFloat32 decoded;
    if (exponent == 0.0) {
        decoded = magnitude <= 0.04045 ? magnitude / 12.92 : pow((magnitude + 0.055) / 1.055, FfxFloat32(2.4));
    } else {
        decoded = pow(magnitude, exponent);
    }
    return sign(value) * decoded;
}

FfxFloat32 ceGammaEncode(FfxFloat32 value, FfxFloat32 exponent) {
    if (value == 0.0 || abs(value) == 1.0) {
        return value;
    }
    FfxFloat32 magnitude = abs(value);
    FfxFloat32 encoded;
    if (exponent == 0.0) {
        encoded = magnitude <= 0.0031308 ? magnitude * 12.92 : 1.055 * pow(magnitude, FfxFloat32(1.0 / 2.4)) - 0.055;
    } else {
        encoded = pow(magnitude, FfxFloat32(1.0) / exponent);
    }
    return sign(value) * encoded;
}

// Deterministic spatial hash: no frame phase, temporal shimmer or extra texture.
// One threshold for all channels keeps neutral gradients neutral. The amplitude
// is half a destination code value; black/white endpoints remain exact.
FfxFloat32 ceGammaNoise(FfxInt32x2 position) {
    FfxUInt32 hash = FfxUInt32(position.x) * 0x9e3779b9u + FfxUInt32(position.y) * 0x85ebca6bu;
    hash ^= hash >> 16u;
    hash *= 0x7feb352du;
    hash ^= hash >> 15u;
    hash *= 0x846ca68bu;
    hash ^= hash >> 16u;
    return FfxFloat32(hash & 0x00ffffffu) * (1.0 / 16777216.0) - 0.5;
}

FfxFloat32 ceGammaConvert(FfxFloat32 value) {
    if (value == 0.0 || abs(value) == 1.0) {
        return value;
    }
    FfxFloat32 magnitude = abs(value);
    FfxFloat32 result;
    if (CE_GAMMA_SOURCE > 0.0 && CE_GAMMA_DESTINATION > 0.0) {
        result = pow(magnitude, CE_GAMMA_SOURCE / CE_GAMMA_DESTINATION);
    } else if (CE_GAMMA_SOURCE == 0.0) {
        result = magnitude <= 0.04045
                     ? pow(magnitude / 12.92, FfxFloat32(1.0) / CE_GAMMA_DESTINATION)
                     : pow((magnitude + 0.055) / 1.055, FfxFloat32(2.4) / CE_GAMMA_DESTINATION);
    } else if (CE_GAMMA_SOURCE == 2.2 || CE_GAMMA_SOURCE == 2.4) {
        // Source-code value corresponding to the sRGB linear breakpoint.
        FfxFloat32 breakpoint = CE_GAMMA_SOURCE == 2.2 ? FfxFloat32(0.07272128361848) : FfxFloat32(0.09047385459310);
        result = magnitude <= breakpoint ? 12.92 * pow(magnitude, CE_GAMMA_SOURCE)
                                          : 1.055 * pow(magnitude, CE_GAMMA_SOURCE / FfxFloat32(2.4)) - 0.055;
    } else {
        // An explicitly verified native engine exponent can be any 1..3 value.
        return ceGammaEncode(ceGammaDecode(value, CE_GAMMA_SOURCE), CE_GAMMA_DESTINATION);
    }
    return sign(value) * result;
}

FfxFloat32 ceGammaChannel(FfxFloat32 value, FfxFloat32 noise) {
    // sRGB views and linear SDR transport must return encoded values before
    // interpreting the game's assumed curve, then undo this for the store.
    FfxFloat32 encoded = (CE_GAMMA_VALUES_LINEAR & 1u) != 0u ? ceGammaEncode(value, 0.0) : value;
    encoded = ceGammaConvert(encoded);
    if (CE_GAMMA_DITHER_SCALE > 0.0 && encoded > 0.0 && encoded < 1.0) {
        // Quantize once, at the final output. Returning the center of the chosen
        // code also avoids an sRGB RTV approximation changing the dither choice.
        encoded = floor(encoded / CE_GAMMA_DITHER_SCALE + 0.5 + noise) * CE_GAMMA_DITHER_SCALE;
    }
    return (CE_GAMMA_VALUES_LINEAR & 2u) != 0u ? ceGammaDecode(encoded, 0.0) : encoded;
}

FfxFloat32x4 ceApplyGamma(FfxFloat32x4 color, FfxInt32x2 position) {
    if (CE_GAMMA_DESTINATION < 0.0 || CE_GAMMA_SOURCE == CE_GAMMA_DESTINATION) {
        return color;
    }
    FfxFloat32 noise = ceGammaNoise(position);
    color.r = ceGammaChannel(color.r, noise);
    color.g = ceGammaChannel(color.g, noise);
    color.b = ceGammaChannel(color.b, noise);
    return color;
}
