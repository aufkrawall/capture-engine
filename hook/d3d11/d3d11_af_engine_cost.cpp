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

namespace {
// Every counter, once: what Add, Since and Snapshot walk.
struct CountField {
    uint64_t CostSnapshot::*snapshot;
    Count CostCounters::*counter;
};
constexpr CountField kCountFields[] = {
    {&CostSnapshot::srvBinds, &CostCounters::srvBinds},
    {&CostSnapshot::samplerBinds, &CostCounters::samplerBinds},
    {&CostSnapshot::shaderBinds, &CostCounters::shaderBinds},
    {&CostSnapshot::passes, &CostCounters::passes},
    {&CostSnapshot::fastSlots, &CostCounters::fastSlots},
    {&CostSnapshot::slowSlots, &CostCounters::slowSlots},
    {&CostSnapshot::viewClasses, &CostCounters::viewClasses},
    {&CostSnapshot::rebinds, &CostCounters::rebinds},
    {&CostSnapshot::timedPasses, &CostCounters::timedPasses},
    {&CostSnapshot::timedTicks, &CostCounters::timedTicks},
    {&CostSnapshot::timedBinds, &CostCounters::timedBinds},
    {&CostSnapshot::timedBindTicks, &CostCounters::timedBindTicks},
    {&CostSnapshot::timedShaderBinds, &CostCounters::timedShaderBinds},
    {&CostSnapshot::timedShaderTicks, &CostCounters::timedShaderTicks},
    {&CostSnapshot::choiceEvictions, &CostCounters::choiceEvictions},
    {&CostSnapshot::shaderRepeats, &CostCounters::shaderRepeats},
    {&CostSnapshot::shaderMemoHits, &CostCounters::shaderMemoHits},
    {&CostSnapshot::shaderLookups, &CostCounters::shaderLookups},
    {&CostSnapshot::shaderEvictions, &CostCounters::shaderEvictions},
    {&CostSnapshot::reconciledSlots, &CostCounters::reconciledSlots},
    {&CostSnapshot::timedBindSlots, &CostCounters::timedBindSlots},
    {&CostSnapshot::timedBindCalls, &CostCounters::timedBindCalls},
    {&CostSnapshot::eagerPasses, &CostCounters::eagerPasses},
    {&CostSnapshot::eagerGroups, &CostCounters::eagerGroups},
    {&CostSnapshot::eagerOverlaps, &CostCounters::eagerOverlaps},
    {&CostSnapshot::supersededSlots, &CostCounters::supersededSlots},
    {&CostSnapshot::deferredBinds, &CostCounters::deferredBinds},
};
constexpr size_t kCountFieldCount = sizeof(kCountFields) / sizeof(kCountFields[0]);
static_assert(kCountFieldCount * sizeof(Count) == sizeof(CostCounters), "a counter is missing from kCountFields");
static_assert(kCountFieldCount * sizeof(uint64_t) == sizeof(CostSnapshot), "a snapshot field is missing");
}  // namespace

void CostSnapshot::Add(const CostSnapshot& other) {
    for (const CountField& field : kCountFields) {
        this->*field.snapshot += other.*field.snapshot;
    }
}

CostSnapshot CostSnapshot::Since(const CostSnapshot& earlier) const {
    CostSnapshot window;
    for (const CountField& field : kCountFields) {
        window.*field.snapshot = CountDelta(this->*field.snapshot, earlier.*field.snapshot);
    }
    return window;
}

CostSnapshot CostCounters::Snapshot() const {
    CostSnapshot snapshot;
    for (const CountField& field : kCountFields) {
        snapshot.*field.snapshot = (this->*field.counter).Load();
    }
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
    // game's shader working set outgrew the memo. slotsPerCall: physical slots per ranged
    // PSSetSamplers call CE made, from the sampled passes.
    const double slotsPerCall = window.timedBindCalls != 0 ? static_cast<double>(window.timedBindSlots) /
                                                                 static_cast<double>(window.timedBindCalls)
                                                           : 0.0;
    // deferral: sampler-bind passes, the draw intervals they fell in (groups) and the draws that
    // reconciled again anyway (overlaps), the slots a later pass redid before the draw, and the
    // passes reconciling only at the draw would save, priced at passUs (d3d11_af_engine.h).
    const uint64_t savable = window.eagerPasses + window.eagerOverlaps - window.eagerGroups;
    const double deferMsPerS = passUs * perS(savable) / 1000.0;
    HookLogImportant(
        "%s window=%llums contexts=%zu binds/s(srv=%.0f sampler=%.0f shader=%.0f) passes/s=%.0f "
        "slots(fast=%llu slow=%llu) viewClasses=%llu rebinds=%llu(slotsPerCall=%.2f) evictions=%llu "
        "shaderSwitches(repeat=%llu memo=%llu lookup=%llu evicted=%llu) "
        "deferral(deferred/s=%.0f eager/s=%.0f groups/s=%.0f overlaps/s=%.0f superseded/s=%.0f savable/s=%.0f "
        "msPerS=%.2f) "
        "passUs=%.2f(sampled=%llu) bindUs=%.2f(sampled=%llu) shaderUs=%.2f(sampled=%llu) "
        "estMsPerS=%.2f(passes=%.2f binds=%.2f shaders=%.2f)",
        tag, n(elapsedMs), contexts, perS(window.srvBinds), perS(window.samplerBinds), perS(window.shaderBinds),
        perS(window.passes), n(window.fastSlots), n(window.slowSlots), n(window.viewClasses), n(window.rebinds),
        slotsPerCall, n(window.choiceEvictions), n(window.shaderRepeats), n(window.shaderMemoHits),
        n(window.shaderLookups), n(window.shaderEvictions), perS(window.deferredBinds), perS(window.eagerPasses),
        perS(window.eagerGroups),
        perS(window.eagerOverlaps), perS(window.supersededSlots), perS(savable), deferMsPerS, passUs,
        n(window.timedPasses), bindUs, n(window.timedBinds), shaderUs, n(window.timedShaderBinds),
        passMsPerS + bindMsPerS + shaderMsPerS, passMsPerS, bindMsPerS, shaderMsPerS);
}

}  // namespace ce::d3d11_af
