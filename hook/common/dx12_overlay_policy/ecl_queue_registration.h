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
// next look unknown again. Measured under 2x FSR FG at 3840x2160: registration ran
// on 1290 of 1290 submissions per second and cost the game 1.9 ms per base frame
// (191 -> 136 fps) — not as GPU work, but because the runtime's submission threads
// serialized on CE's mutex instead of feeding the GPU (board power fell from 150 W
// to 119 W at an unchanged SM clock while the app's own command list kept taking
// the same 4.2 ms).
//
// The game's render queue is the first DIRECT queue CE sees, and it is created
// before any frame-generation runtime initializes. So once FG is active and that
// primary queue is known, a queue CE does not recognise belongs to the runtime:
// registering it is both pointless and wrong, because it would adopt the runtime's
// internal queue as the game's render queue. ECL coverage does not depend on it —
// the detour is installed on the queue vtable, which every queue of that device
// shares, so a queue CE never registers still reaches the hook.
namespace ce::dx12_overlay_policy {

inline bool ShouldRegisterCommandQueueFromExecuteCommandLists(bool frameGenerationActive, bool hasPrimaryGameQueue) {
    // No frame generation, or the game's queue not yet discovered: registration is
    // the discovery mechanism and must run. It is idempotent for the queue that is
    // already the tracked one, so this stays the pre-FG behaviour exactly.
    if (!frameGenerationActive || !hasPrimaryGameQueue) {
        return true;
    }
    // FG active with the game's queue already known: a recognised queue needs no
    // re-registration, and an unrecognised one belongs to the runtime.
    return false;
}

}  // namespace ce::dx12_overlay_policy
