// The shared D3D11 forced-AF engine (d3d11_af_engine.h): state steps, the memoized
// fast resolution and the slow classification that creates replacements.

#include "d3d11_af_engine.h"

#include "dx11_hook_internal.h"

#include "hook/overrides/sampler_override/creation_policy.h"
#include "hook/overrides/sampler_override/translation_scope.h"
#include "hook/metrics/hook_cpu_cost.h"
#include "d3d11_af_metadata.h"
#include "d3d11_af_thread.h"

namespace ce::d3d11_af {

using ce::sampler_override::D3D11AFSamplerPlan;
using ce::sampler_override::D3D11AFTextureMask;
using ce::sampler_override::D3D11ForcedAFResourceDecision;
using ce::sampler_override::D3D11ShaderAFPlan;

namespace {

// Unknown or protected shaders: no slot is an AF candidate, every view change matters
// only to slots CE overrides.
const D3D11ShaderAFPlan kNoPlan{};

constexpr uint8_t ClassOf(D3D11ForcedAFResourceDecision decision) {
    return static_cast<uint8_t>(static_cast<int>(decision) + 1);
}
constexpr uint8_t kAllowClass = ClassOf(D3D11ForcedAFResourceDecision::Allow);

thread_local DeferredReleaseScope* t_releaseScope = nullptr;

void ReleaseObject(IUnknown* object) {
    DeferredReleaseScope::Release(object);
}

template <class T>
void Assign(T*& slot, T* value) {
    if (slot == value) {
        return;
    }
    if (value) {
        value->AddRef();
    }
    if (slot) {
        ReleaseObject(slot);
    }
    slot = value;
}

void ResetChoice(SamplerChoice& choice) {
    for (ID3D11SamplerState*& physical : choice.physical) {
        if (physical) {
            ReleaseObject(physical);
        }
    }
    choice = {};
}

size_t ChoiceHome(const ID3D11SamplerState* logical) {
    // Sampler objects are heap allocations; drop the alignment bits before mixing.
    const uint64_t key = reinterpret_cast<uintptr_t>(logical) >> 4;
    return static_cast<size_t>((key ^ (key >> 7) ^ (key >> 17)) % SamplerChoiceCache::kEntries);
}

void ReleaseChoiceEntry(SamplerChoiceCache::Entry& entry) {
    ResetChoice(entry.choice);
    if (entry.logical) {
        ReleaseObject(entry.logical);
    }
    entry = {};
}

const SamplerChoice* FindChoice(SamplerChoiceCache& cache, ID3D11SamplerState* logical) {
    const size_t home = ChoiceHome(logical);
    for (size_t probe = 0; probe < SamplerChoiceCache::kProbe; ++probe) {
        auto& entry = cache.entries[(home + probe) % SamplerChoiceCache::kEntries];
        if (entry.logical == logical) {
            entry.stamp = ++cache.clock;
            return &entry.choice;
        }
    }
    return nullptr;
}

// The entry for `logical`, inserted (with its own reference) when absent.
SamplerChoice& ClaimChoice(PixelState& state, ID3D11SamplerState* logical) {
    SamplerChoiceCache& cache = state.choices;
    const size_t home = ChoiceHome(logical);
    SamplerChoiceCache::Entry* victim = nullptr;
    for (size_t probe = 0; probe < SamplerChoiceCache::kProbe; ++probe) {
        auto& entry = cache.entries[(home + probe) % SamplerChoiceCache::kEntries];
        if (entry.logical == logical) {
            entry.stamp = ++cache.clock;
            return entry.choice;
        }
        // An empty entry wins; otherwise the least recently used one in the window.
        if (!victim || (victim->logical && (!entry.logical || entry.stamp < victim->stamp))) {
            victim = &entry;
        }
    }
    if (victim->logical) {
        ++state.cost.choiceEvictions;
    }
    ReleaseChoiceEntry(*victim);
    logical->AddRef();
    victim->logical = logical;
    victim->stamp = ++cache.clock;
    return victim->choice;
}

// After a write of `logical[slot]` or `physical[slot]`.
void SyncSlotMasks(PixelState& state, UINT slot) {
    const uint32_t bit = 1u << slot;
    state.trackedMask = (state.trackedMask & ~bit) | (state.logical[slot] ? bit : 0u);
    state.overriddenMask = (state.overriddenMask & ~bit) | (state.physical[slot] != state.logical[slot] ? bit : 0u);
}

// Switches the tracked shader. `planOwner` is borrowed from `shader`.
void SetTrackedShader(PixelState& state, ID3D11PixelShader* shader, IUnknown* planOwner, bool overridesActive) {
    state.shaderTracked = true;
    Assign(state.shader, shader);
    state.planOwner = planOwner;
    state.plan = planOwner ? &static_cast<ShaderMetadataHandle*>(planOwner)->plan : &kNoPlan;
    // A protected or unknown new shader must still restore any old AF replacement.
    const uint32_t overridden = OverriddenMask(state);
    const uint32_t dirty = state.plan->ShaderDirtyMask(TrackedMask(state), overridden);
    MarkDirty(state, overridesActive ? dirty : dirty & overridden);
}

enum class Resolution {
    Resolved,
    NeedsViewClasses,  // only the AF class of some sampled views is missing
    Slow,              // sampler, shader or view state must be classified or refreshed
};

// The AF verdict for a pixel slot from tracked state only. A view classified
// earlier keeps its class in its own private data; that one read is taken here.
Resolution TryVerdict(PixelState& state, UINT slot, bool* allow, D3D11AFTextureMask* unknownViews) {
    if (!state.shaderTracked) {
        return Resolution::Slow;
    }
    *allow = false;
    if (!state.shader || !state.planOwner || !state.plan->available) {
        return Resolution::Resolved;
    }
    const D3D11AFSamplerPlan& samplerPlan = state.plan->samplers[slot];
    if (samplerPlan.textures.Count() == 0 || !samplerPlan.SafeSample()) {
        return Resolution::Resolved;
    }
    bool missingClass = false;
    auto textures = samplerPlan.textures;
    for (UINT textureSlot = textures.TakeFirst(); textureSlot != UINT_MAX; textureSlot = textures.TakeFirst()) {
        if (!state.views[textureSlot]) {
            return Resolution::Slow;  // the slow path refreshes the slot from the context
        }
        uint8_t afClass = state.viewClass[textureSlot];
        D3D11ForcedAFResourceDecision cached = D3D11ForcedAFResourceDecision::Allow;
        if (afClass == 0 && TryGetCachedViewClass(state.views[textureSlot], &cached)) {
            afClass = ClassOf(cached);
            state.viewClass[textureSlot] = afClass;
            ++state.cost.viewClasses;
        }
        if (afClass == 0) {
            unknownViews->Add(textureSlot);
            missingClass = true;
        } else if (afClass != kAllowClass && !missingClass) {
            return Resolution::Resolved;  // a protected view decides before any later one
        }
    }
    if (missingClass) {
        return Resolution::NeedsViewClasses;
    }
    *allow = true;
    return Resolution::Resolved;
}

// The physical sampler for the slot's logical sampler, borrowed.
Resolution TryResolveSlot(PixelState& state, UINT slot, const Policy& policy, ID3D11SamplerState** physical,
                          D3D11AFTextureMask* unknownViews) {
    ID3D11SamplerState* logical = state.logical[slot];
    if (!logical || !policy.active) {
        *physical = logical;  // nothing configured: the game's own sampler
        return Resolution::Resolved;
    }
    const SamplerChoice* found = FindChoice(state.choices, logical);
    if (!found || found->epoch != policy.epoch) {
        return Resolution::Slow;
    }
    if (found->isReplacement) {
        *physical = logical;
        return Resolution::Resolved;
    }
    bool allow = false;
    if (found->descAllowsAF) {
        const Resolution verdict = TryVerdict(state, slot, &allow, unknownViews);
        if (verdict != Resolution::Resolved) {
            return verdict;
        }
    }
    const int index = allow ? 1 : 0;
    if (!found->resolved[index]) {
        return Resolution::Slow;
    }
    *physical = found->physical[index] ? found->physical[index] : logical;
    return Resolution::Resolved;
}

}  // namespace

DeferredReleaseScope::DeferredReleaseScope() : previous_(t_releaseScope) {
    t_releaseScope = this;
}

DeferredReleaseScope::~DeferredReleaseScope() {
    t_releaseScope = previous_;
    for (int i = 0; i < count_; ++i) {
        items_[i]->Release();
    }
    for (IUnknown* object : overflow_) {
        object->Release();
    }
}

void DeferredReleaseScope::Release(IUnknown* object) {
    DeferredReleaseScope* scope = t_releaseScope;
    if (!scope) {
        object->Release();
        return;
    }
    if (scope->count_ < kCapacity) {
        scope->items_[scope->count_++] = object;
        return;
    }
    try {
        scope->overflow_.push_back(object);
    } catch (...) {
        object->Release();  // out of memory: released now, as without a scope
    }
}

PixelState::PixelState() : plan(&kNoPlan) {}

PixelState::~PixelState() {
    ClearState(*this);
    for (auto& entry : choices.entries) {
        ReleaseChoiceEntry(entry);
    }
}

Policy AcquirePolicy() {
    const SamplerPolicySnapshot11 snapshot = AcquireSamplerPolicy11();
    Policy policy;
    policy.gfx = snapshot.gfx;
    policy.epoch = snapshot.epoch;
    policy.hash = snapshot.hash;
    policy.active = g_D3D11SamplerOverridesActive.load(std::memory_order_acquire);
    return policy;
}

bool OverridesActive() {
    return g_D3D11SamplerOverridesActive.load(std::memory_order_relaxed);
}

void ClearState(PixelState& state) {
    for (UINT slot = 0; slot < kViewSlots; ++slot) {
        Assign(state.views[slot], static_cast<ID3D11ShaderResourceView*>(nullptr));
        state.viewClass[slot] = 0;
    }
    for (UINT slot = 0; slot < kSamplerSlots; ++slot) {
        Assign(state.logical[slot], static_cast<ID3D11SamplerState*>(nullptr));
        Assign(state.physical[slot], static_cast<ID3D11SamplerState*>(nullptr));
    }
    state.trackedMask = 0;
    state.overriddenMask = 0;
    state.eagerMask = 0;
    state.planOwner = nullptr;
    state.plan = &kNoPlan;
    Assign(state.shader, static_cast<ID3D11PixelShader*>(nullptr));
    state.shaderTracked = false;
    state.dirtyMask = 0;
    // The choice and shader memos are keyed by objects and stay valid across a state reset.
}

void DropViews(PixelState& state) {
    for (UINT slot = 0; slot < kViewSlots; ++slot) {
        Assign(state.views[slot], static_cast<ID3D11ShaderResourceView*>(nullptr));
        state.viewClass[slot] = 0;
    }
}

#ifdef CE_FLOW_TEST
namespace {
std::atomic<uint64_t> g_slotMaskMismatches{0};
}  // namespace

void CheckSlotMasks(const PixelState& state) {
    uint32_t tracked = 0;
    uint32_t overridden = 0;
    for (UINT slot = 0; slot < kSamplerSlots; ++slot) {
        tracked |= state.logical[slot] ? 1u << slot : 0u;
        overridden |= state.physical[slot] != state.logical[slot] ? 1u << slot : 0u;
    }
    if (tracked != state.trackedMask || overridden != state.overriddenMask) {
        if (g_slotMaskMismatches.fetch_add(1, std::memory_order_relaxed) < 8) {
            HookLogImportant("DX11 AF: slot masks disagree with the slots tracked=0x%04X/0x%04X "
                             "overridden=0x%04X/0x%04X",
                             state.trackedMask, tracked, state.overriddenMask, overridden);
        }
    }
}

extern "C" __declspec(dllexport) uint64_t CEFlow_D3D11AFSlotMaskMismatches() {
    return g_slotMaskMismatches.load(std::memory_order_relaxed);
}
#endif

void MarkDirty(PixelState& state, uint32_t mask) {
    state.dirtyMask |= mask & kAllSamplerSlots;
}

void SyncEpoch(PixelState& state, uint64_t epoch) {
    if (state.epoch != epoch) {
        state.epoch = epoch;
        MarkDirty(state, TrackedMask(state) | OverriddenMask(state));
    }
}

void TrackViews(PixelState& state, UINT startSlot, UINT count, ID3D11ShaderResourceView* const* views,
                bool overridesActive) {
    if (startSlot >= kViewSlots) {
        return;
    }
    const UINT actual = count < kViewSlots - startSlot ? count : kViewSlots - startSlot;
    const uint32_t tracked = TrackedMask(state);
    const uint32_t overridden = OverriddenMask(state);
    uint32_t dirty = 0;
    for (UINT i = 0; i < actual; ++i) {
        const UINT slot = startSlot + i;
        ID3D11ShaderResourceView* view = views ? views[i] : nullptr;
        if (state.views[slot] == view) {
            continue;
        }
        // An untracked shader (CE attached late) may sample anything; a known shader without a
        // usable plan cannot gain AF, so only CE's own replacements matter there.
        if (!state.shaderTracked) {
            dirty |= tracked;
        } else {
            dirty |= state.plan->available ? state.plan->ResourceDirtyMask(slot, tracked) : overridden;
        }
        Assign(state.views[slot], view);
        state.viewClass[slot] = 0;
    }
    MarkDirty(state, overridesActive ? dirty : dirty & overridden);
}

uint32_t TrackSamplers(PixelState& state, UINT startSlot, UINT count, ID3D11SamplerState* const* samplers) {
    uint32_t changed = 0;
    for (UINT i = 0; i < count && startSlot + i < kSamplerSlots; ++i) {
        const UINT slot = startSlot + i;
        ID3D11SamplerState* sampler = samplers ? samplers[i] : nullptr;
        if (state.logical[slot] == sampler) {
            continue;
        }
        Assign(state.logical[slot], sampler);
        SyncSlotMasks(state, slot);
        changed |= 1u << slot;
    }
    return changed;
}

IUnknown* AcquireShaderPlanOwner(ID3D11PixelShader* shader) {
    return AcquireShaderMetadata(shader);
}

void TrackShader(PixelState& state, ID3D11PixelShader* shader, IUnknown* planOwner, ThreadBlock* block,
                 bool overridesActive) {
    if (state.shader == shader) {
        state.shaderTracked = true;
    } else {
        if (shader && planOwner && block && RememberShaderPlan(*block, shader, planOwner)) {
            ++state.cost.shaderEvictions;
        }
        SetTrackedShader(state, shader, planOwner, overridesActive);
    }
    // The acquired reference goes; the shader the state now holds keeps its metadata alive.
    if (planOwner) {
        ReleaseObject(planOwner);
    }
}

bool TrackKnownShader(PixelState& state, ID3D11PixelShader* shader, ThreadBlock* block, bool overridesActive) {
    if (state.shader == shader) {
        // A held shader is always a tracked one; an untracked state holds none.
        if (state.shaderTracked) {
            ++state.cost.shaderRepeats;
        }
        state.shaderTracked = true;
        return true;
    }
    if (!shader) {
        SetTrackedShader(state, nullptr, nullptr, overridesActive);
        return true;
    }
    IUnknown* planOwner = block ? FindShaderPlan(*block, shader) : nullptr;
    if (!planOwner) {
        return false;
    }
    ++state.cost.shaderMemoHits;
    SetTrackedShader(state, shader, planOwner, overridesActive);
    return true;
}

void TranslateToLogical(const PixelState& state, UINT startSlot, UINT count, ID3D11SamplerState** samplers) {
    if (!samplers) {
        return;
    }
    for (UINT i = 0; i < count && startSlot + i < kSamplerSlots; ++i) {
        const UINT slot = startSlot + i;
        ID3D11SamplerState* logical = state.logical[slot];
        if (logical && samplers[i] == state.physical[slot] && samplers[i] != logical) {
            if (samplers[i]) {
                ReleaseObject(samplers[i]);
            }
            logical->AddRef();
            samplers[i] = logical;
        }
    }
}

void RememberPhysical(PixelState& state, UINT slot, ID3D11SamplerState* physical) {
    if (slot < kSamplerSlots) {
        Assign(state.physical[slot], physical);
        SyncSlotMasks(state, slot);
    }
}

void ResolveFast(PixelState& state, uint32_t slots, const Policy& policy, bool bindEmptySlots, bool collectViews,
                 Batch& batch) {
    D3D11AFTextureMask collected;
    for (uint32_t pending = slots; pending != 0; pending &= pending - 1) {
        const UINT slot = static_cast<UINT>(std::countr_zero(pending));
        if (!state.logical[slot] && !bindEmptySlots && !state.physical[slot]) {
            continue;
        }
        ID3D11SamplerState* physical = nullptr;
        D3D11AFTextureMask unknownViews;
        const Resolution resolution = TryResolveSlot(state, slot, policy, &physical, &unknownViews);
        if (resolution == Resolution::Slow || (resolution == Resolution::NeedsViewClasses && !collectViews)) {
            batch.slowMask |= 1u << slot;
            continue;
        }
        if (resolution == Resolution::NeedsViewClasses) {
            batch.viewPendingMask |= 1u << slot;
            for (UINT textureSlot = unknownViews.TakeFirst(); textureSlot != UINT_MAX;
                 textureSlot = unknownViews.TakeFirst()) {
                if (collected.words[textureSlot / 64] & (uint64_t{1} << (textureSlot % 64))) {
                    continue;
                }
                collected.Add(textureSlot);
                state.views[textureSlot]->AddRef();
                batch.views[batch.viewCount++] = {textureSlot, state.views[textureSlot],
                                                  D3D11ForcedAFResourceDecision::UnsupportedViewDimension};
            }
            continue;
        }
        ++state.cost.fastSlots;
        if (physical != state.physical[slot]) {
            // Recorded before the bind: only this context's own thread binds its samplers.
            Assign(state.physical[slot], physical);
            SyncSlotMasks(state, slot);
            if (physical) {
                physical->AddRef();  // the pending bind's reference
            }
            ++state.cost.rebinds;
            batch.binds[batch.bindCount++] = {slot, physical, true};
        }
    }
}

void ClassifyViews(Batch& batch) {
    // After the first classification a view keeps its class in its own private
    // data, so this is one private-data read per changed view.
    for (int i = 0; i < batch.viewCount; ++i) {
        batch.views[i].decision = ClassifyView(batch.views[i].view);
    }
}

void ApplyViewClasses(PixelState& state, Batch& batch) {
    for (int i = 0; i < batch.viewCount; ++i) {
        const PendingView& view = batch.views[i];
        if (state.views[view.textureSlot] == view.view) {
            state.viewClass[view.textureSlot] = ClassOf(view.decision);
        }
    }
    state.cost.viewClasses += static_cast<uint64_t>(batch.viewCount);
}

void ReleaseViews(Batch& batch) {
    for (int i = 0; i < batch.viewCount; ++i) {
        batch.views[i].view->Release();
        batch.views[i].view = nullptr;
    }
    batch.viewCount = 0;
}

void SnapshotSlowSlot(PixelState& state, UINT slot, SlowSlot& out) {
    out.slot = slot;
    Assign(out.logical, state.logical[slot]);
    out.needsShader = !state.shaderTracked;
    out.hasShader = state.shader != nullptr;
    out.hasMetadata = state.planOwner != nullptr && state.plan->available;
    out.disassembleFailed = state.planOwner != nullptr && state.plan->disassembleFailed;
    out.plan = state.plan->samplers[slot];
    out.missingViews = {};
    auto textures = out.plan.textures;
    for (UINT textureSlot = textures.TakeFirst(); textureSlot != UINT_MAX; textureSlot = textures.TakeFirst()) {
        Assign(out.views[textureSlot], state.views[textureSlot]);
        if (!state.views[textureSlot]) {
            out.missingViews.Add(textureSlot);
        }
    }
}

void ReleaseSlowSlot(SlowSlot& slot) {
    if (slot.logical) {
        slot.logical->Release();
    }
    for (ID3D11ShaderResourceView*& view : slot.views) {
        if (view) {
            view->Release();
        }
    }
    slot = {};
}

void StoreChoice(PixelState& state, ID3D11SamplerState* logical, uint64_t epoch, const ChoiceFacts& facts,
                 ID3D11SamplerState* physical) {
    if (!logical || !facts.valid) {
        return;
    }
    SamplerChoice& choice = ClaimChoice(state, logical);
    if (choice.epoch != epoch) {
        ResetChoice(choice);
        choice.epoch = epoch;
        choice.isReplacement = facts.isReplacement;
        choice.descAllowsAF = facts.descAllowsAF;
    }
    const int index = facts.allowAF ? 1 : 0;
    if (choice.resolved[index]) {
        return;
    }
    choice.resolved[index] = true;
    if (physical && physical != logical) {
        physical->AddRef();
        choice.physical[index] = physical;
    }
}

void QueueSlowBind(PixelState& state, UINT slot, ID3D11SamplerState* physical, Batch& batch) {
    if (state.physical[slot] == physical) {
        return;
    }
    if (physical) {
        physical->AddRef();
    }
    batch.binds[batch.bindCount++] = {slot, physical, false};
}

BindSample::BindSample(SampleStream stream) : stream_(stream) {
    thread_local uint32_t t_binds = 0;
    thread_local uint32_t t_shaders = 0;
    uint32_t& calls = stream == SampleStream::kShaders ? t_shaders : t_binds;
    if ((++calls & 31u) == 0) {
        startTicks_ = HookQpcTicks();
    }
}

void BindSample::Finish(PixelState& state) {
    if (startTicks_ == 0) {
        return;
    }
    const auto ticks = static_cast<uint64_t>(HookQpcTicks() - startTicks_);
    if (stream_ == SampleStream::kShaders) {
        ++state.cost.timedShaderBinds;
        state.cost.timedShaderTicks += ticks;
    } else {
        ++state.cost.timedBinds;
        state.cost.timedBindTicks += ticks;
    }
    startTicks_ = 0;
}

bool SamplePass(int64_t* startTicks) {
    thread_local uint32_t t_passes = 0;
    if ((++t_passes & 31u) != 0) {
        return false;
    }
    *startTicks = HookQpcTicks();
    return true;
}

void FinishPass(PixelState& state, int64_t startTicks) {
    ++state.cost.timedPasses;
    state.cost.timedTicks += static_cast<uint64_t>(HookQpcTicks() - startTicks);
}

void NoteDeferralEvidence(PixelState& state, uint32_t mask, PassKind kind) {
    // A draw on this thread since the last sampler-bind pass used what that pass bound.
    if (state.eagerThread != &t_drawSequence || state.eagerSequence != t_drawSequence) {
        state.eagerMask = 0;
    }
    state.cost.supersededSlots += static_cast<uint64_t>(std::popcount(mask & state.eagerMask));
    if (kind == PassKind::kDraw) {
        if (state.eagerMask != 0) {
            ++state.cost.eagerOverlaps;
        }
        state.eagerMask = 0;
        return;
    }
    ++state.cost.eagerPasses;
    if (state.eagerMask == 0) {
        ++state.cost.eagerGroups;
    }
    state.eagerMask |= mask;
    state.eagerSequence = t_drawSequence;
    state.eagerThread = &t_drawSequence;
}

void LogReconcile(const Host& host, uint32_t mask, const Batch& batch, int slowSlots) {
    // Steady state reads the capped counter instead of writing it on every pass; the slots a pass
    // took are a per-context count (CostCounters::reconciledSlots), not a process-wide atomic.
    if (batch.bindCount == 0 || dx11_hook_g_DiagSamplerRebound.load(std::memory_order_relaxed) >= 48) {
        return;
    }
    const int idx = dx11_hook_g_DiagSamplerRebound.fetch_add(batch.bindCount, std::memory_order_relaxed);
    if (idx >= 48) {
        return;
    }
    D3D11_SAMPLER_DESC desc = {};
    if (ID3D11SamplerState* last = batch.binds[batch.bindCount - 1].physical) {
        last->GetDesc(&desc);
    }
    HookLogImportant("DX11: AF reconciled pixel samplers path=%s mask=0x%04X rebound=%d slow=%d last=s%u "
                     "Filter=0x%X Aniso=%u Bias=%.2f (#%d)",
                     host.PathName(), mask, batch.bindCount, slowSlots, batch.binds[batch.bindCount - 1].slot,
                     desc.Filter, desc.MaxAnisotropy, desc.MipLODBias, idx + 1);
}

}  // namespace ce::d3d11_af
