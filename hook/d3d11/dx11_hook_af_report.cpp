// Raw-context forced AF: the sampler policy snapshot, the cost report of both
// forced-AF paths and the flow-test exports. The state machine itself is the
// engine shared with CE's context wrapper (d3d11_af_engine.h, dx11_hook_af_raw.cpp).

#include "dx11_hook_internal.h"

#include "d3d11_af_replacements.h"
#include "hook/metrics/hook_cpu_cost.h"
#include "hook/runtime/hook_clock.h"

SamplerPolicySnapshot11 AcquireSamplerPolicy11() {
    // The hash belongs to the thread's cached config itself, recomputed only when it refreshes.
    const SamplerConfigSnapshot config = GetActiveSamplerConfigCached();
    EnsureSamplerCacheFreshForHash11(config.hash);
    return {config.gfx, dx11_hook_g_SamplerCacheEpoch11.load(std::memory_order_acquire), config.hash};
}

uint64_t DX11Hook_ReconciledSamplerSlots() {
    return RawAF_SumCost(nullptr).reconciledSlots + ce::d3d11_af::SumRegisteredStates(nullptr).reconciledSlots;
}

// Hook service thread: one line per 10 s window and path in which forced AF did any
// work - the raw context hooks and CE's context wrapper, counted by the same engine
// (d3d11_af_engine.h). Sampled passes give the CPU estimate; the game thread itself
// never formats or logs.
void DX11Hook_ReportSamplerCostIfDue() {
    static constexpr uint64_t kWindowMs = 10'000;
    static uint64_t s_windowStartMs = 0;
    static ce::d3d11_af::CostSnapshot s_reportedRaw;
    static ce::d3d11_af::CostSnapshot s_reportedWrapper;
    const uint64_t now = ce::hook_clock::TickCount64();
    if (s_windowStartMs == 0) {
        s_windowStartMs = now;
        return;
    }
    const uint64_t elapsedMs = now - s_windowStartMs;
    if (elapsedMs < kWindowMs) {
        return;
    }
    s_windowStartMs = now;
    size_t rawContexts = 0;
    const ce::d3d11_af::CostSnapshot raw = RawAF_SumCost(&rawContexts);
    size_t wrapperContexts = 0;
    const ce::d3d11_af::CostSnapshot wrapper = ce::d3d11_af::SumRegisteredStates(&wrapperContexts);
    ce::d3d11_af::LogCostWindow("[D3D11 AF COST]", raw.Since(s_reportedRaw), elapsedMs, rawContexts);
    ce::d3d11_af::LogCostWindow("[D3D11 AF COST wrapper]", wrapper.Since(s_reportedWrapper), elapsedMs,
                                wrapperContexts);
    s_reportedRaw = raw;
    s_reportedWrapper = wrapper;

    // `entries` counts the game samplers holding CE replacements: it follows the game's own live
    // samplers and never climbs toward the device's 4096 (20261017_135632).
    static ce::d3d11_af::ReplacementStats s_reportedReplacements;
    const ce::d3d11_af::ReplacementStats replacements = ce::d3d11_af::SnapshotReplacementStats();
    if (replacements.created != s_reportedReplacements.created ||
        replacements.hits != s_reportedReplacements.hits ||
        replacements.createFailures != s_reportedReplacements.createFailures ||
        replacements.registerFailures != s_reportedReplacements.registerFailures) {
        HookLog("[D3D11 AF REPLACEMENTS] window=%llums created=%llu reused=%llu createFailed=%llu "
                "registerFailed=%llu entries=%lld peak=%lld",
                static_cast<unsigned long long>(elapsedMs),
                static_cast<unsigned long long>(replacements.created - s_reportedReplacements.created),
                static_cast<unsigned long long>(replacements.hits - s_reportedReplacements.hits),
                static_cast<unsigned long long>(replacements.createFailures - s_reportedReplacements.createFailures),
                static_cast<unsigned long long>(replacements.registerFailures -
                                                s_reportedReplacements.registerFailures),
                static_cast<long long>(replacements.entries), static_cast<long long>(replacements.peakEntries));
        s_reportedReplacements = replacements;
    }
    // Game sampler creations: BioShock Infinite creates one on almost every draw while textures
    // stream in; tagKept counts the creations that returned an existing, already tagged object.
    // memo: creations whose policy result (desc) and kept tag (tag) came from the creating
    // thread's memo, overlapping tag writes that kept one out of it (raced), threads with a memo.
    // ceUs is CE's own work per creation (policy, tag check) around the runtime's call (runtimeUs),
    // from 1 creation in 32; estMsPerS is CE's share on the creating threads.
    static ce::d3d11_af::SamplerCreationStats s_reportedCreates;
    const ce::d3d11_af::SamplerCreationStats creates = ce::d3d11_af::SnapshotSamplerCreationStats();
    if (creates.creates != s_reportedCreates.creates) {
        const uint64_t count = creates.creates - s_reportedCreates.creates;
        const uint64_t timed = creates.timedCreates - s_reportedCreates.timedCreates;
        const double scale = timed != 0 ? HookQpcTicksToUs() / static_cast<double>(timed) : 0.0;
        const double ceUs = static_cast<double>(creates.timedCeTicks - s_reportedCreates.timedCeTicks) * scale;
        const double runtimeUs =
            static_cast<double>(creates.timedRuntimeTicks - s_reportedCreates.timedRuntimeTicks) * scale;
        const auto delta = [](uint64_t now, uint64_t before) { return static_cast<unsigned long long>(now - before); };
        HookLog("[D3D11 SAMPLER CREATES] window=%llums creates=%llu tagWritten=%llu tagKept=%llu "
                "memo(desc=%llu tag=%llu raced=%llu threads=%zu) ceUs=%.2f runtimeUs=%.2f(sampled=%llu) "
                "estMsPerS=%.2f",
                static_cast<unsigned long long>(elapsedMs), static_cast<unsigned long long>(count),
                delta(creates.tagsWritten, s_reportedCreates.tagsWritten),
                delta(creates.tagsKept, s_reportedCreates.tagsKept),
                delta(creates.descHits, s_reportedCreates.descHits),
                delta(creates.tagSkips, s_reportedCreates.tagSkips), delta(creates.raced, s_reportedCreates.raced),
                creates.threads, ceUs, runtimeUs, static_cast<unsigned long long>(timed),
                ceUs * static_cast<double>(count) / static_cast<double>(elapsedMs));
        s_reportedCreates = creates;
    }
}

#ifdef CE_FLOW_TEST

// A D3D11-to-D3D12 translator's frontend reaches CE as a raw device whose
// context CE never wraps. The flow process's own D3D11CreateDevice resolves to
// CE's wrapper, so this hands back the real objects, hooked the way a created
// device's are, after resolving the config so the sampler gates are current.
extern "C" __declspec(dllexport) bool CEFlow_InstallRawD3D11Hooks(ID3D11Device* device, ID3D11Device** realDevice,
                                                                  ID3D11DeviceContext** realContext) {
    if (!device || !realDevice || !realContext) {
        return false;
    }
    (void)GetActiveGraphicsConfig();
    ID3D11Device* real = DeWrap(device);
    real->AddRef();
    ID3D11DeviceContext* context = nullptr;
    real->GetImmediateContext(&context);
    InstallVTableHooks(real, context, nullptr);
    *realDevice = real;
    *realContext = context;
    return context != nullptr;
}

extern "C" __declspec(dllexport) void CEFlow_RefreshGraphicsConfig() {
    (void)GetActiveGraphicsConfig();
}

namespace {
// Values in CEFlowD3D11SamplerCost order (tests/flow/flow_api.h); returns how many were written.
uint32_t WriteFlowSamplerCost(const ce::d3d11_af::CostSnapshot& total, uint64_t* values, uint32_t count) {
    const uint64_t ordered[] = {
        total.srvBinds,       total.samplerBinds,    total.passes,         total.fastSlots,
        total.slowSlots,      total.viewClasses,     total.rebinds,        total.choiceEvictions,
        total.shaderBinds,    total.timedBinds,      total.timedShaderBinds,
        total.shaderRepeats,  total.shaderMemoHits,  total.shaderLookups,  total.shaderEvictions,
        total.reconciledSlots, total.timedBindSlots, total.timedBindCalls,
    };
    constexpr uint32_t kValues = static_cast<uint32_t>(sizeof(ordered) / sizeof(ordered[0]));
    const uint32_t written = values ? (count < kValues ? count : kValues) : 0u;
    for (uint32_t i = 0; i < written; ++i) {
        values[i] = ordered[i];
    }
    return written;
}
}  // namespace

extern "C" __declspec(dllexport) uint32_t CEFlow_GetD3D11SamplerCost(uint64_t* values, uint32_t count) {
    return WriteFlowSamplerCost(DX11Hook_SamplerCostTotalsForTesting(), values, count);
}

// The same counters for CE's context wrapper, from the same engine.
extern "C" __declspec(dllexport) uint32_t CEFlow_GetD3D11WrapperSamplerCost(uint64_t* values, uint32_t count) {
    return WriteFlowSamplerCost(ce::d3d11_af::SumRegisteredStates(nullptr), values, count);
}

// Values in CEFlowD3D11SamplerCreationStats order (tests/flow/flow_api.h); returns how many were written.
extern "C" __declspec(dllexport) uint32_t CEFlow_GetD3D11SamplerCreationStats(uint64_t* values, uint32_t count) {
    const ce::d3d11_af::SamplerCreationStats stats = ce::d3d11_af::SnapshotSamplerCreationStats();
    const uint64_t ordered[] = {stats.creates,  stats.tagsWritten, stats.tagsKept, stats.timedCreates,
                                stats.descHits, stats.tagSkips,    stats.raced};
    constexpr uint32_t kValues = static_cast<uint32_t>(sizeof(ordered) / sizeof(ordered[0]));
    const uint32_t written = values ? (count < kValues ? count : kValues) : 0u;
    for (uint32_t i = 0; i < written; ++i) {
        values[i] = ordered[i];
    }
    return written;
}

// Values in CEFlowD3D11ReplacementStats order (tests/flow/flow_api.h); returns how many were written.
extern "C" __declspec(dllexport) uint32_t CEFlow_GetD3D11ReplacementStats(uint64_t* values, uint32_t count) {
    const ce::d3d11_af::ReplacementStats stats = ce::d3d11_af::SnapshotReplacementStats();
    const uint64_t ordered[] = {stats.created, stats.hits, stats.createFailures, stats.registerFailures,
                                static_cast<uint64_t>(stats.entries)};
    constexpr uint32_t kValues = static_cast<uint32_t>(sizeof(ordered) / sizeof(ordered[0]));
    const uint32_t written = values ? (count < kValues ? count : kValues) : 0u;
    for (uint32_t i = 0; i < written; ++i) {
        values[i] = ordered[i];
    }
    return written;
}

// What the real context has bound in a pixel sampler slot. The game-facing PSGetSamplers
// returns the game's own sampler; tests need the physical one CE bound.
extern "C" __declspec(dllexport) bool CEFlow_GetPhysicalPixelSampler(ID3D11DeviceContext* context, UINT slot,
                                                                     ID3D11SamplerState** sampler) {
    if (!context || !sampler) {
        return false;
    }
    *sampler = nullptr;
    ID3D11DeviceContext* real = context;
    void* wrapper = nullptr;
    if (SUCCEEDED(context->QueryInterface(IID_CWrapD3D11DeviceContext, &wrapper)) && wrapper) {
        real = static_cast<CWrapD3D11DeviceContext*>(wrapper)->GetReal();
        static_cast<CWrapD3D11DeviceContext*>(wrapper)->Release();
    }
    const auto original = ResolveContextOriginal11(real, ce::d3d11_slots::kPSGetSamplers,
                                                   &D3D11ContextVTableOriginals::psGetSamplers,
                                                   dx11_hook_oPSGetSamplers11);
    if (!original) {
        real->PSGetSamplers(slot, 1, sampler);
        return true;
    }
    original(real, slot, 1, sampler);
    return true;
}
#endif
