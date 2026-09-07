#pragma once

// When ExecuteCommandLists must run full command-queue registration.
//
// Registration exists to DISCOVER the game's DIRECT render queue. It takes the
// process-global command-queue mutex, calls GetDesc/GetDevice on the queue,
// re-points g_CommandQueue (COM AddRef/Release on a driver object) and hooks the
// queue's vtable. That is cheap once per queue and ruinous once per submission.
//
// A frame-generation runtime submits from its OWN internal queues. Those are, by
// construction, none of the queues CE knows, so an "is this queue unknown?" test
// alone re-runs registration on every interpolation submission and never settles:
// each registration re-points g_CommandQueue, which makes the queue that submits
// next look unknown again. Measured under 2x FSR FG at 3840x2160, registration ran
// on 1290 of 1290 submissions per second. Removing it made registrations settle at
// zero. A later broader queue-adoption probe recovered 1.9 ms per base frame, but
// also removed other state, so that larger gain must not be attributed to this one
// registration fix.
//
// The first observed DIRECT queue is a provisional execution anchor, not proof
// of presentation ownership. Exact swapchain bindings are tracked separately.
// Once FG owns presentation and discovery has an anchor, unknown submissions
// must not replace it with a runtime-internal queue. ECL coverage does not depend on it —
// the detour is installed on the queue vtable, which every queue of that device
// shares, so a queue CE never registers still reaches the hook.
namespace ce::dx12_overlay_policy {

// Submission is evidence that a queue executes work, not that it owns presentation. Preserve an
// established queue on the same device; an explicit binding or a proven device migration may replace it.
inline bool ShouldAdoptDiscoveredCommandQueue(bool fromExecuteCommandLists, bool hasCurrentQueue,
                                             bool incomingDeviceKnown, bool currentDeviceKnown,
                                             bool sameDevice) {
    if (!incomingDeviceKnown)
        return false;
    if (!hasCurrentQueue || !fromExecuteCommandLists)
        return true;
    return currentDeviceKnown && !sameDevice;
}

inline bool ShouldRegisterCommandQueueFromExecuteCommandLists(bool frameGenerationActive, bool hasPrimaryGameQueue,
                                                              bool runtimeOwnedPresentPath = false) {
    // No frame generation, or the game's queue not yet discovered: registration is
    // the discovery mechanism and must run. Adoption separately preserves an
    // established same-device queue even when another DIRECT queue submits work.
    // Disabling generation does not destroy the FFX presenter or its queues.
    // Discovery resumes when presentation ownership actually returns to the game.
    if ((!frameGenerationActive && !runtimeOwnedPresentPath) || !hasPrimaryGameQueue) {
        return true;
    }
    // FG active with the game's queue already known: a recognised queue needs no
    // re-registration, and an unrecognised one belongs to the runtime.
    return false;
}

// App-callback native FSR already gives CE the exact output resource and command
// list on every real/generated frame. In that state ExecuteCommandLists is not an
// overlay transport, queue-discovery source, or timing source. Traversing CE's
// normal ECL observers on AMD's submission threads is therefore pure interference.
// Keep every ambiguity on the full path: internal no-callback FSR needs ECL for its
// topmost-batch overlay route, Streamline needs it for PostSL handoff discovery,
// and CE's own nested submissions need their recursion/ownership guards.
inline bool ShouldTransparentForwardNativeFSRCallbackEcl(bool fsrApiActive, bool callbackBridgeExpected,
                                                         bool internalNoCallbackComposition,
                                                         bool streamlineFGRunning, bool postSLActive,
                                                         bool insideCEOverlaySubmission) {
    return fsrApiActive && callbackBridgeExpected && !internalNoCallbackComposition && !streamlineFGRunning &&
           !postSLActive && !insideCEOverlaySubmission;
}

}  // namespace ce::dx12_overlay_policy
