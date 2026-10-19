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

// Submission proves execution, not presentation ownership or device migration. An auxiliary queue
// may expose a different native/interposer device view while the live presentation route is unchanged.
// Discovery is fallback-only; explicit bindings replace the established pair, including across devices.
inline bool ShouldAdoptDiscoveredCommandQueue(bool fromExecuteCommandLists, bool hasCurrentQueue,
                                             bool incomingDeviceKnown) {
    return incomingDeviceKnown && (!hasCurrentQueue || !fromExecuteCommandLists);
}

inline bool ShouldRegisterCommandQueueFromExecuteCommandLists(bool frameGenerationActive, bool hasPrimaryGameQueue,
                                                              bool runtimeOwnedPresentPath = false) {
    // No frame generation, or the game's queue not yet discovered: registration is
    // the discovery mechanism and must run. Adoption separately preserves an
    // established queue even when another device's DIRECT queue submits work.
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

// Whether inject capture may copy the image this Present carries.
//
// A known present-callback verdict (ce::present_association) proves the Present
// is one of the frame-generation runtime's own outputs: the runtime ran the
// callback for exactly this frame on this thread and then presented it, so the
// swapchain backbuffer holds what reaches the screen, generated or application
// alike. Every such output is captured; recording only the application frames
// halved the recorded motion rate (Talos FSR FG, logs/20260930_032355: 120 fps
// on screen, `Input: 60 | Dup: 60` per second into a 120 fps CFR grid).
//
// Without a verdict, while a frame-generation runtime may be presenting, a zero
// command-list count since the previous Present marks a Present CE cannot
// attribute to a rendered image (an FG transport present or a duplicate), so
// only counted submissions qualify. The transparent route above counts nothing,
// which is why the verdict must decide wherever it exists.
//
// With no runtime that could present, every Present carries the application's
// own image and the count is not needed to attribute it. The count is also not
// reliable then: it covers only the frame-classification queue, and after an FSR
// FG phase Talos renders on another DIRECT queue (streamline_ownership.h). Its
// fresh Streamline swapchain ran FG-off at a steady 120 fps for 1.4 s with every
// Present counting zero, so nothing was captured until DLSS FG's final-output
// route took over (session 20261019_182822, FSR FG -> DLSS FG: 1.87 s of video
// frozen against a 219 ms gap in the game's presents).
inline bool IsPresentedFrameForCapture(bool eclCountedNoSubmissions, bool callbackVerdictKnown,
                                       bool frameGenerationMayPresent) {
    return callbackVerdictKnown || !eclCountedNoSubmissions || !frameGenerationMayPresent;
}

// Whether a frame-generation runtime can be the source of a Present CE sees: a
// classified generator (DLSS FG, FSR FG, Smooth Motion, or an unresolved mode),
// Streamline FG running, or FSR's internal no-callback composition. Streamline
// loaded without FG only passes the application's presents through.
inline bool MayFrameGenerationProduceThisPresent(bool runtimeModeGenerates, bool streamlineFGRunning,
                                                 bool noCallbackFSRComposition) {
    return runtimeModeGenerates || streamlineFGRunning || noCallbackFSRComposition;
}

}  // namespace ce::dx12_overlay_policy
