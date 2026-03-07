#pragma once
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include "../../common/config.h"

// Helper to check if SGSSAA is requested and what the sample count is
// Returns true if SGSSAA should be applied, false otherwise.
// Populates 'outBias' with the correct negative LOD bias.
inline bool GetSGSSAABias(bool sgssaa, const char* msaa, float& outBias, int overrideSampleCount = 0) {
    if (!sgssaa) {
        return false;
    }

    int samples = 0;

    // Determine sample count from forced MSAA setting
    if (overrideSampleCount > 0) {
        samples = overrideSampleCount;
    } else {
        if (msaa[0] == 'd' || strcmp(msaa, "off") == 0) {
            // No forced MSAA, so we can't do SGSSAA (unless the game natively uses
            // MSAA, which we can't easily detect globally here) But if
            // overrideSampleCount is 0, we assume we are checking against the forced
            // setting.
            return false;
        } else if (strcmp(msaa, "2x") == 0 || strcmp(msaa, "2") == 0)
            samples = 2;
        else if (strcmp(msaa, "4x") == 0 || strcmp(msaa, "4") == 0)
            samples = 4;
        else if (strcmp(msaa, "8x") == 0 || strcmp(msaa, "8") == 0)
            samples = 8;
    }

    if (samples <= 1)
        return false;

    // Formula: Bias = log2(1 / sqrt(samples))
    // Simplifies to: -0.5 * log2(samples)
    // 2x -> -0.5 * 1 = -0.5
    // 4x -> -0.5 * 2 = -1.0
    // 8x -> -0.5 * 3 = -1.5

    outBias = -0.5f * std::log2((float)samples);
    return true;
}

inline bool HasConfiguredMipBias(const GraphicsConfig& gfx) {
    return !gfx.mipBias.empty() && gfx.mipBias != "default";
}

inline bool TryParseConfiguredMipBias(const GraphicsConfig& gfx, float& outBias) {
    if (!HasConfiguredMipBias(gfx)) {
        return false;
    }

    char* end = nullptr;
    outBias = std::strtof(gfx.mipBias.c_str(), &end);
    return end != gfx.mipBias.c_str();
}

inline float ApplyConfiguredMipBias(const GraphicsConfig& gfx, float originalBias) {
    if (gfx.forceMipBiasClamp) {
        return 0.0f;
    }

    float userBias = 0.0f;
    if (!TryParseConfiguredMipBias(gfx, userBias)) {
        return originalBias;
    }

    if (gfx.mipBiasMode == "offset") {
        return originalBias + userBias;
    }
    if (gfx.mipBiasMode == "base") {
        return (originalBias < 0.0f) ? (originalBias + userBias) : originalBias;
    }
    return userBias;
}

inline float FinalizeMipBias(const GraphicsConfig& gfx, float bias) {
    if (gfx.forceMipBiasClamp) {
        return 0.0f;
    }
    return std::clamp(bias, -16.0f, 15.99f);
}
