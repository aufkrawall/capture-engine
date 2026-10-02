#pragma once

// Optional bounds on a texture sampler's mip LOD bias (`[Graphics]
// mip_bias_min` / `mip_bias_max`). Unlike `mip_bias`, which replaces or offsets
// the application's value, a limit leaves the application in charge: it still
// decides which samplers get a bias and how large it is, and only a value past
// the configured bound is pulled back onto it. A limit is the last step of the
// bias pipeline, after `mip_bias`, SGSSAA and the Unity clamp, so it bounds the
// value the driver actually receives. `force_mip_bias_clamp` still wins.
//
// Shared by the host's config loader, the hook (D3D9-D3D12, OpenGL) and the
// Vulkan layer, so all of them accept exactly the same text.

#include <string>

#include "common/platform/strict_float_parse.h"

namespace ce::mip_bias {

// The D3D sampler contract's MipLODBias range; FinalizeMipBias clamps to it too.
inline constexpr float kMinBias = -16.0f;
inline constexpr float kMaxBias = 15.99f;

struct Limits {
    bool hasMin = false;
    bool hasMax = false;
    float min = kMinBias;
    float max = kMaxBias;

    bool Active() const {
        return hasMin || hasMax;
    }
};

inline bool IsUnsetLimitText(const std::string& text) {
    return text.empty() || text == "default";
}

// "default" (or empty) is no limit. Anything else must be a finite number inside
// the sampler range; a malformed value is no limit rather than a guess.
inline bool TryParseLimit(const std::string& text, float& out) {
    if (IsUnsetLimitText(text)) {
        return false;
    }
    float value = 0.0f;
    if (!ce::TryParseFiniteFloat(text, value) || value < kMinBias || value > kMaxBias) {
        return false;
    }
    out = value;
    return true;
}

// A minimum above the maximum cannot be satisfied by any bias, so neither bound
// applies. The config loader rejects that pair with a diagnostic first; this is
// the defensive half for a hand-edited shared or process-local config.
inline Limits ParseLimits(const std::string& minText, const std::string& maxText) {
    Limits limits;
    limits.hasMin = TryParseLimit(minText, limits.min);
    limits.hasMax = TryParseLimit(maxText, limits.max);
    if (limits.hasMin && limits.hasMax && limits.min > limits.max) {
        return {};
    }
    return limits;
}

inline float ApplyLimits(float bias, const Limits& limits) {
    if (limits.hasMin && bias < limits.min) {
        bias = limits.min;
    }
    if (limits.hasMax && bias > limits.max) {
        bias = limits.max;
    }
    return bias;
}

}  // namespace ce::mip_bias
