#pragma once

#include <cstdint>

// Which D3D9 device-vtable slots carry CE's sampler interception, and when.
//
// Forced AF/LOD/mip overrides on D3D9 intercept SetTexture, SetSamplerState
// and IDirect3DStateBlock9::Apply - per-draw calls a UE3-class renderer makes
// tens of thousands of times a frame. A detour that only passes the call on
// still costs nanoseconds each there: Borderlands 2 (x86, CPU-bound, nothing
// forced) ran ~150 fps with CE against ~175 fps with an overlay that hooks only
// Present, while CE's whole Present path measured ~60 us a frame.
//
// So the per-draw slots are hooked only once a sampler override is configured:
// at device creation when one already is, otherwise at the first Present after
// the configuration gains one. A vtable stays armed afterwards - switching the
// override off restores the application's values through the same detours, and
// unpatching a slot a foreign overlay may have chained through is not safe.
//
// BeginStateBlock/EndStateBlock are hooked from the start. They are rare calls,
// and they tell whether a recording began before the per-draw slots were armed:
// such a recording missed Set* calls, so its snapshot would be incomplete and
// the block takes the device re-read path on Apply instead.
//
// The sampler shadow follows the same rule: it is seeded as tracked (D3D
// defaults known) only while the vtable is armed and an override is
// configured, because only then does CE see every Set* that changes it.

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

// One per device vtable; the caller serializes it (the vtable mutex). Each
// answer is the set of slots to hook now, handed out once: a failed install is
// logged by the caller, not retried on every Present.
class VTableArming {
public:
    // A device on this vtable is getting its hooks (creation or discovery).
    uint32_t OnDeviceHooks(bool overrideConfigured) {
        uint32_t slots = 0;
        if (!lifecycleHooked_) {
            lifecycleHooked_ = true;
            slots |= kLifecycleSlots;
        }
        if (overrideConfigured) {
            slots |= ArmPerDraw();
        }
        return slots;
    }

    // A device on this vtable presents.
    uint32_t OnPresent(bool overrideConfigured) { return overrideConfigured ? ArmPerDraw() : 0u; }

    bool PerDrawArmed() const { return perDrawArmed_; }

private:
    uint32_t ArmPerDraw() {
        if (perDrawArmed_) {
            return 0u;
        }
        perDrawArmed_ = true;
        return kPerDrawSlots;
    }

    bool lifecycleHooked_ = false;
    bool perDrawArmed_ = false;
};

// Whether the sampler shadow may start from the documented D3D9 defaults
// (device creation, Reset) instead of a bootstrap from the device.
constexpr bool ShadowTracksFromDefaults(bool perDrawArmed, bool overrideConfigured) {
    return perDrawArmed && overrideConfigured;
}

// Whether a BeginStateBlock recording that starts now will see all of its
// Set* calls, so its snapshot can stand in for the block on Apply.
constexpr bool RecordingSnapshotTrusted(bool perDrawArmedAtBegin) {
    return perDrawArmedAtBegin;
}

}  // namespace ce::dx9_sampler_interception
