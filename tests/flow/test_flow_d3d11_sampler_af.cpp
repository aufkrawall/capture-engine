#include "tests/flow/flow_test_support.h"

#include "common/ipc/shared_defs.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <atomic>
#include <cfloat>
#include <cstring>
#include <thread>

// Forced AF through the engine both D3D11 paths share (hook/d3d11/d3d11_af_engine.h), on a
// real WARP device: the raw context hooks (a translator's or late-attached context CE never
// wraps) and CE's context wrapper (a device the game created through CE's D3D11CreateDevice).
// Every scenario runs on both, because the two paths used to diverge.
namespace {
using Microsoft::WRL::ComPtr;

constexpr char kShaders[] = R"(
Texture2D t0 : register(t0);
SamplerState s0 : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
V vs(uint id : SV_VertexID) {
    V v;
    v.uv = float2((id << 1) & 2, id & 2);
    v.pos = float4(v.uv * 2 - 1, 0, 1);
    return v;
}
float4 ps(V v) : SV_Target { return t0.Sample(s0, v.uv); }
)";

ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
    const HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto compile = compiler ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler, "D3DCompile"))
                                  : nullptr;
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;
    if (compile)
        compile(kShaders, sizeof(kShaders) - 1, nullptr, nullptr, nullptr, entry, target, 0, 0, &code, &errors);
    return code;
}

ComPtr<ID3D11ShaderResourceView> CreateTextureView(ID3D11Device* device, UINT mipLevels) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = desc.Height = 64;
    desc.MipLevels = mipLevels;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    ComPtr<ID3D11Texture2D> texture;
    ComPtr<ID3D11ShaderResourceView> view;
    if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)))
        device->CreateShaderResourceView(texture.Get(), nullptr, &view);
    return view;
}

enum class Path { kRaw, kWrapper };

// One D3D11 device and context on the requested path, with the hook's test exports.
class AFFixture {
public:
    AFFixture(ce::flow::FlowGame& game, Path path) : memory_(game.CaptureMemory()) {
        const HMODULE hook = GetModuleHandleW(L"capture_hook_x64.dll");
        install_ = reinterpret_cast<bool (*)(ID3D11Device*, ID3D11Device**, ID3D11DeviceContext**)>(
            GetProcAddress(hook, "CEFlow_InstallRawD3D11Hooks"));
        refresh_ = reinterpret_cast<void (*)()>(GetProcAddress(hook, "CEFlow_RefreshGraphicsConfig"));
        cost_ = reinterpret_cast<CEFlow_GetD3D11SamplerCost_t>(GetProcAddress(
            hook, path == Path::kRaw ? "CEFlow_GetD3D11SamplerCost" : "CEFlow_GetD3D11WrapperSamplerCost"));
        physical_ = reinterpret_cast<CEFlow_GetPhysicalPixelSampler_t>(
            GetProcAddress(hook, "CEFlow_GetPhysicalPixelSampler"));
        if (!memory_ || !install_ || !refresh_ || !cost_ || !physical_) {
            return;
        }
        SetAF("16x");
        const HMODULE runtime = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto create =
            runtime ? reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(runtime, "D3D11CreateDevice"))
                    : nullptr;
        // The flow process's own D3D11CreateDevice resolves to CE's wrapper.
        ComPtr<ID3D11Device> created;
        if (!create || FAILED(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                     &created, nullptr, nullptr))) {
            return;
        }
        if (path == Path::kRaw) {
            if (!install_(created.Get(), &device, &context)) {
                return;
            }
        } else {
            device = created;
            device->GetImmediateContext(&context);
        }
        const auto vsCode = Compile("vs", "vs_5_0");
        const auto psCode = Compile("ps", "ps_5_0");
        if (!vsCode || !psCode ||
            FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)) ||
            FAILED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps))) {
            return;
        }
        mipmapped = CreateTextureView(device.Get(), 7);
        singleMip = CreateTextureView(device.Get(), 1);
        linear = CreateSampler(D3D11_TEXTURE_ADDRESS_WRAP);
        clamp = CreateSampler(D3D11_TEXTURE_ADDRESS_CLAMP);
        if (!mipmapped || !singleMip || !linear || !clamp) {
            return;
        }
        context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        context->VSSetShader(vs.Get(), nullptr, 0);
        context->PSSetShader(ps.Get(), nullptr, 0);
        ready_ = true;
    }
    ~AFFixture() {
        if (context) {
            ID3D11ShaderResourceView* noView = nullptr;
            context->PSSetShaderResources(0, 1, &noView);
            context->ClearState();
            context->Flush();
        }
    }

    bool Ready() const { return ready_; }

    void SetAF(const char* level) {
        strcpy_s(memory_->graphicsConfig.anisotropicFiltering, level);
        memory_->configVersion.fetch_add(1, std::memory_order_release);
        refresh_();
    }

    ComPtr<ID3D11SamplerState> CreateSampler(D3D11_TEXTURE_ADDRESS_MODE address) {
        D3D11_SAMPLER_DESC desc = {};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU = desc.AddressV = desc.AddressW = address;
        desc.MaxAnisotropy = 1;
        desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        desc.MaxLOD = FLT_MAX;
        ComPtr<ID3D11SamplerState> sampler;
        device->CreateSamplerState(&desc, &sampler);
        return sampler;
    }

    void Bind(ID3D11SamplerState* sampler) { context->PSSetSamplers(0, 1, &sampler); }
    void DrawWith(ID3D11ShaderResourceView* view) {
        context->PSSetShaderResources(0, 1, &view);
        context->Draw(3, 0);
    }

    // What the real context holds in s0 (CE's replacement where AF applies).
    D3D11_SAMPLER_DESC Physical() const {
        ComPtr<ID3D11SamplerState> sampler;
        D3D11_SAMPLER_DESC desc = {};
        if (physical_(context.Get(), 0, &sampler) && sampler) {
            sampler->GetDesc(&desc);
        }
        return desc;
    }

    CEFlowD3D11SamplerCost Cost() const {
        uint64_t values[8] = {};
        CEFlowD3D11SamplerCost out = {};
        if (cost_(values, 8) == 8u) {
            out = {values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7]};
        }
        return out;
    }

    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ComPtr<ID3D11ShaderResourceView> mipmapped;
    ComPtr<ID3D11ShaderResourceView> singleMip;
    ComPtr<ID3D11SamplerState> linear;
    ComPtr<ID3D11SamplerState> clamp;

private:
    SharedMemoryLayout* memory_ = nullptr;
    bool (*install_)(ID3D11Device*, ID3D11Device**, ID3D11DeviceContext**) = nullptr;
    void (*refresh_)() = nullptr;
    CEFlow_GetD3D11SamplerCost_t cost_ = nullptr;
    CEFlow_GetPhysicalPixelSampler_t physical_ = nullptr;
    bool ready_ = false;
};

void ExpectAF(const D3D11_SAMPLER_DESC& bound, UINT level, const char* where) {
    EXPECT_EQ(bound.Filter, D3D11_FILTER_ANISOTROPIC) << where;
    EXPECT_EQ(bound.MaxAnisotropy, level) << where;
}

// The game's own sampler as the runtime reports it (CE's creation policy may normalize fields).
void ExpectOriginal(const D3D11_SAMPLER_DESC& bound, ID3D11SamplerState* own, const char* where) {
    D3D11_SAMPLER_DESC created = {};
    own->GetDesc(&created);
    EXPECT_EQ(bound.Filter, created.Filter) << where;
    EXPECT_EQ(bound.MaxAnisotropy, created.MaxAnisotropy) << where;
    EXPECT_EQ(bound.AddressU, created.AddressU) << where;
}

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
// it), joined at the next step's lookup. A whole-state drop there lost the game's bind.
std::atomic<int> g_lookupCalls{0};
TEST(FlowD3D11SamplerAF, RawViewDropBetweenTrackingAndBindingKeepsTheBind) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, Path::kRaw);
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
