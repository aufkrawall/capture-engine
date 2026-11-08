#pragma once

#include <cstdint>
#include <utility>

// When a physical Present no overlay route drew on may still count as covered. Header-only for the unit tests.

#include "hook/present/present_originator_policy.h"

namespace ce::dx12_overlay_policy {

// FG-composed inheritance: a runtime that composes its outputs from frames CE drew on (DLSS-G from the proxy
// back buffer CE drew into on the game's Present, FSR FG from the UI resource CE composited) shows the overlay
// on outputs CE never touched. That holds only for that runtime's own presents. A present another component
// issued - XeFG's presenter thread, the application - shows no overlay unless a route drew on it: Talos +
// OptiScaler XeFG (20261023_183119) counted every XeFG-thread present as covered (`inheritIfNoDraw=1 covered=1`)
// while CE drew on half of the displayed frames. An unresolved originator (CE's own wrapper presenting, no
// candidate frame) keeps the runtime's inheritance as before.
inline bool ShouldInheritFGComposedCoverage(ce::present_originator::Originator present) {
    using ce::present_originator::Originator;
    return present == Originator::kUnresolved || present == Originator::kStreamline ||
           present == Originator::kFFXFrameGeneration;
}

// The callback and physical Present run on the same presenter thread. Carry
// the output identity across a configure edge, then expire it at that Present.
class FFXCallbackOutputCoverage {
public:
    void Stage(uintptr_t output) { pendingOutput_ = output; }
    void BeginPresent() { currentOutput_ = std::exchange(pendingOutput_, 0); }
    void EndPresent() { currentOutput_ = 0; }
    bool Covers(uintptr_t output) const { return output != 0 && output == currentOutput_; }

private:
    uintptr_t pendingOutput_ = 0;
    uintptr_t currentOutput_ = 0;
};

}  // namespace ce::dx12_overlay_policy
