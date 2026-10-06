#pragma once

#include <atomic>
#include <cstdint>

namespace ce::present_association {

// Associates a displayed transition with the frame-generation callback that
// produced it.
//
// Under free-running VRR a frame reaches the screen when it is finished, not
// when Present was called, so a PresentStart-anchored latency cannot separate
// "the runtime held the frame" from "the frame was not ready". The pacing trace
// can reconstruct that offline, but the trace only exists for saved episodes;
// the periodic health line needs the same decomposition from live state.
//
// The runtime presenter thread stages a callback end and commits it when the
// Present it belongs to enters CE's detour. The display-timing consumer then
// looks the Present up by its host PresentStart timestamp. Both clocks are QPC
// and describe the same call, so the lookup only needs a tolerance far below one
// output interval; no pointer, id or ordering assumption is involved.
//
// Producer side is wait-free and allocation-free. Readers observe a slot twice
// around its sequence counter and skip anything that was being written, so a
// contended read is a miss rather than a torn value.
struct Association {
    int64_t presentEntryUs = 0;
    int64_t callbackEndUs = 0;
    bool generated = false;
    // The runtime's own identity for the application frame this output came
    // from (FidelityFX frameID + 1; 0 when the callback carried none). A
    // generated frame carries the ID of the real frame it interpolates toward.
    uint64_t generatorFrameToken = 0;
};

// FidelityFX frame IDs may start at zero, so the token reserves zero for
// "unknown".
inline uint64_t GeneratorFrameToken(uint64_t frameId) {
    return frameId + 1;
}

// Far below one output interval at any supported rate, and well above the few
// hundred microseconds measured between CE's Present entry and the host's
// PresentStart for the same call.
inline constexpr int64_t kAssociationToleranceUs = 1500;

// Called from the frame-generation present callback, on the presenter thread.
void NoteCallbackEnd(int64_t callbackEndUs, bool generated, uint64_t generatorFrameToken = 0);

// Called when that thread's runtime Present enters CE's detour. Commits the
// staged callback end; a Present with no staged callback commits nothing, so a
// foreign or unrelated Present can never claim one.
void NotePresentEntry(int64_t presentEntryUs);

// Nearest committed association within the tolerance. Returns false when the
// present is unknown, still being written, or already overwritten.
bool Find(int64_t presentStartUs, Association& out);

// Drops staged and committed state. Used when the pacing epoch changes, so a
// display pair from before a frame-generation transition cannot be attributed
// to a callback from after it.
void Reset();

// What the callback said about the frame the Present now running on this
// thread carries. The runtime calls the callback and then Present for the same
// frame on the same thread, so its isGeneratedFrame is an exact real/generated
// verdict for that Present, not an inference.
struct PresentFrameVerdict {
    bool known = false;
    bool generated = false;
};

// Returns the verdict NotePresentEntry committed for this thread's current
// Present, and clears it so no later Present can inherit it. Unknown when no
// callback preceded the Present on this thread, when it was already consumed,
// or when Reset() ran in between.
PresentFrameVerdict ConsumePresentFrameVerdict();

// The same verdict without clearing it. For Present-entry decisions that run
// before ProcessFrame consumes it (the FPS limiter's call site).
PresentFrameVerdict PeekPresentFrameVerdict();

}  // namespace ce::present_association
