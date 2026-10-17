#pragma once

// One forced-AF state machine for D3D11 pixel samplers, shared by CE's context
// wrapper (CWrapD3D11DeviceContext) and the raw context vtable hooks.
//
// The two paths used to carry separate implementations of the same policy, and
// only one of them got each optimization: the sampler-keyed choice memo landed in
// the raw hooks for Heroes of the Storm, while BioShock Infinite, whose contexts
// CE wraps, kept the per-slot variant lookups (~0.8 ms/frame, 20261017_003122).
// The raw hooks in turn lacked what only the wrapper did: ClearState and
// command-list boundaries, PSGetSamplers transparency, draw evidence.
//
// The engine owns no lock. A D3D11 context is single-threaded by contract, so
// the wrapper hands its own state straight in; the raw hooks reach a context's
// state through their process map and lock it around each step (Access below).
// Every call that can enter the runtime or a hooked device - classifying a view,
// creating a replacement sampler, binding - runs between those steps, never
// inside one.
//
// Ownership: every pointer in PixelState holds a reference, except a shader's metadata
// handle, which the held shader owns as private data.

#include <d3d11.h>

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstddef>
#include <vector>

#include "hook/overrides/sampler_override_utils.h"
#include "hook/overrides/sampler_override/shader_af_plan.h"

struct GraphicsConfig;

namespace ce::d3d11_af {

inline constexpr UINT kSamplerSlots = D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT;
inline constexpr UINT kViewSlots = D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT;
inline constexpr uint32_t kAllSamplerSlots = (1u << kSamplerSlots) - 1u;

// The physical sampler forced AF chose for one logical sampler within one
// replacement-cache epoch, for each shader/view verdict.
struct SamplerChoice {
    uint64_t epoch = 0;  // 0: nothing resolved yet
    bool isReplacement = false;
    bool descAllowsAF = false;
    bool resolved[2] = {};
    ID3D11SamplerState* physical[2] = {};  // [AF allowed]; null keeps the logical sampler
};

// Choices per logical sampler object, so a game that alternates samplers in a
// slot reuses them. Each entry holds a reference to its sampler, so its address
// cannot be reused while cached. Bounded; a full probe window evicts its least
// recently used entry.
struct SamplerChoiceCache {
    static constexpr size_t kEntries = 128;
    static constexpr size_t kProbe = 4;
    struct Entry {
        ID3D11SamplerState* logical = nullptr;
        SamplerChoice choice;
        uint64_t stamp = 0;
    };
    Entry entries[kEntries] = {};
    uint64_t clock = 0;
};

// Pixel shaders this context switched to, with their forced-AF metadata, so a game that
// alternates shaders skips the private-data lookup. Each entry holds a reference to its
// shader, so its address cannot be reused while cached; the metadata belongs to the shader.
// Only shaders with metadata are kept. Bounded like SamplerChoiceCache.
struct ShaderPlanCache {
    static constexpr size_t kEntries = 64;
    static constexpr size_t kProbe = 4;
    struct Entry {
        ID3D11PixelShader* shader = nullptr;
        IUnknown* planOwner = nullptr;  // borrowed: the shader's own metadata handle
        uint64_t stamp = 0;
    };
    Entry entries[kEntries] = {};
    uint64_t clock = 0;
};

// Single-writer counter: the context's own thread adds with a plain load and store
// (no locked instruction); the hook thread may read it at any time.
class Count {
public:
    void operator+=(uint64_t amount) {
        value_.store(value_.load(std::memory_order_relaxed) + amount, std::memory_order_relaxed);
    }
    void operator++() { *this += 1; }
    uint64_t Load() const { return value_.load(std::memory_order_relaxed); }

private:
    std::atomic<uint64_t> value_{0};
};

struct CostSnapshot {
    uint64_t srvBinds = 0;
    uint64_t samplerBinds = 0;
    uint64_t shaderBinds = 0;
    uint64_t passes = 0;       // dirty-slot reconciliations (draw or sampler bind)
    uint64_t fastSlots = 0;    // slots resolved from a memoized choice
    uint64_t slowSlots = 0;    // slots that classified sampler, shader or views
    uint64_t viewClasses = 0;  // view AF classes read for a changed texture
    uint64_t rebinds = 0;      // physical PSSetSamplers calls CE issued
    uint64_t timedPasses = 0;  // passes whose duration was sampled
    uint64_t timedTicks = 0;
    uint64_t timedBinds = 0;   // SRV and sampler bind-tracking calls whose duration was sampled
    uint64_t timedBindTicks = 0;
    uint64_t timedShaderBinds = 0;  // PSSetShader calls, metadata lookup included
    uint64_t timedShaderTicks = 0;
    uint64_t choiceEvictions = 0;
    uint64_t shaderRepeats = 0;    // the tracked shader bound again: nothing to do
    uint64_t shaderMemoHits = 0;   // a switch resolved from ShaderPlanCache
    uint64_t shaderLookups = 0;    // a switch that read the shader's private data
    uint64_t shaderEvictions = 0;  // ShaderPlanCache entries evicted

    void Add(const CostSnapshot& other);
    CostSnapshot Since(const CostSnapshot& earlier) const;
    bool Any() const { return srvBinds + samplerBinds + shaderBinds + passes != 0; }
};

// Per-context work of the forced-AF path.
struct CostCounters {
    Count srvBinds;
    Count samplerBinds;
    Count shaderBinds;
    Count passes;
    Count fastSlots;
    Count slowSlots;
    Count viewClasses;
    Count rebinds;
    Count timedPasses;
    Count timedTicks;
    Count timedBinds;
    Count timedBindTicks;
    Count timedShaderBinds;
    Count timedShaderTicks;
    Count choiceEvictions;
    Count shaderRepeats;
    Count shaderMemoHits;
    Count shaderLookups;
    Count shaderEvictions;

    CostSnapshot Snapshot() const;
};

struct PixelState {
    ID3D11ShaderResourceView* views[kViewSlots] = {};
    // Forced-AF class of each tracked view: 0 unknown, else D3D11ForcedAFResourceDecision + 1.
    uint8_t viewClass[kViewSlots] = {};
    ID3D11SamplerState* logical[kSamplerSlots] = {};   // what the game bound
    ID3D11SamplerState* physical[kSamplerSlots] = {};  // what the real context holds
    ID3D11PixelShader* shader = nullptr;
    // The shader's metadata handle, owner of *plan. Borrowed: the shader keeps its private data
    // alive, and `shader` is held.
    IUnknown* planOwner = nullptr;
    const ce::sampler_override::D3D11ShaderAFPlan* plan = nullptr;  // never null once constructed
    bool shaderTracked = false;     // a PSSetShader (or refresh) set `shader`, null included
    uint32_t dirtyMask = 0;
    uint64_t epoch = 0;             // replacement epoch the masks account for
    SamplerChoiceCache choices;
    ShaderPlanCache shaders;
    CostCounters cost;

    PixelState();
    ~PixelState();
    PixelState(const PixelState&) = delete;
    PixelState& operator=(const PixelState&) = delete;
};

struct Policy {
    const GraphicsConfig* gfx = nullptr;
    uint64_t epoch = 0;
    uint64_t hash = 0;    // of the sampler settings in *gfx, matched against creation tags
    bool active = false;  // any D3D11 sampler override configured
};

// Cheap: a config-version check and the replacement-cache epoch.
Policy AcquirePolicy();
bool OverridesActive();  // one relaxed load

// What the engine needs from the context it serves.
class Host {
public:
    // Binds to the real context, bypassing CE's own interception.
    virtual void BindPhysical(UINT startSlot, UINT count, ID3D11SamplerState* const* samplers) = 0;
    virtual ID3D11PixelShader* AcquireBoundShader() = 0;           // acquired, may be null
    virtual ID3D11ShaderResourceView* AcquireBoundView(UINT slot) = 0;  // acquired, may be null
    virtual ID3D11Device* AcquireDevice() = 0;                     // acquired real device
    // Creates a replacement without entering CE's CreateSamplerState override.
    virtual HRESULT CreateReplacement(ID3D11Device* device, const D3D11_SAMPLER_DESC& desc,
                                      ID3D11SamplerState** sampler) = 0;
    virtual const char* PathName() const = 0;  // "raw" or "wrapper", for logs

protected:
    ~Host() = default;
};

// ---- Sampler creation (both paths' CreateSamplerState) ----

// Creates through `create` (the caller's un-intercepted CreateSamplerState) with CE's
// creation-time sampler policy - AF off, mip mapping, bias - and tags the sampler, so the
// bind-time engine later changes only the resource-dependent AF decision. Forced AF itself
// is never applied here: creation knows neither the shader nor the texture.
using CreateSamplerFn = HRESULT (*)(void* context, const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** sampler);
HRESULT CreateSamplerWithCreationPolicy(const D3D11_SAMPLER_DESC* desc, ID3D11SamplerState** sampler,
                                        CreateSamplerFn create, void* createContext, const char* path);

// Game sampler creations while overrides are active (d3d11_af_creation.cpp). A kept tag means the
// runtime returned an existing object for an identical descriptor that already carried it. One
// creation in 32 per thread is timed: CE's own work around the runtime's call, and the call.
struct SamplerCreationStats {
    uint64_t creates = 0;
    uint64_t tagsWritten = 0;
    uint64_t tagsKept = 0;
    uint64_t descHits = 0;  // the policy's result came from the thread's memo
    uint64_t tagSkips = 0;  // kept tags the memo vouched for, without reading them back
    uint64_t raced = 0;     // confirmations not memoized because another tag write overlapped
    uint64_t timedCreates = 0;
    uint64_t timedCeTicks = 0;
    uint64_t timedRuntimeTicks = 0;
    size_t threads = 0;  // creating threads with a memo
};
SamplerCreationStats SnapshotSamplerCreationStats();

// ---- Deferred releases ----

// While a scope is open on this thread, the COM releases the state steps make are queued
// and run when it closes. The raw hooks hold a context's slot during those steps, and a
// final release can run arbitrary runtime code, a call back into the same context included.
// The wrapper holds nothing and opens no scope: its releases run at once.
class DeferredReleaseScope {
public:
    DeferredReleaseScope();
    ~DeferredReleaseScope();
    DeferredReleaseScope(const DeferredReleaseScope&) = delete;
    DeferredReleaseScope& operator=(const DeferredReleaseScope&) = delete;

    // Releases one object now, or queues it when a scope is open.
    static void Release(IUnknown* object);

private:
    // One context's ClearState drops at most 128 views, 32 samplers, a shader and its plan;
    // a drop across many contexts spills to the heap.
    static constexpr int kCapacity = 192;
    IUnknown* items_[kCapacity];
    int count_ = 0;
    std::vector<IUnknown*> overflow_;
    DeferredReleaseScope* previous_ = nullptr;
};

// ---- State steps (no locking; the caller owns the state for the call) ----

void ClearState(PixelState& state);
// Lets go of every tracked view (the only references that can make a resize fail) and
// keeps samplers, shader and choices: a view the context still has bound is read back
// from the context the next time a slot needs it.
void DropViews(PixelState& state);
uint32_t TrackedMask(const PixelState& state);
uint32_t OverriddenMask(const PixelState& state);
void MarkDirty(PixelState& state, uint32_t mask);
// A new replacement epoch (config change) reconsiders every slot that is bound or overridden.
void SyncEpoch(PixelState& state, uint64_t epoch);
// overridesActive false: only slots CE overrides need reconsidering after the change.
void TrackViews(PixelState& state, UINT startSlot, UINT count, ID3D11ShaderResourceView* const* views,
                bool overridesActive);
// Returns the slots whose logical sampler changed.
uint32_t TrackSamplers(PixelState& state, UINT startSlot, UINT count, ID3D11SamplerState* const* samplers);
// `planOwner` is acquired by the caller (AcquireShaderPlanOwner) and adopted.
void TrackShader(PixelState& state, ID3D11PixelShader* shader, IUnknown* planOwner, bool overridesActive);
// A shader bind that needs no metadata lookup: the tracked shader again, no shader, or one the
// context's ShaderPlanCache holds. False leaves the state untouched.
bool TrackKnownShader(PixelState& state, ID3D11PixelShader* shader, bool overridesActive);
IUnknown* AcquireShaderPlanOwner(ID3D11PixelShader* shader);  // outside any state lock
// The game sees its own sampler where CE bound a replacement.
void TranslateToLogical(const PixelState& state, UINT startSlot, UINT count, ID3D11SamplerState** samplers);
void RememberPhysical(PixelState& state, UINT slot, ID3D11SamplerState* physical);

// ---- Reconciliation pieces ----

struct PendingBind {
    UINT slot;
    ID3D11SamplerState* physical;  // acquired
    bool remembered;               // state.physical already holds it
};

struct PendingView {
    UINT textureSlot;
    ID3D11ShaderResourceView* view;  // acquired
    ce::sampler_override::D3D11ForcedAFResourceDecision decision;
};

// Trivial on purpose: a pass touches only the entries it fills.
struct Batch {
    PendingBind binds[kSamplerSlots];
    PendingView views[kViewSlots];
    int bindCount = 0;
    int viewCount = 0;
    uint32_t viewPendingMask = 0;
    uint32_t slowMask = 0;
};

// Resolves `slots` from tracked state and memoized choices: queues a bind where
// the physical sampler differs, collects views of unknown class when
// `collectViews`, and marks what needs the slow path.
void ResolveFast(PixelState& state, uint32_t slots, const Policy& policy, bool bindEmptySlots, bool collectViews,
                 Batch& batch);
void ClassifyViews(Batch& batch);  // no state; private data / descriptors of the collected views
void ApplyViewClasses(PixelState& state, Batch& batch);
void ReleaseViews(Batch& batch);

// Inputs of one slow slot, copied out of the state so classification runs without it.
struct SlowSlot {
    UINT slot = 0;
    ID3D11SamplerState* logical = nullptr;  // acquired
    bool needsShader = false;               // the shader was never tracked: refresh from the context
    bool hasShader = false;
    bool hasMetadata = false;
    bool disassembleFailed = false;
    ce::sampler_override::D3D11AFSamplerPlan plan;
    ID3D11ShaderResourceView* views[kViewSlots] = {};  // acquired, the plan's textures only
    ce::sampler_override::D3D11AFTextureMask missingViews;  // planned textures with no tracked view
};
void SnapshotSlowSlot(PixelState& state, UINT slot, SlowSlot& out);
void ReleaseSlowSlot(SlowSlot& slot);

struct ChoiceFacts {
    bool valid = false;
    bool isReplacement = false;
    bool descAllowsAF = false;
    bool allowAF = false;
};
// The physical sampler for the slot, acquired (the logical one when nothing changes).
ID3D11SamplerState* ResolveSlow(Host& host, const SlowSlot& slot, const Policy& policy, ChoiceFacts* facts);
void StoreChoice(PixelState& state, ID3D11SamplerState* logical, uint64_t epoch, const ChoiceFacts& facts,
                 ID3D11SamplerState* physical);
// Queues the slow slot's bind when the real context holds something else.
void QueueSlowBind(PixelState& state, UINT slot, ID3D11SamplerState* physical, Batch& batch);

// ---- Cost reporting ----

// Wrapper contexts register their state so the hook thread can sum them; a state's
// counts are kept when it unregisters.
void RegisterReportedState(const PixelState* state);
void UnregisterReportedState(const PixelState* state);
CostSnapshot SumRegisteredStates(size_t* contexts);
// One line in the format both paths share; nothing when the window had no work.
void LogCostWindow(const char* tag, const CostSnapshot& window, uint64_t elapsedMs, size_t contexts);

// ---- Cost sampling (1 call in 32) ----

// SRV and sampler binds share one sample stream; shader binds have their own, so each kind
// is sampled at its own rate whatever the game's call pattern.
enum class SampleStream { kBinds, kShaders };

class BindSample {
public:
    explicit BindSample(SampleStream stream);
    void Finish(PixelState& state);

private:
    int64_t startTicks_ = 0;
    SampleStream stream_;
};
bool SamplePass(int64_t* startTicks);
void FinishPass(PixelState& state, int64_t startTicks);

void LogReconcile(const Host& host, uint32_t mask, const Batch& batch, int slowSlots);

// ---- Orchestration ----
//
// Access provides:
//   template <class Fn> void Run(Fn&& fn)  - fn(PixelState&) with the context's state held exclusively
//                                            (created empty on first use);
//   bool IsClean(uint64_t epoch)           - true when no slot can need work, without touching the
//                                            state's lock: the per-draw exit.
// The wrapper's access is its member; the raw hooks lock their map.

// Reconciles the dirty slots in `rangeMask`. `consumedMask` receives the slots taken.
template <class Access>
int Reconcile(Access& access, Host& host, const Policy& policy, uint32_t rangeMask, bool bindEmptySlots,
              uint32_t* consumedMask) {
    if (consumedMask) {
        *consumedMask = 0;
    }
    int64_t startTicks = 0;
    const bool timed = SamplePass(&startTicks);
    Batch batch;
    uint32_t mask = 0;
    access.Run([&](PixelState& state) {
        SyncEpoch(state, policy.epoch);
        mask = state.dirtyMask & rangeMask;
        state.dirtyMask &= ~mask;
        // With no override configured only CE's own earlier replacements need undoing.
        if (!policy.active) {
            mask &= OverriddenMask(state);
        }
        if (mask != 0) {
            ++state.cost.passes;
            ResolveFast(state, mask, policy, bindEmptySlots, true, batch);
        }
    });
    if (mask == 0) {
        return 0;
    }
    if (batch.viewCount != 0) {
        ClassifyViews(batch);
        access.Run([&](PixelState& state) {
            ApplyViewClasses(state, batch);
            ResolveFast(state, batch.viewPendingMask, policy, bindEmptySlots, false, batch);
        });
        ReleaseViews(batch);
    }
    int slowSlots = 0;
    for (uint32_t pending = batch.slowMask; pending != 0; pending &= pending - 1) {
        const UINT slot = static_cast<UINT>(std::countr_zero(pending));
        SlowSlot input;
        access.Run([&](PixelState& state) { SnapshotSlowSlot(state, slot, input); });
        if (input.needsShader) {
            // Never tracked (CE attached after the game set it): read it from the context once.
            ID3D11PixelShader* shader = host.AcquireBoundShader();
            IUnknown* planOwner = AcquireShaderPlanOwner(shader);
            ReleaseSlowSlot(input);
            access.Run([&](PixelState& state) {
                TrackShader(state, shader, planOwner, policy.active);
                SnapshotSlowSlot(state, slot, input);
            });
            if (shader) {
                shader->Release();
            }
        }
        for (auto missing = input.missingViews; missing.First() != UINT_MAX;) {
            const UINT textureSlot = missing.TakeFirst();
            ID3D11ShaderResourceView* view = host.AcquireBoundView(textureSlot);
            access.Run([&](PixelState& state) { TrackViews(state, textureSlot, 1, &view, policy.active); });
            input.views[textureSlot] = view;  // the snapshot keeps the acquired reference
        }
        if (!input.logical && !bindEmptySlots) {
            ReleaseSlowSlot(input);
            continue;
        }
        ++slowSlots;
        ChoiceFacts facts;
        ID3D11SamplerState* physical = input.logical ? ResolveSlow(host, input, policy, &facts) : nullptr;
        access.Run([&](PixelState& state) {
            if (input.logical) {
                StoreChoice(state, input.logical, policy.epoch, facts, physical);
            }
            QueueSlowBind(state, slot, physical, batch);
        });
        if (physical) {
            physical->Release();
        }
        ReleaseSlowSlot(input);
    }
    bool unremembered = false;
    for (int i = 0; i < batch.bindCount; ++i) {
        host.BindPhysical(batch.binds[i].slot, 1, &batch.binds[i].physical);
        unremembered = unremembered || !batch.binds[i].remembered;
    }
    if (unremembered || slowSlots != 0 || timed) {
        access.Run([&](PixelState& state) {
            for (int i = 0; i < batch.bindCount; ++i) {
                if (!batch.binds[i].remembered) {
                    RememberPhysical(state, batch.binds[i].slot, batch.binds[i].physical);
                    ++state.cost.rebinds;
                }
            }
            state.cost.slowSlots += static_cast<uint64_t>(slowSlots);
            if (timed) {
                FinishPass(state, startTicks);
            }
        });
    }
    LogReconcile(host, mask, batch, slowSlots);
    for (int i = 0; i < batch.bindCount; ++i) {
        if (batch.binds[i].physical) {
            batch.binds[i].physical->Release();
        }
    }
    if (consumedMask) {
        *consumedMask = mask;
    }
    return batch.bindCount;
}

template <class Access>
void OnViews(Access& access, UINT startSlot, UINT count, ID3D11ShaderResourceView* const* views) {
    if (startSlot >= kViewSlots) {
        return;
    }
    const bool active = OverridesActive();
    BindSample sample(SampleStream::kBinds);
    access.Run([&](PixelState& state) {
        ++state.cost.srvBinds;
        TrackViews(state, startSlot, count, views, active);
        sample.Finish(state);
    });
}

template <class Access>
void OnShader(Access& access, ID3D11PixelShader* shader) {
    // Timed from here: the metadata lookup is part of what a shader bind costs.
    BindSample sample(SampleStream::kShaders);
    const bool active = OverridesActive();
    bool known = false;
    access.Run([&](PixelState& state) {
        ++state.cost.shaderBinds;
        known = TrackKnownShader(state, shader, active);
        if (known) {
            sample.Finish(state);
        }
    });
    if (known) {
        return;
    }
    // A shader new to this context. The lookup reads private data; it never runs with the
    // state held.
    IUnknown* planOwner = AcquireShaderPlanOwner(shader);
    access.Run([&](PixelState& state) {
        ++state.cost.shaderLookups;
        TrackShader(state, shader, planOwner, active);
        sample.Finish(state);
    });
}

// Replaces the game's PSSetSamplers: the physical samplers of the range are bound
// instead, and a range already physically correct needs no call at all.
template <class Access>
void OnSamplers(Access& access, Host& host, UINT startSlot, UINT count, ID3D11SamplerState* const* samplers) {
    if (count == 0 || startSlot >= kSamplerSlots) {
        host.BindPhysical(startSlot, count, samplers);
        return;
    }
    const UINT actual = count < kSamplerSlots - startSlot ? count : kSamplerSlots - startSlot;
    const uint32_t rangeMask = (((1u << actual) - 1u) << startSlot) & kAllSamplerSlots;
    const Policy policy = AcquirePolicy();
    BindSample sample(SampleStream::kBinds);
    bool forward = false;
    access.Run([&](PixelState& state) {
        ++state.cost.samplerBinds;
        SyncEpoch(state, policy.epoch);
        // Read before tracking: a slot the game changes differs from its physical sampler
        // until bound, which says nothing about a replacement of CE's being there.
        const uint32_t replacedInRange = OverriddenMask(state) & rangeMask;
        const uint32_t changed = TrackSamplers(state, startSlot, actual, samplers);
        // Nothing to override and nothing of CE's to restore: the game's call is exact.
        forward = !policy.active && replacedInRange == 0;
        if (forward) {
            for (UINT i = 0; i < actual; ++i) {
                RememberPhysical(state, startSlot + i, samplers ? samplers[i] : nullptr);
            }
            state.dirtyMask &= ~rangeMask;
        } else {
            MarkDirty(state, changed);
        }
        sample.Finish(state);
    });
    if (forward) {
        host.BindPhysical(startSlot, count, samplers);
        return;
    }
    Reconcile(access, host, policy, rangeMask, true, nullptr);
}

template <class Access>
void BeforeDraw(Access& access, Host& host) {
    const Policy policy = AcquirePolicy();
    if (access.IsClean(policy.epoch)) {
        return;
    }
    Reconcile(access, host, policy, kAllSamplerSlots, false, nullptr);
}

}  // namespace ce::d3d11_af
