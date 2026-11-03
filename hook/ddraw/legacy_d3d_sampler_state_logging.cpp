#include "legacy_d3d_sampler_state.h"

#include <array>
#include <atomic>

#include "common/graphics/mip_mapping_policy.h"
#include "hook/overrides/sampler_override_utils.h"
#include "hook/runtime/hook_common.h"
#include "legacy_d3d_sampler_state_internal.h"

// Decision logging of the legacy (D3D6/7/8) texture-stage shadow: one line per
// stage when its mip or AF decision changes, capped per API.

namespace ce::legacy_d3d_sampler_state::detail {
namespace {

std::array<std::atomic<int>, 3> g_decisionLogs{};
std::array<std::atomic<int>, 3> g_mipDecisionLogs{};

const char* DecisionLabel(sampler_override::LegacyD3DForcedAFDecision decision) {
    using Decision = sampler_override::LegacyD3DForcedAFDecision;
    switch (decision) {
        case Decision::Allow:
            return "allow";
        case Decision::OverrideDisabled:
            return "override-disabled";
        case Decision::MipFilterDisabled:
            return "mip-filter-disabled";
        case Decision::BorderAddress:
            return "border-address";
        case Decision::NonMaterialAddress:
            return "non-material-address";
        case Decision::PointMinMag:
            return "point-min-mag";
        case Decision::Unsupported:
            return "unsupported";
    }
    return "unknown";
}

const char* MipDecisionLabel(sampler_override::LegacyD3DMipMappingDecision decision) {
    using Decision = sampler_override::LegacyD3DMipMappingDecision;
    switch (decision) {
        case Decision::Allow:
            return "allow";
        case Decision::OverrideDisabled:
            return "override-disabled";
        case Decision::MipFilterDisabled:
            return "mip-filter-disabled";
        case Decision::NonMaterialAddress:
            return "non-material-address";
    }
    return "unknown";
}

uint64_t MipDecisionFingerprint(const StageState& state, const GraphicsConfig& gfx,
                                const MipMappingResult& result) {
    uint64_t hash = 1469598103934665603ull;
    const auto mix = [&hash](uint64_t value) {
        hash ^= value;
        hash *= 1099511628211ull;
    };
    mix(static_cast<uint64_t>(ce::mip_mapping::ParseMode(gfx.mipMapping)));
    mix(static_cast<uint64_t>(result.decision));
    mix(gfx.samplerOverrideMode == "aggressive" ? 1u : 0u);
    mix(state.logical[0]);
    mix(state.logical[1]);
    mix(state.logical[2]);
    mix(state.logical[3]);
    mix(state.logical[4]);
    mix(state.logical[5]);
    mix(result.magFilter);
    mix(result.minFilter);
    mix(result.mipFilter);
    return hash;
}

}  // namespace

void LogMipDecision(DeviceState& deviceState, DWORD stageIndex, StageState& state, const GraphicsConfig& gfx,
                    const MipMappingResult& result) {
    if (!ce::mip_mapping::IsExplicit(ce::mip_mapping::ParseMode(gfx.mipMapping))) {
        state.loggedMipFingerprint = ~uint64_t{0};
        return;
    }

    const uint64_t fingerprint = MipDecisionFingerprint(state, gfx, result);
    if (state.loggedMipFingerprint == fingerprint)
        return;
    state.loggedMipFingerprint = fingerprint;

    const int logIndex = g_mipDecisionLogs[ApiIndex(deviceState.api)].fetch_add(1, std::memory_order_relaxed);
    if (logIndex < 32) {
        HookLogImportant(
            "%s: Sampler mip decision stage=%u decision=%s mode=%s address=%u/%u/%u "
            "min=%u->%u mag=%u->%u mip=%u->%u policy=%s (#%d)",
            ApiName(deviceState.api), stageIndex, MipDecisionLabel(result.decision), gfx.mipMapping.c_str(),
            state.logical[0], state.logical[1], state.logical[2], state.logical[4], result.minFilter,
            state.logical[3], result.magFilter, state.logical[5], result.mipFilter,
            gfx.samplerOverrideMode.c_str(), logIndex + 1);
    }
}

void LogAfDecision(DeviceState& deviceState, DWORD stageIndex, StageState& state,
                   const GraphicsConfig& gfx, sampler_override::LegacyD3DForcedAFDecision decision,
                   const std::array<DWORD, kStateCount>& desired) {
    const bool configured = !gfx.anisotropicFiltering.empty() && gfx.anisotropicFiltering != "default";
    if (!configured) {
        state.loggedAfDecision = -1;
        state.loggedAfRequest = 0;
        state.loggedAfAggressive = false;
        return;
    }

    const int decisionValue = static_cast<int>(decision);
    const UINT requested = sampler_override::IsAnisotropicOverrideEnabled(gfx)
                               ? sampler_override::GetConfiguredMaxAnisotropy(gfx)
                               : 1;
    const bool aggressive = gfx.samplerOverrideMode == "aggressive";
    if (state.loggedAfDecision == decisionValue && state.loggedAfRequest == requested &&
        state.loggedAfAggressive == aggressive) {
        return;
    }
    state.loggedAfDecision = decisionValue;
    state.loggedAfRequest = requested;
    state.loggedAfAggressive = aggressive;

    const int logIndex = g_decisionLogs[ApiIndex(deviceState.api)].fetch_add(1, std::memory_order_relaxed);
    if (logIndex < 32) {
        HookLogImportant(
            "%s: Sampler AF decision stage=%u decision=%s address=%u/%u/%u min=%u mag=%u mip=%u "
            "af=%s desiredAnisotropy=%u cap=%u policy=%s (#%d)",
            ApiName(deviceState.api), stageIndex, DecisionLabel(decision), state.logical[0], state.logical[1],
            state.logical[2], state.logical[4], state.logical[3], state.logical[5],
            gfx.anisotropicFiltering.c_str(), desired[8], deviceState.maxAnisotropy,
            gfx.samplerOverrideMode.c_str(), logIndex + 1);
    }
}

}  // namespace ce::legacy_d3d_sampler_state::detail
