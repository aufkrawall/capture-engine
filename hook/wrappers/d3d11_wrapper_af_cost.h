#pragma once

// Forced-AF work in CE's D3D11 context wrapper (the path a game takes when its
// device is created through CE's D3D11CreateDevice wrapper, e.g. BioShock
// Infinite). The raw-context path has its own counters in D3D11PerContextState.
//
// Counts go into thread-local batches flushed to process totals every 1024
// events, so the game's thread does no atomic per call; a 10 s report may lag
// by at most one batch per thread.

#include <cstdint>

namespace ce::wrapper_af_cost {

enum Counter : unsigned {
    kSrvBinds,        // pixel SRV bind calls that changed a slot
    kSamplerBinds,    // pixel sampler bind calls
    kShaderBinds,     // pixel shader changes
    kPasses,          // ReconcileSamplers calls
    kResolves,        // slots resolved (ResolveForcedAFSampler)
    kVariantLookups,  // sampler-owned variant lookups (after a slot's sampler changed)
    kRebinds,         // physical sampler range binds CE issued
    kTimedWork,       // sampled pass/bind durations
    kTimedWorkTicks,
    kCount,
};

void Add(Counter counter, uint64_t amount = 1);

// 1 call in 32 starts a QPC sample; Finish adds it to kTimedWork/kTimedWorkTicks.
class Sample {
public:
    Sample();
    ~Sample();
    Sample(const Sample&) = delete;
    Sample& operator=(const Sample&) = delete;

private:
    int64_t startTicks_ = 0;
};

// Hook service thread: one line when the window had wrapper AF activity.
void LogWindowIfActive(uint64_t elapsedMs);

}  // namespace ce::wrapper_af_cost
