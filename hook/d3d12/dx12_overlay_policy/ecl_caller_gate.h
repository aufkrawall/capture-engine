#pragma once

#include <atomic>
#include <cstdint>

namespace ce::dx12_overlay_policy {

// Whether an ExecuteCommandLists whose immediate caller is a third-party overlay is the overlay's own submission.
//
// It is only if the game's submissions reach CE without that overlay in between. An overlay hooking the same queue
// method above CE is the immediate caller of every game submission too, and treating those as foreign would leave the
// frame classification with no command lists at all. So a submission from an overlay counts as the overlay's only
// after a direct caller was seen since the module set last changed: an overlay loading later may have just hooked in
// above CE, and has to be proved again.
//
// Until 2026-11-04 the classifier read its own return address, which is CE's ECL detour, so no caller ever classified
// as an overlay and overlays' lists were counted as the game's (the function was never inlined; the PDB holds it).
class EclCallerGate {
public:
    // True: leave this submission out of the game's accounting.
    bool Observe(bool immediateCallerIsOverlay, uint64_t moduleSetGeneration) {
        const uint64_t generationState = moduleSetGeneration << 1;
        uint64_t state = state_.load(std::memory_order_relaxed);
        if ((state & ~kDirectCallerSeen) != generationState) {
            state = generationState;
        }
        if (!immediateCallerIsOverlay) {
            state |= kDirectCallerSeen;
        }
        state_.store(state, std::memory_order_relaxed);
        return immediateCallerIsOverlay && (state & kDirectCallerSeen) != 0;
    }

private:
    static constexpr uint64_t kDirectCallerSeen = 1;
    std::atomic<uint64_t> state_{0};
};

}  // namespace ce::dx12_overlay_policy
