#pragma once

#include <windows.h>

#include <array>
#include <cstddef>
#include <cstdint>

// What a Direct3D 7 / Direct3D 8 state block does to CE's texture-stage shadow.
// The same rules as D3D9 (dx9_state_block_sampler_policy.h), over 8 stages.
//
// CE keeps two views of every tracked texture-stage state: the application's
// LOGICAL value and the PHYSICAL value on the device (the forced one). An
// ApplyStateBlock changes the physical state behind CE's back. CE used to answer
// it by re-reading the physical values and re-deriving the desired ones from the
// logical values it already had - the ones from BEFORE the Apply - so every
// tracked state the block had just set was written straight back: the block was
// undone. With no override configured it was worse. The shadow is not kept up to
// date then (SetTextureStageState passes straight through), so an Apply wrote the
// stale shadow over the application's state: at first the defaults the shadow was
// seeded with at device creation, later whatever an earlier Apply had re-read.
//
// Now:
//   - with no override active nothing is written after an Apply and the shadow is
//     dropped, so it is bootstrapped from the device when an override is enabled;
//   - CreateStateBlock and CaptureStateBlock take the shadow's values for the
//     states the block covers; a recorded block (BeginStateBlock/EndStateBlock)
//     takes the application's own values, which CE records unforced;
//   - Apply merges the snapshot into the shadow and reconciles only the stages it
//     covers; a block CE never saw keeps the conservative path: re-read the device
//     and adopt every changed value as the application's.
//
// Block types have the same values in both APIs: D3DSBT_ALL = 1 and
// D3DSBT_PIXELSTATE = 2 cover every tracked texture-stage state (address,
// filters, LOD bias, MAXMIPLEVEL, MAXANISOTROPY); D3DSBT_VERTEXSTATE = 3 covers
// only texture-coordinate states, which CE does not track.

namespace ce::legacy_d3d_state_block {

inline constexpr size_t kStageCount = 8;
inline constexpr size_t kStateCount = 9;
using StageValues = std::array<DWORD, kStateCount>;

// Index 2 is D3DTSS_ADDRESSW, which only Direct3D 8 has.
inline constexpr uint16_t kAllStatesMask = static_cast<uint16_t>((1u << kStateCount) - 1u);
inline constexpr uint16_t kD3D7StatesMask = static_cast<uint16_t>(kAllStatesMask & ~(1u << 2));

inline constexpr DWORD kBlockTypeAll = 1;
inline constexpr DWORD kBlockTypePixelState = 2;

struct StageSnapshot {
    uint16_t stateMask = 0;  // bit i: tracked state i is part of the block
    // The values are known: recorded from the application, or captured from a
    // shadow that tracked the application at the time.
    bool valid = false;
    StageValues logical = {};
    StageValues physical = {};
};

struct Snapshot {
    std::array<StageSnapshot, kStageCount> stages = {};
};

inline bool BlockTypeCoversTextureStageStates(DWORD type) {
    return type == kBlockTypeAll || type == kBlockTypePixelState;
}

// A block created by CreateStateBlock(type), before its capture.
inline Snapshot MakeSnapshot(DWORD type, uint16_t trackedMask) {
    Snapshot snapshot;
    if (!BlockTypeCoversTextureStageStates(type)) {
        return snapshot;
    }
    for (StageSnapshot& stage : snapshot.stages) {
        stage.stateMask = trackedMask;
    }
    return snapshot;
}

// CreateStateBlock and CaptureStateBlock: the block now holds the device's
// current values for everything it covers. `shadowTracksApplication` is false
// while no override is active: the shadow is not kept up to date then.
template <typename Stage>
void CaptureSnapshot(Snapshot& snapshot, const std::array<Stage, kStageCount>& shadow,
                     bool shadowTracksApplication) {
    for (size_t i = 0; i < kStageCount; ++i) {
        StageSnapshot& stage = snapshot.stages[i];
        if (stage.stateMask == 0) {
            continue;
        }
        stage.valid = shadowTracksApplication && shadow[i].initialized;
        stage.logical = shadow[i].logical;
        stage.physical = shadow[i].physical;
    }
}

// A SetTextureStageState recorded between BeginStateBlock and EndStateBlock. The
// value goes into the block unforced, so the block restores it verbatim.
inline void RecordState(Snapshot& snapshot, size_t stage, size_t stateIndex, DWORD value) {
    if (stage >= kStageCount || stateIndex >= kStateCount) {
        return;
    }
    StageSnapshot& entry = snapshot.stages[stage];
    entry.stateMask = static_cast<uint16_t>(entry.stateMask | (1u << stateIndex));
    entry.logical[stateIndex] = value;
    entry.physical[stateIndex] = value;
    entry.valid = true;
}

struct ApplyResult {
    uint32_t coveredStages = 0;  // bit i: stage i is part of the block; reconcile these
    uint32_t staleStages = 0;    // covered, initialized, but the snapshot is unknown: drop and re-read
};

// Apply: the block's values are now on the device. An uninitialized stage stays
// so and is bootstrapped from the device by the caller.
template <typename Stage>
ApplyResult ApplySnapshotToShadow(const Snapshot& snapshot, std::array<Stage, kStageCount>& shadow) {
    ApplyResult result;
    for (size_t i = 0; i < kStageCount; ++i) {
        const StageSnapshot& stage = snapshot.stages[i];
        if (stage.stateMask == 0) {
            continue;
        }
        result.coveredStages |= 1u << i;
        Stage& state = shadow[i];
        if (!state.initialized) {
            continue;
        }
        if (!stage.valid) {
            result.staleStages |= 1u << i;
            continue;
        }
        for (size_t s = 0; s < kStateCount; ++s) {
            if (stage.stateMask & (1u << s)) {
                state.logical[s] = stage.logical[s];
                state.physical[s] = stage.physical[s];
            }
        }
    }
    return result;
}

// A block CE did not see being made: `state.physical` has just been re-read from
// the device. Whatever changed was set by the block, whose values are the
// application's own, so they become the logical values.
template <typename Stage>
void AdoptExternalPhysical(Stage& state, const StageValues& previousPhysical) {
    for (size_t s = 0; s < kStateCount; ++s) {
        if (state.physical[s] != previousPhysical[s]) {
            state.logical[s] = state.physical[s];
        }
    }
}

// Whether an Apply may write anything. With no override active the block's state
// is the application's and the shadow is not authoritative.
inline bool ApplyMayWrite(bool overrideConfigured, bool overrideActive, bool recording) {
    return !recording && (overrideConfigured || overrideActive);
}

}  // namespace ce::legacy_d3d_state_block
