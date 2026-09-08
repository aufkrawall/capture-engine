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
};

// Far below one output interval at any supported rate, and well above the few
// hundred microseconds measured between CE's Present entry and the host's
// PresentStart for the same call.
inline constexpr int64_t kAssociationToleranceUs = 1500;

// Called from the frame-generation present callback, on the presenter thread.
void NoteCallbackEnd(int64_t callbackEndUs, bool generated);

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

}  // namespace ce::present_association
