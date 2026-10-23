#pragma once

// Present provenance: which module issued a Present CE intercepted, and the [PRESENT ORIGIN] record of it.

#include <windows.h>

#include <cstdint>

#include "present_originator_policy.h"

struct IDXGISwapChain;

namespace DXGIShared {

struct PresentOrigin {
    static constexpr int kMaxModules = 8;
    static constexpr int kModuleNameLength = 32;

    ce::present_originator::Originator originator = ce::present_originator::Originator::kUnresolved;
    bool fromStreamline = false;
    bool fromFFXFrameGeneration = false;
    bool fromXeFG = false;
    // Resolved from the frames above a foreign chain; otherwise it is the immediate caller.
    bool walked = false;
    char module[MAX_PATH] = {};  // the originator's path, empty when unresolved
    // Base names of the modules walked up to and including the originator (consecutive repeats folded).
    uint8_t moduleCount = 0;
    char modules[kMaxModules][kModuleNameLength] = {};
};

// Below a foreign Present chain the immediate caller is always the last foreign overlay in it, so the
// originator is resolved from the frames above them: past CE's own frames, the tracked overlays and the
// system's dispatch modules. Bounded walk, one pass - it runs on the Present hot path.
void ResolvePresentOriginatorBelowForeignChain(PresentOrigin* out);

// The same classification for the immediate caller (CE above or without a foreign chain).
void ClassifyPresentCallerOrigin(const void* callerAddress, PresentOrigin* out);

// [PRESENT ORIGIN]: one line per (swapchain, thread, Present/Present1) stream whenever its originator changes,
// with the repeats in between, plus a summary of every originator kind each 10 s.
void NotePresentOrigin(IDXGISwapChain* swapchain, bool present1, const PresentOrigin& origin);

}  // namespace DXGIShared
