#include "tests/flow/d3d11_af_fixture.h"

#include <atomic>
#include <thread>

// Forced AF through the engine both D3D11 paths share (hook/d3d11/d3d11_af_engine.h), on a
// real WARP device: the raw context hooks (a translator's or late-attached context CE never
// wraps) and CE's context wrapper (a device the game created through CE's D3D11CreateDevice).
// Every scenario runs on both, because the two paths used to diverge.
namespace {
using Microsoft::WRL::ComPtr;
using ce::flow::af::AFFixture;
using ce::flow::af::ExpectAF;
using ce::flow::af::ExpectOriginal;
using ce::flow::af::Path;
using ce::flow::af::ReplacementStats;

// Per-texture AF from memoized choices: classified once, then one cached lookup per change.
void RunMemoizedChoices(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    af.Bind(af.linear.Get());
    af.DrawWith(af.mipmapped.Get());
    ExpectAF(af.Physical(), 16, "mipmapped");
    af.DrawWith(af.singleMip.Get());
    ExpectOriginal(af.Physical(), af.linear.Get(), "single mip");

    const CEFlowD3D11SamplerCost warm = af.Cost();
    EXPECT_GE(warm.slowSlots, 2u) << "both verdicts classify once";
    constexpr unsigned kDraws = 64;
    for (unsigned draw = 0; draw < kDraws; ++draw) {
        const bool forced = draw % 2 == 0;
        af.DrawWith(forced ? af.mipmapped.Get() : af.singleMip.Get());
        const D3D11_SAMPLER_DESC bound = af.Physical();
        EXPECT_EQ(bound.Filter, forced ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER_MIN_MAG_MIP_LINEAR) << draw;
    }
    const CEFlowD3D11SamplerCost steady = af.Cost();
    EXPECT_EQ(steady.slowSlots, warm.slowSlots) << "memoized choices must not reclassify";
    EXPECT_EQ(steady.fastSlots - warm.fastSlots, kDraws);
    EXPECT_EQ(steady.rebinds - warm.rebinds, kDraws);
    EXPECT_EQ(steady.passes - warm.passes, kDraws);
    EXPECT_EQ(steady.viewClasses - warm.viewClasses, kDraws) << "one cached class read per changed texture";

    // Games rebind samplers per draw (HOTS: hundreds per frame). Each logical sampler keeps its
    // own choice, so alternating two samplers in one slot reclassifies neither.
    ID3D11SamplerState* const samplers[2] = {af.linear.Get(), af.clamp.Get()};
    af.Bind(samplers[1]);
    af.DrawWith(af.mipmapped.Get());
    af.DrawWith(af.singleMip.Get());
    const CEFlowD3D11SamplerCost beforeAlternation = af.Cost();
    for (unsigned draw = 0; draw < kDraws; ++draw) {
        const int which = static_cast<int>(draw % 2);
        const bool forced = (draw / 2) % 2 == 0;
        af.Bind(samplers[which]);
        af.DrawWith(forced ? af.mipmapped.Get() : af.singleMip.Get());
        const D3D11_SAMPLER_DESC bound = af.Physical();
        EXPECT_EQ(bound.Filter, forced ? D3D11_FILTER_ANISOTROPIC : D3D11_FILTER_MIN_MAG_MIP_LINEAR) << draw;
        EXPECT_EQ(bound.AddressU, which ? D3D11_TEXTURE_ADDRESS_CLAMP : D3D11_TEXTURE_ADDRESS_WRAP) << draw;
    }
    const CEFlowD3D11SamplerCost alternated = af.Cost();
    EXPECT_EQ(alternated.slowSlots, beforeAlternation.slowSlots) << "alternating samplers keep their choices";
    EXPECT_EQ(alternated.evictions, 0u);

    // Other stages are not tracked: their binds never reach CE's state.
    af.Bind(af.linear.Get());
    af.DrawWith(af.mipmapped.Get());
    const CEFlowD3D11SamplerCost beforeOtherStages = af.Cost();
    for (unsigned bind = 0; bind < 8; ++bind) {
        ID3D11ShaderResourceView* view = bind % 2 ? af.mipmapped.Get() : af.singleMip.Get();
        ID3D11SamplerState* sampler = af.linear.Get();
        af.context->VSSetShaderResources(0, 1, &view);
        af.context->CSSetSamplers(0, 1, &sampler);
    }
    const CEFlowD3D11SamplerCost afterOtherStages = af.Cost();
    EXPECT_EQ(afterOtherStages.srvBinds, beforeOtherStages.srvBinds);
    EXPECT_EQ(afterOtherStages.samplerBinds, beforeOtherStages.samplerBinds);

    // A sampler bind that changes nothing physical is absorbed without a bind.
    const uint64_t rebindsBefore = af.Cost().rebinds;
    af.Bind(af.linear.Get());
    EXPECT_EQ(af.Cost().rebinds, rebindsBefore);
    ExpectAF(af.Physical(), 16, "absorbed bind");
}

// A config change applies at the next draw, without the game changing any binding.
void RunLiveConfigChange(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    af.Bind(af.linear.Get());
    af.DrawWith(af.mipmapped.Get());
    ExpectAF(af.Physical(), 16, "16x");
    af.SetAF("4x");
    af.context->Draw(3, 0);
    ExpectAF(af.Physical(), 4, "4x without a rebind");
    // Turning forced AF off restores the game's own sampler, also without a rebind.
    af.SetAF("default");
    af.context->Draw(3, 0);
    ExpectOriginal(af.Physical(), af.linear.Get(), "AF off without a rebind");
    af.SetAF("16x");
    af.context->Draw(3, 0);
    ExpectAF(af.Physical(), 16, "AF back on");
}

// ClearState unbinds everything behind CE's back. The raw hooks did not see it, kept
// believing the replacement was bound, and swallowed the game's rebind of the same sampler:
// the real slot stayed empty.
void RunClearStateRebind(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    af.Bind(af.linear.Get());
    af.DrawWith(af.mipmapped.Get());
    ExpectAF(af.Physical(), 16, "before ClearState");
    af.context->ClearState();
    EXPECT_EQ(af.Physical().MaxAnisotropy, 0u) << "ClearState leaves the slot empty";
    af.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    af.context->VSSetShader(af.vs.Get(), nullptr, 0);
    af.context->PSSetShader(af.ps.Get(), nullptr, 0);
    af.Bind(af.linear.Get());
    af.DrawWith(af.mipmapped.Get());
    ExpectAF(af.Physical(), 16, "same sampler rebound after ClearState");
}

// The game reads back its own sampler, never CE's replacement.
void RunReadBackIsTheGamesSampler(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    af.Bind(af.linear.Get());
    af.DrawWith(af.mipmapped.Get());
    ExpectAF(af.Physical(), 16, "physical");
    ComPtr<ID3D11SamplerState> readBack;
    af.context->PSGetSamplers(0, 1, &readBack);
    EXPECT_EQ(readBack.Get(), af.linear.Get());
}

// BioShock Infinite creates a sampler per draw for each streaming texture (MinLOD clamp, fade
// bias) and releases it after the bind. CE kept the forced-AF replacement of every such
// descriptor until the config changed, the device ran out of its 4096 unique sampler objects
// and the game's own CreateSamplerState failed with D3D11_ERROR_TOO_MANY_UNIQUE_STATE_OBJECTS;
// its purge-and-retry could free none of CE's and it crashed (session 20261017_135632). Every
// one of those samplers still gets AF with its own bias and LOD clamp.
void RunPerDrawSamplersDoNotExhaustTheDevice(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    const CEFlowD3D11ReplacementStats before = ReplacementStats();
    constexpr UINT kSamplers = 2 * D3D11_REQ_SAMPLER_OBJECT_COUNT_PER_DEVICE;
    constexpr UINT kDrawsPerFrame = 64;
    for (UINT i = 0; i < kSamplers; ++i) {
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized test descriptor
        D3D11_SAMPLER_DESC desc = {};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU = desc.AddressV = desc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
        desc.MaxAnisotropy = 1;
        desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        desc.MipLODBias = static_cast<float>(i) / 1024.0f;  // every descriptor distinct
        desc.MinLOD = static_cast<float>(i % 3);
        desc.MaxLOD = FLT_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        const HRESULT hr = af.device->CreateSamplerState(&desc, &sampler);
        ASSERT_TRUE(SUCCEEDED(hr) && sampler) << "sampler " << i << " hr=0x" << std::hex << hr << std::dec
                                              << " entries=" << ReplacementStats().entries;
        af.Bind(sampler.Get());
        af.DrawWith(af.mipmapped.Get());
        if (i % kDrawsPerFrame == kDrawsPerFrame - 1) {
            // The runtime destroys a released state object at the next flush, as a game's Present does.
            af.context->Flush();
        }
        if (i % 1024 == 0 || i + 1 == kSamplers) {
            const D3D11_SAMPLER_DESC bound = af.Physical();
            ExpectAF(bound, 16, "per-draw sampler");
            EXPECT_FLOAT_EQ(bound.MipLODBias, desc.MipLODBias) << i;
            EXPECT_FLOAT_EQ(bound.MinLOD, desc.MinLOD) << i;
        }
    }
    const CEFlowD3D11ReplacementStats after = ReplacementStats();
    EXPECT_EQ(after.created - before.created, kSamplers) << "one replacement per distinct descriptor";
    EXPECT_EQ(after.createFailures, before.createFailures);
    EXPECT_EQ(after.registerFailures, before.registerFailures);
    // What survives is CE's bounded choice memo (128 entries per context) plus the bound slot.
    EXPECT_LE(after.entries, before.entries + 129u) << "replacements must die with the game's samplers";
}

// A live game sampler keeps its replacement for every context, so another context's first bind
// reuses it instead of creating one; a config change lets go of every replacement at once.
void RunReplacementsFollowTheGameSampler(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    const CEFlowD3D11ReplacementStats before = ReplacementStats();
    af.Bind(af.clamp.Get());
    af.DrawWith(af.mipmapped.Get());
    af.Bind(af.linear.Get());
    af.DrawWith(af.mipmapped.Get());
    ExpectAF(af.Physical(), 16, "immediate");
    const CEFlowD3D11ReplacementStats first = ReplacementStats();
    EXPECT_EQ(first.created - before.created, 2u);
    EXPECT_EQ(first.entries, before.entries + 2u);

    ComPtr<ID3D11DeviceContext> deferred;
    ASSERT_TRUE(SUCCEEDED(af.device->CreateDeferredContext(0, &deferred)) && deferred);
    deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    deferred->VSSetShader(af.vs.Get(), nullptr, 0);
    deferred->PSSetShader(af.ps.Get(), nullptr, 0);
    ID3D11SamplerState* sampler = af.linear.Get();
    ID3D11ShaderResourceView* view = af.mipmapped.Get();
    deferred->PSSetSamplers(0, 1, &sampler);
    deferred->PSSetShaderResources(0, 1, &view);
    deferred->Draw(3, 0);
    const CEFlowD3D11ReplacementStats shared = ReplacementStats();
    EXPECT_EQ(shared.created, first.created) << "the other context reuses the registered replacement";
    EXPECT_EQ(shared.hits - first.hits, 1u);
    ComPtr<ID3D11CommandList> commands;
    ASSERT_TRUE(SUCCEEDED(deferred->FinishCommandList(FALSE, &commands)));

    // The epoch moves at the next draw; the clamp sampler's 16x replacement goes with it.
    af.SetAF("4x");
    af.context->Draw(3, 0);
    ExpectAF(af.Physical(), 4, "4x after the epoch drop");
    const CEFlowD3D11ReplacementStats changed = ReplacementStats();
    EXPECT_EQ(changed.created - shared.created, 1u);
    EXPECT_EQ(changed.entries, before.entries + 1u) << "a new epoch drops every registered replacement";
}

TEST(FlowD3D11SamplerAF, RawReplacementsFollowTheGameSampler) {
    RunReplacementsFollowTheGameSampler(Path::kRaw);
}
TEST(FlowD3D11SamplerAF, WrapperReplacementsFollowTheGameSampler) {
    RunReplacementsFollowTheGameSampler(Path::kWrapper);
}

TEST(FlowD3D11SamplerAF, RawPerDrawSamplersDoNotExhaustTheDevice) {
    RunPerDrawSamplersDoNotExhaustTheDevice(Path::kRaw);
}
TEST(FlowD3D11SamplerAF, WrapperPerDrawSamplersDoNotExhaustTheDevice) {
    RunPerDrawSamplersDoNotExhaustTheDevice(Path::kWrapper);
}

// Another thread's drop of the raw path's per-context state (resize, host disconnect, CE
// shutdown) runs inside the exact window under test: flow-only probes in the raw state step
// call it between this thread's cached slot lookup and taking the slot, or while the slot is
// held. No timing is involved.
struct RawSlotProbes {
    using Probe = void (*)(ID3D11DeviceContext*);
    void (*set)(Probe, Probe) = nullptr;
    void (*dropViews)() = nullptr;
    void (*dropAll)() = nullptr;
    uint64_t (*retries)() = nullptr;
    uint64_t (*incarnation)(ID3D11DeviceContext*) = nullptr;

    RawSlotProbes() {
        const HMODULE hook = GetModuleHandleW(L"capture_hook_x64.dll");
        set = reinterpret_cast<void (*)(Probe, Probe)>(GetProcAddress(hook, "CEFlow_SetRawAFProbes"));
        dropViews = reinterpret_cast<void (*)()>(GetProcAddress(hook, "CEFlow_ReleaseTrackedD3D11Views"));
        dropAll = reinterpret_cast<void (*)()>(GetProcAddress(hook, "CEFlow_ReleaseAllTrackedD3D11States"));
        retries = reinterpret_cast<uint64_t (*)()>(GetProcAddress(hook, "CEFlow_RawAFStaleSlotRetries"));
        incarnation = reinterpret_cast<uint64_t (*)(ID3D11DeviceContext*)>(
            GetProcAddress(hook, "CEFlow_RawAFSlotIncarnation"));
    }
    bool Ready() const { return set && dropViews && dropAll && retries && incarnation; }
    ~RawSlotProbes() {
        if (set) {
            set(nullptr, nullptr);
        }
    }
};

RawSlotProbes* g_probes = nullptr;
std::atomic<int> g_probeCalls{0};
std::thread g_dropper;

ULONG References(IUnknown* object) {
    object->AddRef();
    return object->Release();
}

// The state is dropped (and its slot recycled) after this thread found the slot in its
// cache: the step must notice once it holds the slot and start over on a fresh state.
TEST(FlowD3D11SamplerAF, RawStateDroppedBehindTheCachedLookupIsLookedUpAgain) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, Path::kRaw);
    ASSERT_TRUE(af.Ready());
    RawSlotProbes probes;
    ASSERT_TRUE(probes.Ready());
    g_probes = &probes;
    af.Bind(af.linear.Get());
    af.DrawWith(af.mipmapped.Get());
    ExpectAF(af.Physical(), 16, "before");
    const uint64_t retriesBefore = probes.retries();
    const uint64_t incarnationBefore = probes.incarnation(af.context.Get());
    g_probeCalls = 0;
    probes.set(
        [](ID3D11DeviceContext*) {
            if (g_probeCalls.fetch_add(1) == 0) {
                std::thread([] { g_probes->dropAll(); }).join();
            }
        },
        nullptr);
    af.Bind(af.clamp.Get());  // its first state step finds the cached slot recycled
    probes.set(nullptr, nullptr);
    EXPECT_GE(g_probeCalls.load(), 1);
    EXPECT_GT(probes.retries(), retriesBefore) << "the stale slot must be detected, not used";
    EXPECT_NE(probes.incarnation(af.context.Get()), incarnationBefore);
    af.DrawWith(af.mipmapped.Get());
    const D3D11_SAMPLER_DESC bound = af.Physical();
    ExpectAF(bound, 16, "after the drop");
    EXPECT_EQ(bound.AddressU, D3D11_TEXTURE_ADDRESS_CLAMP) << "the bind made across the drop is the game's";
    g_probes = nullptr;
}

// A resize's view drop from another thread lands exactly between a sampler bind's tracking
// step and its binding step: started while the tracking step holds the slot (so it waits for
// it), joined at the next step's lookup. A whole-state drop there lost the game's bind. Only an
// eager bind has a binding step (slot hooks); a deferred one is covered below.
std::atomic<int> g_lookupCalls{0};
TEST(FlowD3D11SamplerAF, RawViewDropBetweenTrackingAndBindingKeepsTheBind) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, Path::kRawSlotHooks);
    ASSERT_TRUE(af.Ready());
    RawSlotProbes probes;
    ASSERT_TRUE(probes.Ready());
    g_probes = &probes;
    af.Bind(af.linear.Get());
    af.DrawWith(af.mipmapped.Get());
    g_probeCalls = 0;
    g_lookupCalls = 0;
    probes.set(
        [](ID3D11DeviceContext*) {
            // Call 0 is the tracking step's lookup, call 1 the next step's.
            if (g_lookupCalls.fetch_add(1) == 1 && g_dropper.joinable()) {
                g_dropper.join();
            }
        },
        [](ID3D11DeviceContext*) {
            if (g_probeCalls.fetch_add(1) == 0) {
                g_dropper = std::thread([] { g_probes->dropViews(); });  // waits for the held slot
            }
        });
    af.Bind(af.clamp.Get());
    probes.set(nullptr, nullptr);
    EXPECT_FALSE(g_dropper.joinable()) << "the drop ran inside the window";
    if (g_dropper.joinable()) {
        g_dropper.join();
    }
    const D3D11_SAMPLER_DESC bound = af.Physical();
    ExpectAF(bound, 16, "bind across the view drop");
    EXPECT_EQ(bound.AddressU, D3D11_TEXTURE_ADDRESS_CLAMP);
    g_probes = nullptr;
}

// The deferred form: the resize's view drop lands between the game's sampler bind, which only
// tracks, and the draw that binds it. The pending bind must survive the drop.
TEST(FlowD3D11SamplerAF, RawViewDropKeepsPendingBinds) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, Path::kRaw);
    ASSERT_TRUE(af.Ready());
    RawSlotProbes probes;
    ASSERT_TRUE(probes.Ready());
    af.Bind(af.linear.Get());
    af.DrawWith(af.mipmapped.Get());
    af.Bind(af.clamp.Get());
    EXPECT_EQ(af.Physical().AddressU, D3D11_TEXTURE_ADDRESS_WRAP) << "pending until the draw";
    std::thread([&probes] { probes.dropViews(); }).join();
    af.DrawWith(af.mipmapped.Get());
    const D3D11_SAMPLER_DESC bound = af.Physical();
    ExpectAF(bound, 16, "pending bind across the view drop");
    EXPECT_EQ(bound.AddressU, D3D11_TEXTURE_ADDRESS_CLAMP);
}

// What a resize needs: CE lets go of every view it tracks, and keeps deciding correctly by
// reading the bound views back from the context. A context unused since the previous drop
// (destroyed, as far as CE can tell) is dropped whole.
TEST(FlowD3D11SamplerAF, RawViewDropReleasesViewsAndKeepsDeciding) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, Path::kRaw);
    ASSERT_TRUE(af.Ready());
    RawSlotProbes probes;
    ASSERT_TRUE(probes.Ready());
    af.Bind(af.linear.Get());
    af.DrawWith(af.mipmapped.Get());
    ExpectAF(af.Physical(), 16, "before");
    const ULONG tracked = References(af.mipmapped.Get());
    const uint64_t incarnation = probes.incarnation(af.context.Get());
    ASSERT_NE(incarnation, 0u);
    probes.dropViews();
    EXPECT_EQ(References(af.mipmapped.Get()), tracked - 1) << "CE's view reference must be gone";
    EXPECT_EQ(probes.incarnation(af.context.Get()), incarnation) << "a context in use keeps its state";
    af.context->Draw(3, 0);
    ExpectAF(af.Physical(), 16, "no rebind: the replacement stays");
    af.DrawWith(af.singleMip.Get());
    ExpectOriginal(af.Physical(), af.linear.Get(), "protected texture after the drop");
    af.DrawWith(af.mipmapped.Get());
    ExpectAF(af.Physical(), 16, "eligible texture after the drop");

    probes.dropViews();  // used since the previous drop: views only
    EXPECT_EQ(probes.incarnation(af.context.Get()), incarnation);
    probes.dropViews();  // not used since: dropped whole
    EXPECT_EQ(probes.incarnation(af.context.Get()), 0u);
    af.Bind(af.clamp.Get());
    af.DrawWith(af.mipmapped.Get());
    ExpectAF(af.Physical(), 16, "a dropped context starts over on its next call");
}

TEST(FlowD3D11SamplerAF, MemoizedChoicesKeepPerTextureAFWithoutReclassifying) {
    RunMemoizedChoices(Path::kRaw);
}
TEST(FlowD3D11SamplerAF, WrapperUsesTheSameMemoizedChoices) {
    RunMemoizedChoices(Path::kWrapper);
}
TEST(FlowD3D11SamplerAF, RawConfigChangeAppliesWithoutARebind) {
    RunLiveConfigChange(Path::kRaw);
}
TEST(FlowD3D11SamplerAF, WrapperConfigChangeAppliesWithoutARebind) {
    RunLiveConfigChange(Path::kWrapper);
}

void RunLiveBiasWithAFChanges(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    af.SetMipBias("1");
    auto sampler = af.CreateSampler(D3D11_TEXTURE_ADDRESS_MIRROR);
    ASSERT_TRUE(sampler);
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized test descriptor
    D3D11_SAMPLER_DESC logical = {};
    sampler->GetDesc(&logical);
    ASSERT_FLOAT_EQ(logical.MipLODBias, 1.0f);
    af.Bind(sampler.Get());
    af.DrawWith(af.mipmapped.Get());
    EXPECT_FLOAT_EQ(af.Physical().MipLODBias, 1.0f);
    for (const char* level : {"4x", "off", "16x"}) {
        af.SetAF(level);
        af.context->Draw(3, 0);  // no game rebind: the epoch alone must refresh the choice
        EXPECT_FLOAT_EQ(af.Physical().MipLODBias, 1.0f) << level;
        if (std::strcmp(level, "off") != 0)
            ExpectAF(af.Physical(), std::strcmp(level, "4x") == 0 ? 4u : 16u, level);
        else
            EXPECT_EQ(af.Physical().Filter, logical.Filter);
    }
    // A changed or removed offset also starts from the application's descriptor,
    // whose bias was zero, instead of the logical object's creation-adjusted bias.
    af.SetMipBias("2");
    af.context->Draw(3, 0);
    EXPECT_FLOAT_EQ(af.Physical().MipLODBias, 2.0f);
    af.SetMipBias("default");
    af.context->Draw(3, 0);
    EXPECT_FLOAT_EQ(af.Physical().MipLODBias, 0.0f);
    sampler->GetDesc(&logical);
    EXPECT_FLOAT_EQ(logical.MipLODBias, 1.0f) << "the game's immutable object stays transparent";
    af.SetMipBias("1");  // restore the creation policy: the memo must not retain a later bias
    af.context->Draw(3, 0);
    EXPECT_FLOAT_EQ(af.Physical().MipLODBias, 1.0f);
}

TEST(FlowD3D11SamplerAF, RawAFChangesDoNotReapplyCreationBias) {
    RunLiveBiasWithAFChanges(Path::kRaw);
}
TEST(FlowD3D11SamplerAF, WrapperAFChangesDoNotReapplyCreationBias) {
    RunLiveBiasWithAFChanges(Path::kWrapper);
}
TEST(FlowD3D11SamplerAF, RawClearStateDoesNotSwallowTheNextBind) {
    RunClearStateRebind(Path::kRaw);
}
TEST(FlowD3D11SamplerAF, WrapperClearStateDoesNotSwallowTheNextBind) {
    RunClearStateRebind(Path::kWrapper);
}
TEST(FlowD3D11SamplerAF, RawReadBackIsTheGamesSampler) {
    RunReadBackIsTheGamesSampler(Path::kRaw);
}
TEST(FlowD3D11SamplerAF, WrapperReadBackIsTheGamesSampler) {
    RunReadBackIsTheGamesSampler(Path::kWrapper);
}

}  // namespace
