#pragma once

// When a fixed-function Direct3D runtime (D3D6/7, D3D8, D3D9) gets CE's
// sampler-override hooks.
//
// Forced AF/LOD/mip overrides intercept the calls that set sampler or texture
// stage state, and a state block's Apply - per-draw calls a renderer makes
// thousands of times a frame. A detour that only passes the call on still costs
// nanoseconds each there: Borderlands 2 (D3D9, x86, CPU-bound, nothing forced)
// ran ~150 fps with CE against ~175 fps with an overlay that hooks only Present,
// and ~168 fps once these hooks stayed off without an override (2026-11-03).
//
// So those "override" hooks go in only once a sampler override is configured:
// at device creation when one already is, otherwise at the first frame boundary
// (Present, EndScene) after the configuration gains one. A vtable stays armed
// afterwards - switching the override off restores the application's values
// through the same detours, and unpatching a slot a foreign overlay may have
// chained through is not safe.
//
// The state-block recording pair (BeginStateBlock/EndStateBlock) is hooked from
// the start. It is rare, and it tells whether a recording began before the
// override hooks were armed: such a recording missed Set* calls, so its snapshot
// would be incomplete and the block takes the device re-read path on Apply.
//
// The sampler shadow follows the same rule: it starts from the documented
// defaults (device creation, Reset) only while the vtable is armed and an
// override is configured, because only then does CE see every call that
// changes it. Otherwise it bootstraps from the device when it is needed.

namespace ce::sampler_hook_arming {

// What to hook on one device vtable now. Each part is handed out once: a failed
// install is logged by the caller, not retried on every frame.
struct ArmStep {
    bool lifecycle = false;  // the state-block recording pair
    bool overrides = false;  // the per-draw override hooks (and whatever rides with them)
};

// One per device vtable; the caller serializes it (its vtable mutex).
class VTableArming {
public:
    // A device on this vtable is getting its hooks (creation or discovery).
    ArmStep OnDeviceHooks(bool overrideConfigured) {
        ArmStep step;
        if (!lifecycleHooked_) {
            lifecycleHooked_ = true;
            step.lifecycle = true;
        }
        step.overrides = overrideConfigured && ArmOverrides();
        return step;
    }

    // A device on this vtable reached a frame boundary.
    ArmStep OnFrame(bool overrideConfigured) {
        ArmStep step;
        step.overrides = overrideConfigured && ArmOverrides();
        return step;
    }

    bool OverridesArmed() const { return overridesArmed_; }

private:
    bool ArmOverrides() {
        if (overridesArmed_) {
            return false;
        }
        overridesArmed_ = true;
        return true;
    }

    bool lifecycleHooked_ = false;
    bool overridesArmed_ = false;
};

// Whether the sampler shadow may start from the documented defaults (device
// creation, Reset) instead of a bootstrap from the device.
constexpr bool ShadowTracksFromDefaults(bool overridesArmed, bool overrideConfigured) {
    return overridesArmed && overrideConfigured;
}

// Whether a BeginStateBlock recording that starts now will see all of its Set*
// calls, so its snapshot can stand in for the block on Apply.
constexpr bool RecordingSnapshotTrusted(bool overridesArmedAtBegin) {
    return overridesArmedAtBegin;
}

}  // namespace ce::sampler_hook_arming
