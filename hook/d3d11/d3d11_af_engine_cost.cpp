// The shared D3D11 forced-AF engine: cost snapshots, the registry of wrapper
// contexts the hook thread sums, and the one cost-line format both paths log.

#include "d3d11_af_engine.h"

#include <mutex>
#include <vector>

#include "hook/metrics/hook_cpu_cost.h"
#include "hook/runtime/hook_common.h"

namespace ce::d3d11_af {

namespace {
std::mutex g_reportedStatesMutex;
std::vector<const PixelState*> g_reportedStates;
CostSnapshot g_retiredCost;  // counts of unregistered states
}  // namespace

void CostSnapshot::Add(const CostSnapshot& other) {
    srvBinds += other.srvBinds;
    samplerBinds += other.samplerBinds;
    shaderBinds += other.shaderBinds;
    passes += other.passes;
    fastSlots += other.fastSlots;
    slowSlots += other.slowSlots;
    viewClasses += other.viewClasses;
    rebinds += other.rebinds;
    timedPasses += other.timedPasses;
    timedTicks += other.timedTicks;
    timedBinds += other.timedBinds;
    timedBindTicks += other.timedBindTicks;
    timedShaderBinds += other.timedShaderBinds;
    timedShaderTicks += other.timedShaderTicks;
    choiceEvictions += other.choiceEvictions;
    shaderRepeats += other.shaderRepeats;
    shaderMemoHits += other.shaderMemoHits;
    shaderLookups += other.shaderLookups;
    shaderEvictions += other.shaderEvictions;
}

CostSnapshot CostSnapshot::Since(const CostSnapshot& earlier) const {
    CostSnapshot window = *this;
    window.srvBinds -= earlier.srvBinds;
    window.samplerBinds -= earlier.samplerBinds;
    window.shaderBinds -= earlier.shaderBinds;
    window.passes -= earlier.passes;
    window.fastSlots -= earlier.fastSlots;
    window.slowSlots -= earlier.slowSlots;
    window.viewClasses -= earlier.viewClasses;
    window.rebinds -= earlier.rebinds;
    window.timedPasses -= earlier.timedPasses;
    window.timedTicks -= earlier.timedTicks;
    window.timedBinds -= earlier.timedBinds;
    window.timedBindTicks -= earlier.timedBindTicks;
    window.timedShaderBinds -= earlier.timedShaderBinds;
    window.timedShaderTicks -= earlier.timedShaderTicks;
    window.choiceEvictions -= earlier.choiceEvictions;
    window.shaderRepeats -= earlier.shaderRepeats;
    window.shaderMemoHits -= earlier.shaderMemoHits;
    window.shaderLookups -= earlier.shaderLookups;
    window.shaderEvictions -= earlier.shaderEvictions;
    return window;
}

CostSnapshot CostCounters::Snapshot() const {
    CostSnapshot snapshot;
    snapshot.srvBinds = srvBinds.Load();
    snapshot.samplerBinds = samplerBinds.Load();
    snapshot.shaderBinds = shaderBinds.Load();
    snapshot.passes = passes.Load();
    snapshot.fastSlots = fastSlots.Load();
    snapshot.slowSlots = slowSlots.Load();
    snapshot.viewClasses = viewClasses.Load();
    snapshot.rebinds = rebinds.Load();
    snapshot.timedPasses = timedPasses.Load();
    snapshot.timedTicks = timedTicks.Load();
    snapshot.timedBinds = timedBinds.Load();
    snapshot.timedBindTicks = timedBindTicks.Load();
    snapshot.timedShaderBinds = timedShaderBinds.Load();
    snapshot.timedShaderTicks = timedShaderTicks.Load();
    snapshot.choiceEvictions = choiceEvictions.Load();
    snapshot.shaderRepeats = shaderRepeats.Load();
    snapshot.shaderMemoHits = shaderMemoHits.Load();
    snapshot.shaderLookups = shaderLookups.Load();
    snapshot.shaderEvictions = shaderEvictions.Load();
    return snapshot;
}

void RegisterReportedState(const PixelState* state) {
    if (!state) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_reportedStatesMutex);
    try {
        g_reportedStates.push_back(state);
    } catch (...) {
        // Unreported is harmless: the state works the same, its counts are just not summed.
    }
}

void UnregisterReportedState(const PixelState* state) {
    if (!state) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_reportedStatesMutex);
    for (size_t i = 0; i < g_reportedStates.size(); ++i) {
        if (g_reportedStates[i] == state) {
            g_retiredCost.Add(state->cost.Snapshot());
            g_reportedStates[i] = g_reportedStates.back();
            g_reportedStates.pop_back();
            return;
        }
    }
}

CostSnapshot SumRegisteredStates(size_t* contexts) {
    std::lock_guard<std::mutex> lock(g_reportedStatesMutex);
    CostSnapshot total = g_retiredCost;
    for (const PixelState* state : g_reportedStates) {
        total.Add(state->cost.Snapshot());
    }
    if (contexts) {
        *contexts = g_reportedStates.size();
    }
    return total;
}

void LogCostWindow(const char* tag, const CostSnapshot& window, uint64_t elapsedMs, size_t contexts) {
    if (!window.Any() || elapsedMs == 0) {
        return;
    }
    const auto meanUs = [](uint64_t ticks, uint64_t samples) {
        return samples != 0 ? static_cast<double>(ticks) * HookQpcTicksToUs() / static_cast<double>(samples) : 0.0;
    };
    const double passUs = meanUs(window.timedTicks, window.timedPasses);
    // SRV and sampler binds share one sample stream: their tracking work, outside the
    // reconciliation passes a sampler bind or draw then runs (timed separately). Shader binds
    // are timed on their own, from before the metadata lookup (until 2026-10-17 the lookup ran
    // outside every sample).
    const double bindUs = meanUs(window.timedBindTicks, window.timedBinds);
    const double shaderUs = meanUs(window.timedShaderTicks, window.timedShaderBinds);
    const double seconds = static_cast<double>(elapsedMs) / 1000.0;
    const double passMsPerS = passUs * static_cast<double>(window.passes) / seconds / 1000.0;
    const double bindMsPerS = bindUs * static_cast<double>(window.srvBinds + window.samplerBinds) / seconds / 1000.0;
    const double shaderMsPerS = shaderUs * static_cast<double>(window.shaderBinds) / seconds / 1000.0;
    const auto n = [](uint64_t value) { return static_cast<unsigned long long>(value); };
    const auto perS = [seconds](uint64_t value) { return static_cast<double>(value) / seconds; };
    // shaderSwitches: how shader binds resolved (the tracked one again, the context's shader memo,
    // a private-data lookup) and memo evictions; lookups climbing with evictions means the
    // game's shader working set outgrew the memo.
    HookLogImportant(
        "%s window=%llums contexts=%zu binds/s(srv=%.0f sampler=%.0f shader=%.0f) passes/s=%.0f "
        "slots(fast=%llu slow=%llu) viewClasses=%llu rebinds=%llu evictions=%llu "
        "shaderSwitches(repeat=%llu memo=%llu lookup=%llu evicted=%llu) passUs=%.2f(sampled=%llu) "
        "bindUs=%.2f(sampled=%llu) shaderUs=%.2f(sampled=%llu) estMsPerS=%.2f(passes=%.2f binds=%.2f "
        "shaders=%.2f)",
        tag, n(elapsedMs), contexts, perS(window.srvBinds), perS(window.samplerBinds), perS(window.shaderBinds),
        perS(window.passes), n(window.fastSlots), n(window.slowSlots), n(window.viewClasses), n(window.rebinds),
        n(window.choiceEvictions), n(window.shaderRepeats), n(window.shaderMemoHits), n(window.shaderLookups),
        n(window.shaderEvictions), passUs, n(window.timedPasses), bindUs, n(window.timedBinds), shaderUs,
        n(window.timedShaderBinds), passMsPerS + bindMsPerS + shaderMsPerS, passMsPerS, bindMsPerS, shaderMsPerS);
}

}  // namespace ce::d3d11_af
