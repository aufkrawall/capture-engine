// The raw context hooks' side of the shared forced-AF engine (d3d11_af_engine.h):
// how the engine reaches a real context CE does not wrap, and the process map
// that holds those contexts' states.
//
// A context's state lives in dx11_hook_g_D3D11ContextStates under one mutex. The
// engine holds it only for its short state steps; every runtime call (view
// classification, sampler creation, binds) runs between them. A state that is
// dropped is moved out first and released without the mutex: a final COM release
// can run arbitrary runtime code.

#include "dx11_hook_internal.h"

#include "hook/overrides/sampler_override/translation_scope.h"

namespace {

using ce::d3d11_af::PixelState;

std::atomic<uint64_t> g_sweptEpoch{0};

void NoteDirtyTransition(bool wasDirty, bool isDirty) {
    if (wasDirty == isDirty) {
        return;
    }
    if (isDirty) {
        dx11_hook_g_D3D11DirtyContextCount.fetch_add(1, std::memory_order_release);
    } else {
        dx11_hook_g_D3D11DirtyContextCount.fetch_sub(1, std::memory_order_release);
    }
}

// A config change (new replacement epoch) reconsiders every context once, so the
// per-draw exit below can keep looking at one counter.
void SweepEpoch(uint64_t epoch) {
    if (g_sweptEpoch.load(std::memory_order_acquire) == epoch) {
        return;
    }
    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order) - order is irrelevant
    for (auto& [context, entry] : dx11_hook_g_D3D11ContextStates) {
        (void)context;
        const bool wasDirty = entry->af.dirtyMask != 0;
        ce::d3d11_af::SyncEpoch(entry->af, epoch);
        NoteDirtyTransition(wasDirty, entry->af.dirtyMask != 0);
    }
    g_sweptEpoch.store(epoch, std::memory_order_release);
}

struct RawAFAccess {
    ID3D11DeviceContext* context;

    template <class Fn>
    void Run(Fn&& fn) {
        std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
        std::unique_ptr<D3D11PerContextState>& entry = dx11_hook_g_D3D11ContextStates[context];
        if (!entry) {
            entry = std::make_unique<D3D11PerContextState>();
            entry->af.epoch = g_sweptEpoch.load(std::memory_order_relaxed);
        }
        PixelState& state = entry->af;
        const bool wasDirty = state.dirtyMask != 0;
        fn(state);
        NoteDirtyTransition(wasDirty, state.dirtyMask != 0);
    }

    bool IsClean(uint64_t epoch) {
        SweepEpoch(epoch);
        return dx11_hook_g_D3D11DirtyContextCount.load(std::memory_order_acquire) == 0;
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
    if (access.IsClean(policy.epoch)) {
        return;
    }
    const SetSamplers11_t original = ResolvePSSetSamplersOriginal(context);
    if (!original) {
        return;
    }
    RawAFHost host(context, original);
    ce::d3d11_af::Reconcile(access, host, policy, ce::d3d11_af::kAllSamplerSlots, false, nullptr);
}

void RawAF_TranslateToLogical(ID3D11DeviceContext* context, UINT startSlot, UINT numSamplers,
                              ID3D11SamplerState** samplers) {
    if (!context || !samplers || startSlot >= ce::d3d11_af::kSamplerSlots) {
        return;
    }
    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    const auto it = dx11_hook_g_D3D11ContextStates.find(context);
    if (it != dx11_hook_g_D3D11ContextStates.end()) {
        ce::d3d11_af::TranslateToLogical(it->second->af, startSlot, numSamplers, samplers);
    }
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
        std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
        auto it = dx11_hook_g_D3D11ContextStates.find(context);
        if (it == dx11_hook_g_D3D11ContextStates.end()) {
            return;
        }
        retired = std::move(it->second);
        dx11_hook_g_D3D11ContextStates.erase(it);
        NoteDirtyTransition(retired->af.dirtyMask != 0, false);
        dx11_hook_g_RetiredSamplerCost11.Add(retired->af.cost.Snapshot());
    }
    // Driver-owned COM destruction must not run under the tracking mutex; a
    // release can execute arbitrary runtime code and must be safe to re-enter.
    retired.reset();
}

void ReleaseTrackedShaderResources11() {
    std::unordered_map<ID3D11DeviceContext*, std::unique_ptr<D3D11PerContextState>> retired;
    {
        std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
        // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order) - order is irrelevant
        for (auto& [context, entry] : dx11_hook_g_D3D11ContextStates) {
            (void)context;
            dx11_hook_g_RetiredSamplerCost11.Add(entry->af.cost.Snapshot());
        }
        retired.swap(dx11_hook_g_D3D11ContextStates);
        dx11_hook_g_D3D11DirtyContextCount.store(0, std::memory_order_release);
    }
    retired.clear();
}

ce::d3d11_af::CostSnapshot DX11Hook_SamplerCostTotalsForTesting() {
    std::lock_guard<std::mutex> lock(dx11_hook_g_D3D11ContextStateMutex);
    ce::d3d11_af::CostSnapshot total = dx11_hook_g_RetiredSamplerCost11;
    // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order) - summing is order-independent
    for (const auto& [context, entry] : dx11_hook_g_D3D11ContextStates) {
        (void)context;
        total.Add(entry->af.cost.Snapshot());
    }
    return total;
}
