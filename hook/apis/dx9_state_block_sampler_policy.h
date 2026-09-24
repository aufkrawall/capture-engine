#pragma once

#include <d3d9.h>

#include <array>
#include <cstddef>
#include <cstdint>

// What a D3D9 state block does to CE's sampler shadow.
//
// CE forces AF by keeping two views of every tracked sampler state: the
// application's LOGICAL value (what it set, what GetSamplerState returns to it)
// and the PHYSICAL value on the device (the forced one). A state block changes
// the physical state behind CE's back, and CE used to answer an Apply by
// re-reading the physical state and re-deriving the physical values from the
// logical ones it already had. Those logical values were the ones from BEFORE
// the Apply, so every tracked state the block had just set - an address mode,
// a filter, a LOD bias, MAXMIPLEVEL - was immediately written back to its old
// value: the block's sampler state was undone on every Apply. With no override
// configured the first Apply after device creation reset every sampler to the
// D3D defaults the shadow had been initialized with. Recording made it worse:
// between BeginStateBlock and EndStateBlock D3D9 records Set* calls instead of
// applying them, but the shadow took them as applied.
//
// A state block now carries a snapshot of the sampler values it will restore,
// in both views:
//   - CreateStateBlock(type) and Capture() take the shadow's current values for
//     the states that type covers (a block holds the physical state it saw,
//     which is exactly what the shadow's physical view says; the logical view
//     is what the application had set);
//   - a recorded block takes the application's own values - CE records them
//     unforced and forces them after the Apply, like any other Set.
// Apply merges the snapshot into the shadow and reconciles only the samplers
// the block covers. A block CE never saw created (made before injection) keeps
// the conservative path: re-read the device and adopt every changed value as
// the application's.
//
// D3DSBT_ALL covers textures and sampler states; D3DSBT_PIXELSTATE covers the
// sampler states but no textures; D3DSBT_VERTEXSTATE covers only
// D3DSAMP_DMAPOFFSET, which CE does not track.

namespace ce::dx9_sampler_state {

inline constexpr size_t kSamplerCount = 21;
inline constexpr size_t kStateCount = 9;
using SamplerStateValues = std::array<DWORD, kStateCount>;

inline constexpr std::array<D3DSAMPLERSTATETYPE, kStateCount> kTrackedTypes = {
    D3DSAMP_ADDRESSU,  D3DSAMP_ADDRESSV,      D3DSAMP_ADDRESSW,    D3DSAMP_MAGFILTER,     D3DSAMP_MINFILTER,
    D3DSAMP_MIPFILTER, D3DSAMP_MIPMAPLODBIAS, D3DSAMP_MAXMIPLEVEL, D3DSAMP_MAXANISOTROPY,
};

struct SamplerState {
    SamplerStateValues logical = {
        D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTADDRESS_WRAP, D3DTEXF_POINT, D3DTEXF_POINT, D3DTEXF_NONE, 0, 0, 1,
    };
    SamplerStateValues physical = logical;
    UINT textureMipLevels = 0;
    bool textureBound = false;
    bool textureUsesAddressW = false;
    bool textureSupportsAnisotropy = false;
    bool initialized = false;
    bool bootstrapAttempted = false;
};

using SamplerShadow = std::array<SamplerState, kSamplerCount>;

struct StateBlockSampler {
    uint16_t stateMask = 0;  // bit i: kTrackedTypes[i] is part of the block
    bool textureCovered = false;
    // The values below are known: recorded from the application, or captured
    // from a shadow that was initialized at the time.
    bool valid = false;
    SamplerStateValues logical = {};
    SamplerStateValues physical = {};
    UINT textureMipLevels = 0;
    bool textureBound = false;
    bool textureUsesAddressW = false;
    bool textureSupportsAnisotropy = false;
};

struct StateBlockSnapshot {
    std::array<StateBlockSampler, kSamplerCount> samplers = {};
};

enum class StateBlockCoverage : uint8_t {
    kNone,
    kSamplers,
    kSamplersAndTextures,
};

constexpr StateBlockCoverage CoverageForStateBlockType(D3DSTATEBLOCKTYPE type) {
    switch (type) {
        case D3DSBT_ALL:
            return StateBlockCoverage::kSamplersAndTextures;
        case D3DSBT_PIXELSTATE:
            return StateBlockCoverage::kSamplers;
        case D3DSBT_VERTEXSTATE:
        default:
            return StateBlockCoverage::kNone;
    }
}

inline constexpr uint16_t kAllTrackedStates = static_cast<uint16_t>((1u << kStateCount) - 1u);

inline bool SamplerCovered(const StateBlockSampler& sampler) {
    return sampler.stateMask != 0 || sampler.textureCovered;
}

inline void CopyTextureMetadata(const SamplerState& from, StateBlockSampler& to) {
    to.textureMipLevels = from.textureMipLevels;
    to.textureBound = from.textureBound;
    to.textureUsesAddressW = from.textureUsesAddressW;
    to.textureSupportsAnisotropy = from.textureSupportsAnisotropy;
}

inline void CopyTextureMetadata(const StateBlockSampler& from, SamplerState& to) {
    to.textureMipLevels = from.textureMipLevels;
    to.textureBound = from.textureBound;
    to.textureUsesAddressW = from.textureUsesAddressW;
    to.textureSupportsAnisotropy = from.textureSupportsAnisotropy;
}

// A block created by CreateStateBlock(type), before its capture.
inline StateBlockSnapshot MakeStateBlockSnapshot(StateBlockCoverage coverage) {
    StateBlockSnapshot snapshot;
    if (coverage == StateBlockCoverage::kNone) {
        return snapshot;
    }
    for (StateBlockSampler& sampler : snapshot.samplers) {
        sampler.stateMask = kAllTrackedStates;
        sampler.textureCovered = coverage == StateBlockCoverage::kSamplersAndTextures;
    }
    return snapshot;
}

// CreateStateBlock and Capture(): the block now holds the device's current
// values for everything it covers.
inline void CaptureStateBlock(StateBlockSnapshot& snapshot, const SamplerShadow& shadow) {
    for (size_t i = 0; i < kSamplerCount; ++i) {
        StateBlockSampler& sampler = snapshot.samplers[i];
        if (!SamplerCovered(sampler)) {
            continue;
        }
        sampler.valid = shadow[i].initialized;
        sampler.logical = shadow[i].logical;
        sampler.physical = shadow[i].physical;
        if (sampler.textureCovered) {
            CopyTextureMetadata(shadow[i], sampler);
        }
    }
}

// A SetSamplerState recorded between BeginStateBlock and EndStateBlock. The
// value goes into the block unforced, so the block restores it verbatim.
inline void RecordSamplerState(StateBlockSnapshot& snapshot, size_t sampler, size_t stateIndex, DWORD value) {
    if (sampler >= kSamplerCount || stateIndex >= kStateCount) {
        return;
    }
    StateBlockSampler& entry = snapshot.samplers[sampler];
    entry.stateMask = static_cast<uint16_t>(entry.stateMask | (1u << stateIndex));
    entry.logical[stateIndex] = value;
    entry.physical[stateIndex] = value;
    entry.valid = true;
}

// A SetTexture recorded between BeginStateBlock and EndStateBlock; `metadata`
// describes the recorded texture.
inline void RecordTexture(StateBlockSnapshot& snapshot, size_t sampler, const SamplerState& metadata) {
    if (sampler >= kSamplerCount) {
        return;
    }
    StateBlockSampler& entry = snapshot.samplers[sampler];
    entry.textureCovered = true;
    CopyTextureMetadata(metadata, entry);
    entry.valid = true;
}

// Apply: the block's values are now on the device. Returns the samplers it
// covered (bit i: sampler slot i), which are the only ones to reconcile. A
// covered sampler whose snapshot is unknown loses its shadow, so it is
// bootstrapped from the device again instead of trusting a stale view.
inline uint32_t ApplyStateBlockToShadow(const StateBlockSnapshot& snapshot, SamplerShadow& shadow) {
    uint32_t covered = 0;
    for (size_t i = 0; i < kSamplerCount; ++i) {
        const StateBlockSampler& sampler = snapshot.samplers[i];
        if (!SamplerCovered(sampler)) {
            continue;
        }
        covered |= 1u << i;
        SamplerState& state = shadow[i];
        if (!state.initialized) {
            continue;  // bootstrap reads whatever the device now holds
        }
        if (!sampler.valid) {
            state = SamplerState{};
            continue;
        }
        for (size_t s = 0; s < kStateCount; ++s) {
            if (sampler.stateMask & (1u << s)) {
                state.logical[s] = sampler.logical[s];
                state.physical[s] = sampler.physical[s];
            }
        }
        if (sampler.textureCovered) {
            CopyTextureMetadata(sampler, state);
        }
    }
    return covered;
}

// A block CE did not see being made: `state.physical` has just been re-read
// from the device. Whatever changed was set by the block, whose values are the
// application's own (CE never forced them), so they become the logical values.
inline void AdoptExternalPhysical(SamplerState& state, const SamplerStateValues& previousPhysical) {
    for (size_t s = 0; s < kStateCount; ++s) {
        if (state.physical[s] != previousPhysical[s]) {
            state.logical[s] = state.physical[s];
        }
    }
}

}  // namespace ce::dx9_sampler_state
