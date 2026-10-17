// The raw context hooks' side of the shared forced-AF engine (d3d11_af_engine.h):
// how the engine reaches a real context CE does not wrap, and where those
// contexts' states live.
//
// Each tracked context owns a slot. The thread that calls into a context finds
// its slot through a small thread-local cache and holds only that slot, through
// a spin flag, for one engine state step; the steady state takes no process-wide
// lock and does no map lookup. (Until 2026-10-17 every bind took one process
// mutex plus a hash lookup, and deferred-context recording threads serialized on
// it.)
//
// Other threads still reach slots: a resize or a host disconnect drops every
// state, the hook thread sums the counters, a config change sweeps every state
// once. They take the slot map's mutex and then each slot's flag, never the
// other way round, and a state step never takes the map's mutex.
//
// Slots are never freed, only recycled, so a cached slot pointer stays valid
// memory. A recycled slot gets a new incarnation; the owner re-checks the
// incarnation it cached once it holds the slot and looks the context up again
// when it changed. A dropped state is moved out of its slot and released with
// no lock held, and COM releases made during a state step run only after the
// slot is free (DeferredReleaseScope): a final release can run arbitrary
// runtime code, including a call back into the same context.

#include "dx11_hook_internal.h"

#include <vector>

#include "hook/overrides/sampler_override/translation_scope.h"

namespace {

using ce::d3d11_af::PixelState;

struct RawContextSlot {
    std::atomic<bool> busy{false};
    // Written with `busy` held and under g_slotsMutex: readable under either.
    uint64_t incarnation = 1;
    ID3D11DeviceContext* context = nullptr;
    std::unique_ptr<D3D11PerContextState> state;
    bool usedSinceClear = false;  // `busy` held
};

// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex constructors are noexcept on this toolchain
std::mutex g_slotsMutex;
std::unordered_map<ID3D11DeviceContext*, RawContextSlot*> g_slots;  // g_slotsMutex
std::vector<RawContextSlot*> g_freeSlots;                           // g_slotsMutex
ce::d3d11_af::CostSnapshot g_retiredCost;                           // g_slotsMutex
std::atomic<uint32_t> g_dirtyContexts{0};
std::atomic<uint64_t> g_sweptEpoch{0};
std::atomic<uint64_t> g_staleSlotRetries{0};

#ifdef CE_FLOW_TEST
// Flow tests run another thread's state drop inside the window between the
// thread-local lookup and taking the slot, and while the slot is held.
std::atomic<void (*)(ID3D11DeviceContext*)> g_lookupProbe{nullptr};
std::atomic<void (*)(ID3D11DeviceContext*)> g_heldProbe{nullptr};
#endif

void AcquireSlot(RawContextSlot* slot) {
    for (unsigned spins = 0; slot->busy.exchange(true, std::memory_order_acquire);) {
        // Held only for one state step by the owner, or briefly by a cross-thread drop.
        while (slot->busy.load(std::memory_order_relaxed)) {
            if ((++spins & 63u) == 0) {
                SwitchToThread();
            } else {
                YieldProcessor();
            }
        }
    }
}

void ReleaseSlot(RawContextSlot* slot) {
    slot->busy.store(false, std::memory_order_release);
}

void NoteDirtyTransition(bool wasDirty, bool isDirty) {
    if (wasDirty == isDirty) {
        return;
    }
    if (isDirty) {
        g_dirtyContexts.fetch_add(1, std::memory_order_release);
    } else {
        g_dirtyContexts.fetch_sub(1, std::memory_order_release);
    }
}

// Trivially destructible on purpose: no TLS destructor in an injected DLL.
struct SlotCache {
    static constexpr unsigned kEntries = 4;
    ID3D11DeviceContext* context[kEntries];
    RawContextSlot* slot[kEntries];
    uint64_t incarnation[kEntries];
    unsigned next;
};
thread_local SlotCache t_slots;

// The context's slot, created on first use, and the incarnation this thread saw.
RawContextSlot* LookupSlot(ID3D11DeviceContext* context, uint64_t* incarnation) {
    SlotCache& cache = t_slots;
    for (unsigned i = 0; i < SlotCache::kEntries; ++i) {
        if (cache.context[i] == context && cache.slot[i]) {
            *incarnation = cache.incarnation[i];
            return cache.slot[i];
        }
    }
    RawContextSlot* slot = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_slotsMutex);
        RawContextSlot*& entry = g_slots[context];
        if (!entry) {
            if (!g_freeSlots.empty()) {
                entry = g_freeSlots.back();
                g_freeSlots.pop_back();
            } else {
                entry = new RawContextSlot();  // never freed: cached pointers stay valid memory
            }
            AcquireSlot(entry);
            entry->context = context;
            entry->state = std::make_unique<D3D11PerContextState>();
            entry->state->af.epoch = g_sweptEpoch.load(std::memory_order_relaxed);
            ReleaseSlot(entry);
        }
        slot = entry;
        *incarnation = slot->incarnation;
    }
    const unsigned index = cache.next++ % SlotCache::kEntries;
    cache.context[index] = context;
    cache.slot[index] = slot;
    cache.incarnation[index] = *incarnation;
    return slot;
}

void ForgetSlot(ID3D11DeviceContext* context) {
    SlotCache& cache = t_slots;
    for (unsigned i = 0; i < SlotCache::kEntries; ++i) {
        if (cache.context[i] == context) {
            cache.context[i] = nullptr;
            cache.slot[i] = nullptr;
        }
    }
}

// With g_slotsMutex and the slot held: takes the state out and recycles the slot.
std::unique_ptr<D3D11PerContextState> RetireHeldSlotLocked(RawContextSlot* slot) {
    ++slot->incarnation;
    std::unique_ptr<D3D11PerContextState> retired = std::move(slot->state);
    slot->context = nullptr;
    slot->usedSinceClear = false;
    ReleaseSlot(slot);
    if (retired) {
        NoteDirtyTransition(retired->af.dirtyMask != 0, false);
        g_retiredCost.Add(retired->af.cost.Snapshot());
    }
    g_freeSlots.push_back(slot);
    return retired;
}

// A config change (new replacement epoch) reconsiders every context once, so the
// per-draw exit below can keep looking at one counter.
void SweepEpoch(uint64_t epoch) {
    if (g_sweptEpoch.load(std::memory_order_acquire) == epoch) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_slotsMutex);
    // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order) - order is irrelevant
    for (auto& [context, slot] : g_slots) {
        (void)context;
        AcquireSlot(slot);
        PixelState& state = slot->state->af;
        const bool wasDirty = state.dirtyMask != 0;
        ce::d3d11_af::SyncEpoch(state, epoch);
        NoteDirtyTransition(wasDirty, state.dirtyMask != 0);
        ReleaseSlot(slot);
    }
    g_sweptEpoch.store(epoch, std::memory_order_release);
}

struct RawAFAccess {
    // Eager: d3d11.dll rewrites a context's draw entries in its per-context vtable after a flush,
    // and CE's raw draw hooks then miss the draws a deferred bind would wait for (2026-10-17, WARP).
    static constexpr bool kDefersSamplerBinds = false;

    ID3D11DeviceContext* context;

    template <class Fn>
    void Run(Fn&& fn) {
        // Destroyed last: queued releases run once the slot is free.
        ce::d3d11_af::DeferredReleaseScope releases;
        for (;;) {
            uint64_t incarnation = 0;
            RawContextSlot* slot = LookupSlot(context, &incarnation);
#ifdef CE_FLOW_TEST
            if (const auto probe = g_lookupProbe.load(std::memory_order_acquire)) {
                probe(context);
            }
#endif
            AcquireSlot(slot);
            if (slot->incarnation != incarnation) {
                // Dropped (and maybe recycled) between this thread's lookup and now.
                ReleaseSlot(slot);
                ForgetSlot(context);
                g_staleSlotRetries.fetch_add(1, std::memory_order_relaxed);
                continue;
            }
            slot->usedSinceClear = true;
            PixelState& state = slot->state->af;
            const bool wasDirty = state.dirtyMask != 0;
            fn(state);
            NoteDirtyTransition(wasDirty, state.dirtyMask != 0);
#ifdef CE_FLOW_TEST
            if (const auto probe = g_heldProbe.load(std::memory_order_acquire)) {
                probe(context);
            }
#endif
            ReleaseSlot(slot);
            return;
        }
    }

    bool IsClean(uint64_t epoch) {
        SweepEpoch(epoch);
        return g_dirtyContexts.load(std::memory_order_acquire) == 0;
    }
};

class RawAFHost final : public ce::d3d11_af::Host {
public:
    RawAFHost(ID3D11DeviceContext* context, SetSamplers11_t original) : context_(context), original_(original) {}

    void BindPhysical(UINT startSlot, UINT count, ID3D11SamplerState* const* samplers) override {
        if (original_) {
            original_(context_, startSlot, count, samplers);
        }
    }
    ID3D11PixelShader* AcquireBoundShader() override {
        ID3D11PixelShader* shader = nullptr;
        UINT classInstanceCount = 0;
        context_->PSGetShader(&shader, nullptr, &classInstanceCount);
        return shader;
    }
    ID3D11ShaderResourceView* AcquireBoundView(UINT slot) override {
        ID3D11ShaderResourceView* view = nullptr;
        context_->PSGetShaderResources(slot, 1, &view);
        return view;
    }
    ID3D11Device* AcquireDevice() override {
        ID3D11Device* device = nullptr;
        context_->GetDevice(&device);
        return device;
    }
    HRESULT CreateReplacement(ID3D11Device* device, const D3D11_SAMPLER_DESC& desc,
                              ID3D11SamplerState** sampler) override {
        // The saved original skips CE's own CreateSamplerState override.
        ce::sampler_override::D3D11SamplerCreationScope translationScope;
        return dx11_hook_oCreateSamplerState ? dx11_hook_oCreateSamplerState(device, &desc, sampler)
                                             : device->CreateSamplerState(&desc, sampler);
    }
    const char* PathName() const override { return "raw"; }

private:
    ID3D11DeviceContext* context_;
    SetSamplers11_t original_;
};

SetSamplers11_t ResolvePSSetSamplersOriginal(ID3D11DeviceContext* context) {
    return ResolveContextOriginal11(context, ce::d3d11_slots::kPSSetSamplers,
                                    &D3D11ContextVTableOriginals::psSetSamplers, dx11_hook_oPSSetSamplers11);
}

}  // namespace

void RawAF_OnViews(ID3D11DeviceContext* context, UINT startSlot, UINT numViews,
                   ID3D11ShaderResourceView* const* views) {
    if (!context) {
        return;
    }
    RawAFAccess access{context};
    ce::d3d11_af::OnViews(access, startSlot, numViews, views);
}

void RawAF_OnShader(ID3D11DeviceContext* context, ID3D11PixelShader* shader) {
    if (!context) {
        return;
    }
    RawAFAccess access{context};
    ce::d3d11_af::OnShader(access, shader);
}

void RawAF_OnSamplers(SetSamplers11_t originalFn, ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                      ID3D11SamplerState* const* samplers) {
    if (!originalFn) {
        return;
    }
    if (!context || dx11_hook_g_InOverlayRender) {
        // CE's own overlay saves and restores the game's bindings: what it binds is
        // physical state only.
        originalFn(context, startSlot, numSamplers, samplers);
        if (context && startSlot < ce::d3d11_af::kSamplerSlots) {
            RawAFAccess access{context};
            access.Run([&](PixelState& state) {
                for (UINT i = 0; i < numSamplers && startSlot + i < ce::d3d11_af::kSamplerSlots; ++i) {
                    ce::d3d11_af::RememberPhysical(state, startSlot + i, samplers ? samplers[i] : nullptr);
                }
            });
        }
        return;
    }
    RawAFAccess access{context};
    RawAFHost host(context, originalFn);
    ce::d3d11_af::OnSamplers(access, host, startSlot, numSamplers, samplers);
}

void RawAF_BeforeDraw(ID3D11DeviceContext* context) {
    // Clean draws (the steady state) return here: a config-version check, the swept
    // epoch and one counter. The policy read is what notices a config change at a draw
    // the game binds nothing new for.
    RawAFAccess access{context};
    const ce::d3d11_af::Policy policy = ce::d3d11_af::AcquirePolicy();
    if (!access.IsClean(policy.epoch)) {
        if (const SetSamplers11_t original = ResolvePSSetSamplersOriginal(context)) {
            RawAFHost host(context, original);
            ce::d3d11_af::Reconcile(access, host, policy, ce::d3d11_af::kAllSamplerSlots,
                                    ce::d3d11_af::PassKind::kDraw, nullptr);
        }
    }
    ce::d3d11_af::NoteDraw();
}

void RawAF_TranslateToLogical(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                              ID3D11SamplerState** samplers) {
    if (!context || !samplers || startSlot >= ce::d3d11_af::kSamplerSlots) {
        return;
    }
    RawAFAccess access{context};
    access.Run([&](PixelState& state) { ce::d3d11_af::TranslateToLogical(state, startSlot, numSamplers, samplers); });
}

int RawAF_Bootstrap(ID3D11DeviceContext* context) {
    if (!context) {
        return 0;
    }
    RawAFAccess access{context};
    ID3D11PixelShader* shader = nullptr;
    UINT classInstanceCount = 0;
    context->PSGetShader(&shader, nullptr, &classInstanceCount);
    ce::d3d11_af::OnShader(access, shader);
    if (shader) {
        shader->Release();
    }
    ID3D11ShaderResourceView* views[ce::d3d11_af::kViewSlots] = {};
    context->PSGetShaderResources(0, ce::d3d11_af::kViewSlots, views);
    ce::d3d11_af::OnViews(access, 0, ce::d3d11_af::kViewSlots, views);
    for (ID3D11ShaderResourceView* view : views) {
        if (view) {
            view->Release();
        }
    }
    // Through CE's own PSGetSamplers hook: a replacement CE bound reads back as the game's sampler.
    ID3D11SamplerState* bound[ce::d3d11_af::kSamplerSlots] = {};
    context->PSGetSamplers(0, ce::d3d11_af::kSamplerSlots, bound);
    ID3D11SamplerState* physical[ce::d3d11_af::kSamplerSlots] = {};
    if (const auto original = ResolveContextOriginal11(context, ce::d3d11_slots::kPSGetSamplers,
                                                       &D3D11ContextVTableOriginals::psGetSamplers,
                                                       dx11_hook_oPSGetSamplers11)) {
        original(context, 0, ce::d3d11_af::kSamplerSlots, physical);
    }
    int boundSamplers = 0;
    access.Run([&](PixelState& state) {
        const uint32_t changed = ce::d3d11_af::TrackSamplers(state, 0, ce::d3d11_af::kSamplerSlots, bound);
        for (UINT slot = 0; slot < ce::d3d11_af::kSamplerSlots; ++slot) {
            ce::d3d11_af::RememberPhysical(state, slot, physical[slot] ? physical[slot] : bound[slot]);
            boundSamplers += bound[slot] ? 1 : 0;
        }
        ce::d3d11_af::MarkDirty(state, changed);
    });
    for (UINT slot = 0; slot < ce::d3d11_af::kSamplerSlots; ++slot) {
        if (bound[slot]) {
            bound[slot]->Release();
        }
        if (physical[slot]) {
            physical[slot]->Release();
        }
    }
    return boundSamplers;
}

void ClearTrackedContextState11(ID3D11DeviceContext* context) {
    if (!context) {
        return;
    }
    std::unique_ptr<D3D11PerContextState> retired;
    {
        std::lock_guard<std::mutex> lock(g_slotsMutex);
        auto it = g_slots.find(context);
        if (it == g_slots.end()) {
            return;
        }
        RawContextSlot* slot = it->second;
        g_slots.erase(it);
        AcquireSlot(slot);
        retired = RetireHeldSlotLocked(slot);
    }
    // Driver-owned COM destruction must not run under a CE lock; a release can
    // execute arbitrary runtime code and must be safe to re-enter.
    retired.reset();
}

void ReleaseTrackedShaderResources11() {
    // Any thread: a resize (CE's references to back-buffer views must not make it fail) or a
    // host disconnect. A context used since the previous clear keeps its samplers, shader and
    // memo and loses only its views: dropping the rest would lose a bind its owner tracked but
    // has not applied yet, and which of CE's replacements the real context holds. A context
    // not used since then (destroyed, as far as CE can tell) is dropped whole, so a dead
    // context's samplers cannot keep its device alive.
    ce::d3d11_af::DeferredReleaseScope releases;  // flushed after the map mutex below
    std::vector<std::unique_ptr<D3D11PerContextState>> retired;
    {
        std::lock_guard<std::mutex> lock(g_slotsMutex);
        // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order) - order is irrelevant
        for (auto it = g_slots.begin(); it != g_slots.end();) {
            RawContextSlot* slot = it->second;
            AcquireSlot(slot);
            if (!slot->usedSinceClear) {
                retired.push_back(RetireHeldSlotLocked(slot));
                it = g_slots.erase(it);
                continue;
            }
            slot->usedSinceClear = false;
            ce::d3d11_af::DropViews(slot->state->af);
            ReleaseSlot(slot);
            ++it;
        }
    }
    retired.clear();
}

void ReleaseAllTrackedContextStates11() {
    std::vector<std::unique_ptr<D3D11PerContextState>> retired;
    {
        std::lock_guard<std::mutex> lock(g_slotsMutex);
        retired.reserve(g_slots.size());
        // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order) - order is irrelevant
        for (auto& [context, slot] : g_slots) {
            (void)context;
            AcquireSlot(slot);
            retired.push_back(RetireHeldSlotLocked(slot));
        }
        g_slots.clear();
    }
    retired.clear();
}

ce::d3d11_af::CostSnapshot RawAF_SumCost(size_t* contexts) {
    std::lock_guard<std::mutex> lock(g_slotsMutex);
    ce::d3d11_af::CostSnapshot total = g_retiredCost;
    // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order) - summing is order-independent
    for (const auto& [context, slot] : g_slots) {
        (void)context;
        total.Add(slot->state->af.cost.Snapshot());  // single-writer counters: no slot flag needed
    }
    if (contexts) {
        *contexts = g_slots.size();
    }
    return total;
}

ce::d3d11_af::CostSnapshot DX11Hook_SamplerCostTotalsForTesting() {
    return RawAF_SumCost(nullptr);
}

#ifdef CE_FLOW_TEST

extern "C" __declspec(dllexport) void CEFlow_SetRawAFProbes(void (*lookupProbe)(ID3D11DeviceContext*),
                                                           void (*heldProbe)(ID3D11DeviceContext*)) {
    g_lookupProbe.store(lookupProbe, std::memory_order_release);
    g_heldProbe.store(heldProbe, std::memory_order_release);
}

// What a resize or a host disconnect does: every raw context's tracked views dropped.
extern "C" __declspec(dllexport) void CEFlow_ReleaseTrackedD3D11Views() {
    ReleaseTrackedShaderResources11();
}

// What CE's shutdown does: every raw context's state dropped.
extern "C" __declspec(dllexport) void CEFlow_ReleaseAllTrackedD3D11States() {
    ReleaseAllTrackedContextStates11();
}

// The incarnation of the context's slot, 0 when the context has no state: unchanged
// across a view drop, gone after a whole drop.
extern "C" __declspec(dllexport) uint64_t CEFlow_RawAFSlotIncarnation(ID3D11DeviceContext* context) {
    std::lock_guard<std::mutex> lock(g_slotsMutex);
    const auto it = g_slots.find(context);
    return it == g_slots.end() ? 0 : it->second->incarnation;
}

// How often a thread found its cached slot dropped once it held it.
extern "C" __declspec(dllexport) uint64_t CEFlow_RawAFStaleSlotRetries() {
    return g_staleSlotRetries.load(std::memory_order_relaxed);
}

#endif
