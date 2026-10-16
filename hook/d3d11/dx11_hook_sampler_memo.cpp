// Raw-context forced AF: memoized pixel-sampler choices and the reconciliation
// that applies them.
//
// A dirty pixel slot needs the physical sampler for (logical sampler, AF
// verdict). The verdict follows from the tracked shader's plan and the AF class
// of each sampled view; the physical sampler for each verdict follows from the
// logical sampler's descriptor and the config. Both are immutable for a tracked
// object, so after one classification a slot resolves under the state lock that
// is already held, without COM queries, descriptor reads or cache scans. Every
// input the memo cannot vouch for (untracked shader, unseen view or verdict, a
// cleared replacement cache) takes the original classification path.

#include "dx11_hook_internal.h"

#include <bit>

#include "hook/metrics/hook_cpu_cost.h"
#include "hook/runtime/hook_clock.h"
#include "hook/wrappers/d3d11_wrapper_af_cost.h"

using ce::sampler_override::D3D11ForcedAFResourceDecision;

void ResetPixelSamplerChoice11(D3D11PixelSamplerChoice& choice) {
    for (ID3D11SamplerState*& physical : choice.physical) {
        if (physical) {
            physical->Release();
        }
    }
    choice = {};
}

namespace {

size_t SamplerChoiceHome11(const ID3D11SamplerState* logical) {
    // Sampler objects are heap allocations; drop the alignment bits before mixing.
    const uint64_t key = reinterpret_cast<uintptr_t>(logical) >> 4;
    return static_cast<size_t>((key ^ (key >> 7) ^ (key >> 17)) % D3D11SamplerChoiceCache::kEntries);
}

void ReleaseSamplerChoiceEntry11(D3D11SamplerChoiceCache::Entry& entry) {
    ResetPixelSamplerChoice11(entry.choice);
    if (entry.logical) {
        entry.logical->Release();
    }
    entry = {};
}

// The entry for `logical`, inserted (with its own reference) when absent.
D3D11SamplerChoiceCache::Entry& ClaimPixelSamplerChoiceLocked11(D3D11PerContextState& state,
                                                                ID3D11SamplerState* logical) {
    D3D11SamplerChoiceCache& cache = state.samplerChoices;
    const size_t home = SamplerChoiceHome11(logical);
    D3D11SamplerChoiceCache::Entry* victim = nullptr;
    for (size_t probe = 0; probe < D3D11SamplerChoiceCache::kProbe; ++probe) {
        auto& entry = cache.entries[(home + probe) % D3D11SamplerChoiceCache::kEntries];
        if (entry.logical == logical) {
            entry.stamp = ++cache.clock;
            return entry;
        }
        // An empty entry wins; otherwise the least recently used one in the window.
        if (!victim || (victim->logical && (!entry.logical || entry.stamp < victim->stamp))) {
            victim = &entry;
        }
    }
    if (victim->logical) {
        ++state.cost.choiceEvictions;
    }
    ReleaseSamplerChoiceEntry11(*victim);
    logical->AddRef();
    victim->logical = logical;
    victim->stamp = ++cache.clock;
    return *victim;
}

}  // namespace

const D3D11PixelSamplerChoice* FindPixelSamplerChoiceLocked11(D3D11SamplerChoiceCache& cache,
                                                              ID3D11SamplerState* logical) {
    const size_t home = SamplerChoiceHome11(logical);
    for (size_t probe = 0; probe < D3D11SamplerChoiceCache::kProbe; ++probe) {
        auto& entry = cache.entries[(home + probe) % D3D11SamplerChoiceCache::kEntries];
        if (entry.logical == logical) {
            entry.stamp = ++cache.clock;
            return &entry.choice;
        }
    }
    return nullptr;
}

void ReleaseSamplerChoiceCache11(D3D11SamplerChoiceCache& cache) {
    for (auto& entry : cache.entries) {
        ReleaseSamplerChoiceEntry11(entry);
    }
    cache.clock = 0;
}

D3D11BindCostSample11::D3D11BindCostSample11() {
    thread_local uint32_t t_binds = 0;
    if ((++t_binds & 31u) == 0) {
        startTicks_ = HookQpcTicks();
    }
}

void D3D11BindCostSample11::Finish(D3D11PerContextState& state) {
    if (startTicks_ != 0) {
        ++state.cost.timedBinds;
        state.cost.timedBindTicks += static_cast<uint64_t>(HookQpcTicks() - startTicks_);
        startTicks_ = 0;
    }
}

SamplerPolicySnapshot11 AcquireSamplerPolicy11() {
    // The version is read before the config: a config published in between
    // makes the cached hash look older than its content, costing one recompute.
    SharedMemoryLayout* memory = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    const uint32_t version = memory ? memory->configVersion.load(std::memory_order_acquire) : 0;
    const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();
    thread_local SharedMemoryLayout* t_memory = nullptr;
    thread_local uint32_t t_version = 0;
    thread_local uint64_t t_hash = 0;
    if (!memory || t_memory != memory || t_version != version) {
        t_hash = ce::sampler_override::HashSamplerOverrideConfig(gfx);
        t_memory = memory;
        t_version = version;
    }
    EnsureSamplerCacheFreshForHash11(t_hash);
    return {&gfx, dx11_hook_g_SamplerCacheEpoch11.load(std::memory_order_acquire)};
}

void NotePixelViewAFClass11(ID3D11DeviceContext* context, UINT textureSlot, ID3D11ShaderResourceView* view,
                            D3D11ForcedAFResourceDecision decision) {
    if (!context || !view || textureSlot >= D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT) {
        return;
    }
    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    auto it = dx11_hook_g_D3D11ContextStates.find(context);
    if (it == dx11_hook_g_D3D11ContextStates.end()) {
        return;
    }
    D3D11StageState& pixel = it->second.stages[GetStageIndex(D3D11ShaderStage::Pixel)];
    if (pixel.srvs[textureSlot] == view) {
        pixel.srvAFClass[textureSlot] = static_cast<uint8_t>(static_cast<int>(decision) + 1);
    }
}

void StorePixelSamplerChoice11(ID3D11DeviceContext* context, UINT slot, ID3D11SamplerState* logical, uint64_t epoch,
                               const D3D11PixelSamplerFacts11& facts, ID3D11SamplerState* physical) {
    if (!context || !logical || !facts.valid || slot >= D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT) {
        return;
    }
    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    auto it = dx11_hook_g_D3D11ContextStates.find(context);
    if (it == dx11_hook_g_D3D11ContextStates.end()) {
        return;
    }
    // The caller holds a reference to `logical`, so the facts still describe this object.
    (void)slot;
    D3D11PixelSamplerChoice& choice = ClaimPixelSamplerChoiceLocked11(it->second, logical).choice;
    if (choice.epoch != epoch) {
        ResetPixelSamplerChoice11(choice);
        choice.epoch = epoch;
        choice.isReplacement = facts.isReplacement;
        choice.descAllowsAF = facts.descAllowsAF;
    }
    const int verdict = facts.allowAF ? 1 : 0;
    if (choice.resolved[verdict]) {
        return;
    }
    choice.resolved[verdict] = true;
    if (physical && physical != logical) {
        physical->AddRef();
        choice.physical[verdict] = physical;
    }
}

namespace {

enum class PixelSlotResolution11 {
    Resolved,
    NeedsViewClasses,  // only the AF class of some sampled views is missing
    Slow,              // sampler, shader or view state must be classified or refreshed
};

constexpr uint8_t kAllowAFClass11 = static_cast<uint8_t>(static_cast<int>(D3D11ForcedAFResourceDecision::Allow) + 1);

// The AF verdict ShouldForceAnisotropyForStageSlot reaches for a pixel slot,
// from tracked state only. A view classified earlier keeps its class in its own
// private data; that one read is taken here, under the held lock, like the shader
// plan read at PSSetShader. Views never classified are reported in *unknownViews.
PixelSlotResolution11 TryPixelAFVerdictLocked11(D3D11PerContextState& state, UINT slot, bool* allow,
                                                ce::sampler_override::D3D11AFTextureMask* unknownViews) {
    if (!state.pixelShaderTracked) {
        return PixelSlotResolution11::Slow;
    }
    *allow = false;
    if (!state.pixelShader || !state.hasPixelShaderMetadata || !state.pixelShaderAFPlan.available) {
        return PixelSlotResolution11::Resolved;
    }
    const auto& samplerPlan = state.pixelShaderAFPlan.samplers[slot];
    if (samplerPlan.textures.Count() == 0 || !samplerPlan.SafeSample()) {
        return PixelSlotResolution11::Resolved;
    }
    D3D11StageState& pixel = state.stages[GetStageIndex(D3D11ShaderStage::Pixel)];
    bool missingClass = false;
    auto textures = samplerPlan.textures;
    for (UINT textureSlot = textures.TakeFirst(); textureSlot != UINT_MAX; textureSlot = textures.TakeFirst()) {
        if (!pixel.srvs[textureSlot]) {
            return PixelSlotResolution11::Slow;  // the original path refreshes the slot from the context
        }
        uint8_t afClass = pixel.srvAFClass[textureSlot];
        D3D11ForcedAFResourceDecision cached = D3D11ForcedAFResourceDecision::Allow;
        if (afClass == 0 && TryGetCachedWrapperForcedAFViewDecision(pixel.srvs[textureSlot], &cached)) {
            afClass = static_cast<uint8_t>(static_cast<int>(cached) + 1);
            pixel.srvAFClass[textureSlot] = afClass;
            ++state.cost.viewClasses;
        }
        if (afClass == 0) {
            unknownViews->Add(textureSlot);
            missingClass = true;
        } else if (afClass != kAllowAFClass11 && !missingClass) {
            return PixelSlotResolution11::Resolved;  // a protected view decides before any later one
        }
    }
    if (missingClass) {
        return PixelSlotResolution11::NeedsViewClasses;
    }
    *allow = true;
    return PixelSlotResolution11::Resolved;
}

// The physical sampler for the slot's tracked logical sampler, borrowed.
PixelSlotResolution11 TryResolvePixelSlotLocked11(D3D11PerContextState& state, UINT slot, uint64_t epoch,
                                                  ID3D11SamplerState** physical,
                                                  ce::sampler_override::D3D11AFTextureMask* unknownViews) {
    const D3D11StageState& pixel = state.stages[GetStageIndex(D3D11ShaderStage::Pixel)];
    ID3D11SamplerState* logical = pixel.samplers[slot];
    if (!logical) {
        *physical = nullptr;
        return PixelSlotResolution11::Resolved;
    }
    const D3D11PixelSamplerChoice* found = FindPixelSamplerChoiceLocked11(state.samplerChoices, logical);
    if (!found || found->epoch != epoch) {
        return PixelSlotResolution11::Slow;
    }
    const D3D11PixelSamplerChoice& choice = *found;
    if (choice.isReplacement) {
        *physical = logical;
        return PixelSlotResolution11::Resolved;
    }
    bool allow = false;
    if (choice.descAllowsAF) {
        const PixelSlotResolution11 verdict = TryPixelAFVerdictLocked11(state, slot, &allow, unknownViews);
        if (verdict != PixelSlotResolution11::Resolved) {
            return verdict;
        }
    }
    const int verdict = allow ? 1 : 0;
    if (!choice.resolved[verdict]) {
        return PixelSlotResolution11::Slow;
    }
    *physical = choice.physical[verdict] ? choice.physical[verdict] : logical;
    return PixelSlotResolution11::Resolved;
}

// Trivial on purpose: a pass touches only the entries it fills, so the arrays
// are never zeroed on the draw path.
struct PendingPixelBind11 {
    UINT slot;
    ID3D11SamplerState* physical;  // acquired
    bool remembered;               // realSamplers already holds it (set under the deciding lock)
};

struct PendingViewClass11 {
    UINT textureSlot;
    ID3D11ShaderResourceView* view;  // acquired
    D3D11ForcedAFResourceDecision decision;
};

struct PixelSlotBatch11 {
    PendingPixelBind11 binds[D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT];
    PendingViewClass11 views[D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT];
    int bindCount = 0;
    int viewCount = 0;
    uint32_t viewPendingMask = 0;
    uint32_t slowMask = 0;
};

// Resolves `slots` under the held state lock: queues a bind where the physical
// sampler differs, collects views whose class is missing (acquired, once per
// texture slot) when `collectViews`, and marks what only the original path can resolve.
void ResolvePixelSlotsLocked11(D3D11PerContextState& state, uint32_t slots, uint64_t epoch, bool bindEmptySlots,
                               bool collectViews, PixelSlotBatch11& batch) {
    D3D11StageState& pixel = state.stages[GetStageIndex(D3D11ShaderStage::Pixel)];
    ce::sampler_override::D3D11AFTextureMask collected;
    for (uint32_t pending = slots; pending != 0; pending &= pending - 1) {
        const UINT slot = static_cast<UINT>(std::countr_zero(pending));
        if (!pixel.samplers[slot] && !bindEmptySlots) {
            continue;
        }
        ID3D11SamplerState* physical = nullptr;
        ce::sampler_override::D3D11AFTextureMask unknownViews;
        const PixelSlotResolution11 resolution =
            TryResolvePixelSlotLocked11(state, slot, epoch, &physical, &unknownViews);
        if (resolution == PixelSlotResolution11::Slow ||
            (resolution == PixelSlotResolution11::NeedsViewClasses && !collectViews)) {
            batch.slowMask |= 1u << slot;
            continue;
        }
        if (resolution == PixelSlotResolution11::NeedsViewClasses) {
            batch.viewPendingMask |= 1u << slot;
            for (UINT textureSlot = unknownViews.TakeFirst(); textureSlot != UINT_MAX;
                 textureSlot = unknownViews.TakeFirst()) {
                if (collected.words[textureSlot / 64] & (uint64_t{1} << (textureSlot % 64))) {
                    continue;
                }
                collected.Add(textureSlot);
                pixel.srvs[textureSlot]->AddRef();
                batch.views[batch.viewCount++] = {textureSlot, pixel.srvs[textureSlot],
                                                  D3D11ForcedAFResourceDecision::UnsupportedViewDimension};
            }
            continue;
        }
        ++state.cost.fastSlots;
        ID3D11SamplerState*& remembered = pixel.realSamplers[slot];
        if (physical != remembered) {
            // Recorded before the bind outside the lock: only this context's own
            // thread binds its samplers, so no one observes the gap.
            if (physical) {
                physical->AddRef();
                physical->AddRef();  // one for realSamplers, one for the pending bind
            }
            if (remembered) {
                remembered->Release();
            }
            remembered = physical;
            ++state.cost.rebinds;
            batch.binds[batch.bindCount++] = {slot, physical, true};
        }
    }
}

}  // namespace

int ReconcilePixelSamplers11(SetSamplers11_t originalFn, ID3D11DeviceContext* context, uint32_t rangeMask,
                             bool bindEmptySlots, uint32_t* consumedMask) {
    if (consumedMask) {
        *consumedMask = 0;
    }
    if (!originalFn || !context || rangeMask == 0 || !g_D3D11SamplerOverridesActive.load(std::memory_order_acquire) ||
        dx11_hook_g_InOverlayRender) {
        return 0;
    }
    thread_local uint32_t t_passes = 0;
    const bool timed = (++t_passes & 31u) == 0;
    const int64_t startTicks = timed ? HookQpcTicks() : 0;
    const SamplerPolicySnapshot11 policy = AcquireSamplerPolicy11();

    PixelSlotBatch11 batch;
    uint32_t mask = 0;
    {
        std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
        auto it = dx11_hook_g_D3D11ContextStates.find(context);
        if (it == dx11_hook_g_D3D11ContextStates.end()) {
            return 0;
        }
        D3D11PerContextState& state = it->second;
        mask = state.pixelSamplerDirtyMask & rangeMask;
        if (mask == 0) {
            return 0;
        }
        state.pixelSamplerDirtyMask &= ~mask;
        if (state.pixelSamplerDirtyMask == 0) {
            dx11_hook_g_D3D11DirtyContextCount.fetch_sub(1, std::memory_order_release);
        }
        ++state.cost.passes;
        ResolvePixelSlotsLocked11(state, mask, policy.epoch, bindEmptySlots, true, batch);
    }
    if (batch.viewCount != 0) {
        // A view keeps its class in its own private data after the first
        // classification, so this is one private-data read per changed view.
        for (int i = 0; i < batch.viewCount; ++i) {
            batch.views[i].decision = ClassifyViewForForcedAF11(nullptr, batch.views[i].view);
        }
        {
            std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
            auto it = dx11_hook_g_D3D11ContextStates.find(context);
            if (it == dx11_hook_g_D3D11ContextStates.end()) {
                batch.slowMask |= batch.viewPendingMask;
            } else {
                D3D11PerContextState& state = it->second;
                D3D11StageState& pixel = state.stages[GetStageIndex(D3D11ShaderStage::Pixel)];
                for (int i = 0; i < batch.viewCount; ++i) {
                    const PendingViewClass11& view = batch.views[i];
                    if (pixel.srvs[view.textureSlot] == view.view) {
                        pixel.srvAFClass[view.textureSlot] = static_cast<uint8_t>(static_cast<int>(view.decision) + 1);
                    }
                }
                state.cost.viewClasses += static_cast<uint64_t>(batch.viewCount);
                ResolvePixelSlotsLocked11(state, batch.viewPendingMask, policy.epoch, bindEmptySlots, false, batch);
            }
        }
        for (int i = 0; i < batch.viewCount; ++i) {
            batch.views[i].view->Release();
        }
    }
    PendingPixelBind11* const binds = batch.binds;
    int& bindCount = batch.bindCount;
    const uint32_t slowMask = batch.slowMask;
    if (consumedMask) {
        *consumedMask = mask;
    }
    dx11_hook_g_DiagSamplerReconcileSlots.fetch_add(std::popcount(mask), std::memory_order_relaxed);

    // Classification calls into the runtime and may create a sampler through a
    // hooked device; it never runs under the state lock.
    int slowSlots = 0;
    for (uint32_t pending = slowMask; pending != 0; pending &= pending - 1) {
        const UINT slot = static_cast<UINT>(std::countr_zero(pending));
        ID3D11SamplerState* logical = GetTrackedSampler11(context, D3D11ShaderStage::Pixel, slot);
        if (!logical && !bindEmptySlots) {
            continue;
        }
        ++slowSlots;
        ID3D11SamplerState* physical = nullptr;
        if (logical) {
            D3D11PixelSamplerFacts11 facts;
            physical = GetOrCreateReplacementSampler11(context, D3D11ShaderStage::Pixel, slot, logical, &facts);
            StorePixelSamplerChoice11(context, slot, logical, policy.epoch, facts, physical);
        }
        ID3D11SamplerState* remembered = GetRememberedRealSampler11(context, D3D11ShaderStage::Pixel, slot);
        if (remembered != physical) {
            if (physical) {
                physical->AddRef();
            }
            binds[bindCount++] = {slot, physical, false};
        }
        if (remembered) {
            remembered->Release();
        }
        if (logical) {
            logical->Release();
        }
    }

    int unrememberedBinds = 0;
    for (int i = 0; i < bindCount; ++i) {
        originalFn(context, binds[i].slot, 1, &binds[i].physical);
        unrememberedBinds += binds[i].remembered ? 0 : 1;
    }
    if (unrememberedBinds != 0 || slowSlots != 0 || timed) {
        std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
        auto it = dx11_hook_g_D3D11ContextStates.find(context);
        if (it != dx11_hook_g_D3D11ContextStates.end()) {
            D3D11PerContextState& state = it->second;
            D3D11StageState& pixel = state.stages[GetStageIndex(D3D11ShaderStage::Pixel)];
            for (int i = 0; i < bindCount; ++i) {
                if (binds[i].remembered) {
                    continue;
                }
                ID3D11SamplerState*& tracked = pixel.realSamplers[binds[i].slot];
                if (tracked != binds[i].physical) {
                    if (binds[i].physical) {
                        binds[i].physical->AddRef();
                    }
                    if (tracked) {
                        tracked->Release();
                    }
                    tracked = binds[i].physical;
                }
            }
            state.cost.slowSlots += static_cast<uint64_t>(slowSlots);
            state.cost.rebinds += static_cast<uint64_t>(unrememberedBinds);
            if (timed) {
                ++state.cost.timedPasses;
                state.cost.timedTicks += static_cast<uint64_t>(HookQpcTicks() - startTicks);
            }
        }
    }
    // Steady state reads the capped counter instead of writing it on every pass.
    if (bindCount != 0 && dx11_hook_g_DiagSamplerRebound.load(std::memory_order_relaxed) < 48) {
        const int idx = dx11_hook_g_DiagSamplerRebound.fetch_add(bindCount, std::memory_order_relaxed);
        if (idx < 48) {
            D3D11_SAMPLER_DESC desc = {};
            if (ID3D11SamplerState* last = binds[bindCount - 1].physical) {
                last->GetDesc(&desc);
            }
            HookLogImportant("DX11: AF reconciled pixel samplers mask=0x%04X rebound=%d slow=%d last=s%u Filter=0x%X "
                             "Aniso=%u Bias=%.2f (#%d)",
                             mask, bindCount, slowSlots, binds[bindCount - 1].slot, desc.Filter, desc.MaxAnisotropy,
                             desc.MipLODBias, idx + 1);
        }
    }
    for (int i = 0; i < bindCount; ++i) {
        if (binds[i].physical) {
            binds[i].physical->Release();
        }
    }
    return bindCount;
}

// Hook service thread: one line per 10 s window in which the raw D3D11
// forced-AF path did any work. Sampled passes give the CPU estimate; the game
// thread itself never formats or logs.
void DX11Hook_ReportSamplerCostIfDue() {
    static constexpr uint64_t kWindowMs = 10'000;
    static uint64_t s_windowStartMs = 0;
    static D3D11SamplerCostCounters s_reported;
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
    ce::wrapper_af_cost::LogWindowIfActive(elapsedMs);
    D3D11SamplerCostCounters total;
    size_t contexts = 0;
    {
        std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
        total = dx11_hook_g_RetiredSamplerCost11;
        for (const auto& [context, state] : dx11_hook_g_D3D11ContextStates) {
            (void)context;
            total.Add(state.cost);
        }
        contexts = dx11_hook_g_D3D11ContextStates.size();
    }
    D3D11SamplerCostCounters window = total;
    window.srvBinds -= s_reported.srvBinds;
    window.samplerBinds -= s_reported.samplerBinds;
    window.shaderBinds -= s_reported.shaderBinds;
    window.passes -= s_reported.passes;
    window.fastSlots -= s_reported.fastSlots;
    window.slowSlots -= s_reported.slowSlots;
    window.viewClasses -= s_reported.viewClasses;
    window.rebinds -= s_reported.rebinds;
    window.timedPasses -= s_reported.timedPasses;
    window.timedTicks -= s_reported.timedTicks;
    window.timedBinds -= s_reported.timedBinds;
    window.timedBindTicks -= s_reported.timedBindTicks;
    window.choiceEvictions -= s_reported.choiceEvictions;
    s_reported = total;
    if (window.srvBinds + window.samplerBinds + window.shaderBinds + window.passes == 0) {
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
        "[D3D11 AF COST] window=%llums contexts=%zu binds/s(srv=%.0f sampler=%.0f shader=%.0f) passes/s=%.0f "
        "slots(fast=%llu slow=%llu) viewClasses=%llu rebinds=%llu evictions=%llu passUs=%.2f(sampled=%llu) "
        "bindUs=%.2f(sampled=%llu) estMsPerS=%.2f(passes=%.2f binds=%.2f)",
        static_cast<unsigned long long>(elapsedMs), contexts, static_cast<double>(window.srvBinds) / seconds,
        static_cast<double>(window.samplerBinds) / seconds, static_cast<double>(window.shaderBinds) / seconds,
        static_cast<double>(window.passes) / seconds, static_cast<unsigned long long>(window.fastSlots),
        static_cast<unsigned long long>(window.slowSlots), static_cast<unsigned long long>(window.viewClasses),
        static_cast<unsigned long long>(window.rebinds), static_cast<unsigned long long>(window.choiceEvictions),
        passUs, static_cast<unsigned long long>(window.timedPasses), bindUs,
        static_cast<unsigned long long>(window.timedBinds), passMsPerS + bindMsPerS, passMsPerS, bindMsPerS);
}

D3D11SamplerCostCounters DX11Hook_SamplerCostTotalsForTesting() {
    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    D3D11SamplerCostCounters total = dx11_hook_g_RetiredSamplerCost11;
    for (const auto& [context, state] : dx11_hook_g_D3D11ContextStates) {
        (void)context;
        total.Add(state.cost);
    }
    return total;
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

// Values in CEFlowD3D11SamplerCost order (tests/flow/flow_api.h); returns how many were written.
extern "C" __declspec(dllexport) uint32_t CEFlow_GetD3D11SamplerCost(uint64_t* values, uint32_t count) {
    const D3D11SamplerCostCounters total = DX11Hook_SamplerCostTotalsForTesting();
    const uint64_t ordered[] = {total.srvBinds,    total.samplerBinds, total.passes,         total.fastSlots,
                                total.slowSlots,   total.viewClasses,  total.rebinds,        total.choiceEvictions};
    constexpr uint32_t kValues = static_cast<uint32_t>(sizeof(ordered) / sizeof(ordered[0]));
    const uint32_t written = values ? (count < kValues ? count : kValues) : 0u;
    for (uint32_t i = 0; i < written; ++i) {
        values[i] = ordered[i];
    }
    return written;
}
#endif
