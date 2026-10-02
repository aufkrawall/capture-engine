#pragma once

#include <cstdint>

// When CE re-arms a D3D9 sampler slot another overlay has re-patched.
//
// CE hooks SetTexture/GetSamplerState/SetSamplerState by writing the device
// vtable slots. Another overlay can re-patch a slot at any time. When its
// handler forwards to whatever the slot held before (CE's detour), CE still
// sees every call; when it forwards straight to d3d9's own function - it
// resolved that from a clean vtable, or restored its own older snapshot - CE
// drops out and forced AF and the logical sampler shadow die silently.
//
// Writing CE's detour back into the slot is not an answer. CE's saved original
// would then have to be the foreign handler (joining a foreign chain: the
// mutual-hook cycle that froze Gothic II's DirectDraw Flip), or the foreign
// handler is evicted and re-patches, and the two ping-pong on one slot.
//
// CE instead goes BELOW the slot's owner, the rule the DXGI Present path follows
// for foreign entry patches: an inline hook on the body of d3d9's own function,
// which is the function CE saved as its original. Whatever the slot holds now
// ends in that function, so CE sees the call exactly once:
//   - a foreign handler that bypasses CE reaches the body hook;
//   - a foreign handler (or the slot) that reaches CE's vtable detour calls the
//     body hook's trampoline from there, never the patched body again.
// CE never calls the foreign handler and never writes the slot, so there is
// nothing for the other overlay to fight over.
//
// Guards:
//   - the drift must be stable - the same foreign value for kSettlePresents
//     consecutive checks - so a tool that is mid-install (or that toggles its
//     patch per call) is left alone;
//   - one attempt per slot per session, successful or not; a refusal or a
//     failure is logged once and never retried;
//   - the body is only hooked when CE's saved original lies inside the module
//     that owns the vtable. If CE itself hooked on top of another tool, that
//     "original" is foreign code, which CE does not patch.

namespace ce::dx9_sampler_rearm {

// About one to two seconds of presents.
inline constexpr uint32_t kSettlePresents = 120;

enum class Action : uint8_t {
    kNone,                   // the slot is CE's, or the slot was already handled
    kWait,                   // drifted, not yet stable
    kArm,                    // install the body hook now
    kRefuseForeignOriginal,  // CE's saved original is not the vtable owner's code
};

struct SlotWatch {
    const void* driftedTo = nullptr;
    uint32_t stablePresents = 0;
    bool attempted = false;
};

// One Present's look at one slot.
inline Action Observe(SlotWatch& watch, const void* slotValue, const void* ceDetour, bool originalOwnedByVtableModule) {
    if (slotValue == ceDetour) {
        watch.driftedTo = nullptr;
        watch.stablePresents = 0;
        return Action::kNone;
    }
    if (watch.attempted) {
        return Action::kNone;
    }
    if (slotValue != watch.driftedTo) {
        watch.driftedTo = slotValue;
        watch.stablePresents = 1;
        return Action::kWait;
    }
    if (++watch.stablePresents < kSettlePresents) {
        return Action::kWait;
    }
    watch.attempted = true;
    return originalOwnedByVtableModule ? Action::kArm : Action::kRefuseForeignOriginal;
}

}  // namespace ce::dx9_sampler_rearm
