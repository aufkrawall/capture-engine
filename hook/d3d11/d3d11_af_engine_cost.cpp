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
    choiceEvictions += other.choiceEvictions;
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
    window.choiceEvictions -= earlier.choiceEvictions;
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
    snapshot.choiceEvictions = choiceEvictions.Load();
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
    // The three bind hooks share one sample stream: their tracking work, outside the
    // reconciliation passes a sampler bind or draw then runs (timed separately).
    const double bindUs = meanUs(window.timedBindTicks, window.timedBinds);
    const double seconds = static_cast<double>(elapsedMs) / 1000.0;
    const double passMsPerS = passUs * static_cast<double>(window.passes) / seconds / 1000.0;
    const double bindMsPerS =
        bindUs * static_cast<double>(window.srvBinds + window.samplerBinds + window.shaderBinds) / seconds / 1000.0;
    HookLogImportant(
        "%s window=%llums contexts=%zu binds/s(srv=%.0f sampler=%.0f shader=%.0f) passes/s=%.0f "
        "slots(fast=%llu slow=%llu) viewClasses=%llu rebinds=%llu evictions=%llu passUs=%.2f(sampled=%llu) "
        "bindUs=%.2f(sampled=%llu) estMsPerS=%.2f(passes=%.2f binds=%.2f)",
        tag, static_cast<unsigned long long>(elapsedMs), contexts, static_cast<double>(window.srvBinds) / seconds,
        static_cast<double>(window.samplerBinds) / seconds, static_cast<double>(window.shaderBinds) / seconds,
        static_cast<double>(window.passes) / seconds, static_cast<unsigned long long>(window.fastSlots),
        static_cast<unsigned long long>(window.slowSlots), static_cast<unsigned long long>(window.viewClasses),
        static_cast<unsigned long long>(window.rebinds), static_cast<unsigned long long>(window.choiceEvictions),
        passUs, static_cast<unsigned long long>(window.timedPasses), bindUs,
        static_cast<unsigned long long>(window.timedBinds), passMsPerS + bindMsPerS, passMsPerS, bindMsPerS);
}

}  // namespace ce::d3d11_af
