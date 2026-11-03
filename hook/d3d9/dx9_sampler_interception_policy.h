#pragma once

#include <cstdint>

#include "hook/overrides/sampler_override/hook_arming_policy.h"

// Which D3D9 device-vtable slots carry CE's sampler interception, and when.
// The rule is shared with D3D6/7 and D3D8 (hook_arming_policy.h): the per-draw
// slots - SetTexture, Get/SetSamplerState, CreateStateBlock and
// IDirect3DStateBlock9::Apply/Capture - are hooked only once a sampler override
// is configured; BeginStateBlock/EndStateBlock from the start.

namespace ce::dx9_sampler_interception {

enum Slot : uint32_t {
    kSlotSetTexture = 1u << 0,
    kSlotGetSamplerState = 1u << 1,
    kSlotSetSamplerState = 1u << 2,
    kSlotCreateStateBlock = 1u << 3,
    // IDirect3DStateBlock9::Apply/Capture, reached through state-block prototypes.
    kSlotStateBlockApply = 1u << 4,
    kSlotBeginStateBlock = 1u << 5,
    kSlotEndStateBlock = 1u << 6,
};

inline constexpr uint32_t kLifecycleSlots = kSlotBeginStateBlock | kSlotEndStateBlock;
inline constexpr uint32_t kPerDrawSlots =
    kSlotSetTexture | kSlotGetSamplerState | kSlotSetSamplerState | kSlotCreateStateBlock | kSlotStateBlockApply;

constexpr uint32_t SlotsFor(sampler_hook_arming::ArmStep step) {
    return (step.lifecycle ? kLifecycleSlots : 0u) | (step.overrides ? kPerDrawSlots : 0u);
}

// The shared arming rule, answered as the D3D9 slots to hook now.
class VTableArming {
public:
    uint32_t OnDeviceHooks(bool overrideConfigured) { return SlotsFor(arming_.OnDeviceHooks(overrideConfigured)); }
    uint32_t OnPresent(bool overrideConfigured) { return SlotsFor(arming_.OnFrame(overrideConfigured)); }
    bool PerDrawArmed() const { return arming_.OverridesArmed(); }

private:
    sampler_hook_arming::VTableArming arming_;
};

using sampler_hook_arming::RecordingSnapshotTrusted;
using sampler_hook_arming::ShadowTracksFromDefaults;

}  // namespace ce::dx9_sampler_interception
