// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall

#pragma once

#include <d3d9.h>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <string>

#include "common/config/config.h"
#include "common/graphics/mip_bias_limits.h"
#include "common/graphics/mip_mapping_policy.h"
#include "common/platform/strict_float_parse.h"
#include "dx9_state_block_sampler_policy.h"
#include "hook/ddraw/lod_helper.h"
#include "hook/overrides/sampler_override_utils.h"

namespace ce::dx9_sampler_state {

struct ParsedSamplerConfig {
    bool overrideConfigured = false;
    bool aggressiveMode = false;
    bool afOff = false;
    bool afEnabled = false;
    DWORD forcedMaxAniso = 1;
    ce::mip_mapping::Mode mipMode = ce::mip_mapping::Mode::Default;
    bool hasExplicitMipBias = false;
    float explicitMipBias = 0.0f;
    bool mipBiasOffsetMode = false;
    bool mipBiasBaseMode = false;
    bool hasSgssaaBias = false;
    float sgssaaBias = 0.0f;
    bool forceMipBiasClamp = false;
    ce::mip_bias::Limits mipBiasLimits{};
    uint64_t configHash = 0;
};

inline uint64_t ComputeGraphicsConfigHash(const GraphicsConfig& gfx) {
    uint64_t hash = 1469598103934665603ULL;
    auto mixBytes = [&hash](const void* data, size_t len) {
        const auto* bytes = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < len; ++i) {
            hash ^= bytes[i];
            hash *= 1099511628211ULL;
        }
        hash ^= 0xFFU;
        hash *= 1099511628211ULL;
    };
    auto mixStr = [&mixBytes](const std::string& value) {
        mixBytes(value.data(), value.size());
    };
    mixStr(gfx.anisotropicFiltering);
    mixStr(gfx.mipMapping);
    mixStr(gfx.mipBias);
    mixStr(gfx.mipBiasMin);
    mixStr(gfx.mipBiasMax);
    mixStr(gfx.mipBiasMode);
    mixStr(gfx.msaaSamples);
    mixStr(gfx.samplerOverrideMode);
    mixBytes(&gfx.forceMipBiasClamp, sizeof(gfx.forceMipBiasClamp));
    mixBytes(&gfx.sgssaa, sizeof(gfx.sgssaa));
    mixBytes(&gfx.disableAutoMipBias, sizeof(gfx.disableAutoMipBias));
    return hash;
}

inline ParsedSamplerConfig ParseSamplerConfig(const GraphicsConfig& gfx) {
    ParsedSamplerConfig out{};
    out.configHash = ComputeGraphicsConfigHash(gfx);
    out.mipBiasLimits = ce::mip_bias::ParseLimits(gfx.mipBiasMin, gfx.mipBiasMax);

    out.aggressiveMode = (gfx.samplerOverrideMode == "aggressive");

    if (!gfx.anisotropicFiltering.empty() && gfx.anisotropicFiltering != "default") {
        if (gfx.anisotropicFiltering == "off") {
            out.afOff = true;
        } else {
            out.afEnabled = true;
            out.forcedMaxAniso = sampler_override::GetConfiguredMaxAnisotropy(gfx);
        }
    }

    if (!gfx.mipMapping.empty() && gfx.mipMapping != "default") {
        out.mipMode = ce::mip_mapping::ParseMode(gfx.mipMapping);
    }

    if (HasConfiguredMipBias(gfx)) {
        float parsed = 0.0f;
        if (ce::TryParseFiniteFloat(gfx.mipBias, parsed)) {
            out.hasExplicitMipBias = true;
            out.explicitMipBias = parsed;
        }
    }
    out.mipBiasOffsetMode = (gfx.mipBiasMode == "offset");
    out.mipBiasBaseMode = (gfx.mipBiasMode == "base");

    if (gfx.sgssaa && !gfx.disableAutoMipBias) {
        float sgssaaBias = 0.0f;
        if (GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), sgssaaBias)) {
            out.hasSgssaaBias = true;
            out.sgssaaBias = sgssaaBias;
        }
    }

    out.forceMipBiasClamp = gfx.forceMipBiasClamp;

    out.overrideConfigured =
        out.afOff || out.afEnabled || (out.mipMode != ce::mip_mapping::Mode::Default) ||
        HasMipBiasOverride(gfx) || out.hasSgssaaBias;
    return out;
}

inline sampler_override::D3D9SamplerForcedAFInfo MakeAFInfo(const SamplerState& state, UINT deviceMaxAnisotropy,
                                                            const std::array<DWORD, kStateCount>& desired) {
    sampler_override::D3D9SamplerForcedAFInfo info = {};
    info.addressU = desired[0];
    info.addressV = desired[1];
    info.addressW = desired[2];
    info.magFilter = desired[3];
    info.minFilter = desired[4];
    info.mipFilter = desired[5];
    const UINT maxMipLevel = desired[7];
    info.textureMipLevels = state.textureMipLevels > maxMipLevel ? state.textureMipLevels - maxMipLevel : 0;
    info.deviceMaxAnisotropy = state.textureSupportsAnisotropy ? deviceMaxAnisotropy : 1;
    info.textureBound = state.textureBound;
    info.usesAddressW = state.textureUsesAddressW;
    return info;
}

inline std::array<DWORD, kStateCount> BuildDesiredState(const SamplerState& state, UINT deviceMaxAnisotropy,
                                                        const ParsedSamplerConfig& parsed,
                                                        sampler_override::D3D9ForcedAFDecision* afDecision) {
    std::array<DWORD, kStateCount> desired = state.logical;
    const bool textureHasMips =
        state.textureBound && desired[7] < state.textureMipLevels && state.textureMipLevels - desired[7] > 1;
    const auto materialAddress = [](DWORD address) {
        return address == D3DTADDRESS_WRAP || address == D3DTADDRESS_MIRROR || address == D3DTADDRESS_CLAMP;
    };
    const bool safeAddress =
        parsed.aggressiveMode || (materialAddress(desired[0]) && materialAddress(desired[1]) &&
                                  (!state.textureUsesAddressW || materialAddress(desired[2])));

    if (textureHasMips && desired[5] != D3DTEXF_NONE && safeAddress) {
        ce::mip_mapping::ApplyDiscreteFilters(
            parsed.mipMode, static_cast<DWORD>(D3DTEXF_POINT),
            static_cast<DWORD>(D3DTEXF_LINEAR), static_cast<DWORD>(D3DTEXF_POINT),
            static_cast<DWORD>(D3DTEXF_LINEAR), static_cast<DWORD>(D3DTEXF_POINT),
            static_cast<DWORD>(D3DTEXF_LINEAR), desired[3], desired[4], desired[5]);
    }

    const auto info = MakeAFInfo(state, deviceMaxAnisotropy, desired);
    sampler_override::D3D9ForcedAFDecision decision = sampler_override::D3D9ForcedAFDecision::OverrideDisabled;
    if (!parsed.afEnabled) {
        decision = sampler_override::D3D9ForcedAFDecision::OverrideDisabled;
    } else if (!info.textureBound) {
        decision = sampler_override::D3D9ForcedAFDecision::MissingTexture;
    } else if (info.textureMipLevels <= 1) {
        decision = sampler_override::D3D9ForcedAFDecision::SingleMipTexture;
    } else if (info.mipFilter == D3DTEXF_NONE) {
        decision = sampler_override::D3D9ForcedAFDecision::MipFilterDisabled;
    } else if (info.addressU == D3DTADDRESS_BORDER || info.addressV == D3DTADDRESS_BORDER ||
               (info.usesAddressW && info.addressW == D3DTADDRESS_BORDER)) {
        decision = sampler_override::D3D9ForcedAFDecision::BorderAddress;
    } else if (info.deviceMaxAnisotropy < 2) {
        decision = sampler_override::D3D9ForcedAFDecision::Unsupported;
    } else if (!parsed.aggressiveMode) {
        const auto linearFilter = [](DWORD filter) {
            return filter == D3DTEXF_LINEAR || filter == D3DTEXF_ANISOTROPIC;
        };
        if (!materialAddress(info.addressU) || !materialAddress(info.addressV) ||
            (info.usesAddressW && !materialAddress(info.addressW))) {
            decision = sampler_override::D3D9ForcedAFDecision::NonMaterialAddress;
        } else if (!linearFilter(info.minFilter) || !linearFilter(info.magFilter)) {
            decision = sampler_override::D3D9ForcedAFDecision::PointMinMag;
        } else {
            decision = sampler_override::D3D9ForcedAFDecision::Allow;
        }
    } else {
        decision = sampler_override::D3D9ForcedAFDecision::Allow;
    }

    if (afDecision) {
        *afDecision = decision;
    }
    if (decision == sampler_override::D3D9ForcedAFDecision::Allow) {
        if (state.textureSupportsMagAnisotropy) {
            desired[3] = D3DTEXF_ANISOTROPIC;
        } else if (desired[3] == D3DTEXF_ANISOTROPIC || desired[3] == D3DTEXF_POINT) {
            desired[3] = D3DTEXF_LINEAR;
        }
        desired[4] = D3DTEXF_ANISOTROPIC;
        desired[8] = std::max<DWORD>(1u, std::min<DWORD>(parsed.forcedMaxAniso, static_cast<DWORD>(info.deviceMaxAnisotropy)));
    } else if (parsed.afOff) {
        if (desired[3] == D3DTEXF_ANISOTROPIC) {
            desired[3] = D3DTEXF_LINEAR;
        }
        if (desired[4] == D3DTEXF_ANISOTROPIC) {
            desired[4] = D3DTEXF_LINEAR;
        }
        desired[8] = 1;
    }

    float bias = 0.0f;
    if (parsed.forceMipBiasClamp) {
        bias = 0.0f;
    } else {
        float orig = std::bit_cast<float>(state.logical[6]);
        if (!parsed.hasExplicitMipBias) {
            bias = orig;
        } else if (parsed.mipBiasOffsetMode) {
            bias = orig + parsed.explicitMipBias;
        } else if (parsed.mipBiasBaseMode) {
            bias = (orig < 0.0f) ? (orig + parsed.explicitMipBias) : orig;
        } else {
            bias = parsed.explicitMipBias;
        }
        if (parsed.hasSgssaaBias) {
            bias += parsed.sgssaaBias;
        }
        bias = ce::mip_bias::ApplyLimits(bias, parsed.mipBiasLimits);
        bias = std::clamp(bias, ce::mip_bias::kMinBias, ce::mip_bias::kMaxBias);
    }
    desired[6] = std::bit_cast<DWORD>(bias);
    return desired;
}

}  // namespace ce::dx9_sampler_state
