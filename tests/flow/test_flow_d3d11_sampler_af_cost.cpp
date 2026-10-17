#include "tests/flow/d3d11_af_fixture.h"

#include <atomic>
#include <cstdio>
#include <thread>
#include <string>
#include <vector>

// The work forced AF does on the game's render thread (hook/d3d11/d3d11_af_engine.h), on a real
// WARP device and both paths the engine serves: what the cost lines sample, and the shortcuts
// that keep repeated binds and creations cheap. The decisions themselves are covered in
// test_flow_d3d11_sampler_af.cpp.
namespace {
using ce::flow::af::AFFixture;
using ce::flow::af::ExpectAF;
using ce::flow::af::Path;
using Microsoft::WRL::ComPtr;

CEFlowD3D11SamplerCreationStats CreationStats() {
    const auto read = reinterpret_cast<CEFlow_GetD3D11SamplerCost_t>(
        GetProcAddress(ce::flow::af::FlowHook(), "CEFlow_GetD3D11SamplerCreationStats"));
    constexpr uint32_t kValues = static_cast<uint32_t>(sizeof(CEFlowD3D11SamplerCreationStats) / sizeof(uint64_t));
    uint64_t values[kValues] = {};
    CEFlowD3D11SamplerCreationStats out = {};
    if (read && read(values, kValues) == kValues) {
        std::memcpy(&out, values, sizeof(out));
    }
    return out;
}

// Shader binds have their own 1-in-32 sample stream, which starts before the shader's metadata
// lookup: that private-data read used to run outside every sample, so the cost line never saw
// it. SRV and sampler binds keep theirs. 64 consecutive calls on one thread hold exactly two
// multiples of 32, whatever the stream's position.
void RunShaderBindsAreSampledOnTheirOwn(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    const CEFlowD3D11SamplerCost before = af.Cost();
    for (int i = 0; i < 64; ++i) {
        af.context->PSSetShader(i % 2 ? af.ps.Get() : af.ps2.Get(), nullptr, 0);
    }
    const CEFlowD3D11SamplerCost shaders = af.Cost();
    EXPECT_EQ(shaders.shaderBinds - before.shaderBinds, 64u);
    EXPECT_EQ(shaders.timedShaderBinds - before.timedShaderBinds, 2u);
    EXPECT_EQ(shaders.timedBinds, before.timedBinds) << "shader binds must not consume the bind stream";
    for (int i = 0; i < 32; ++i) {
        af.DrawWith(i % 2 ? af.mipmapped.Get() : af.singleMip.Get());
        af.Bind(i % 2 ? af.linear.Get() : af.clamp.Get());
    }
    const CEFlowD3D11SamplerCost binds = af.Cost();
    EXPECT_EQ(binds.timedBinds - shaders.timedBinds, 2u);
    EXPECT_EQ(binds.timedShaderBinds, shaders.timedShaderBinds);
}

TEST(FlowD3D11SamplerAFCost, RawShaderBindsAreSampledOnTheirOwn) {
    RunShaderBindsAreSampledOnTheirOwn(Path::kRaw);
}
TEST(FlowD3D11SamplerAFCost, WrapperShaderBindsAreSampledOnTheirOwn) {
    RunShaderBindsAreSampledOnTheirOwn(Path::kWrapper);
}

// The game's sampler creations are sampled too (BioShock Infinite: ~157k per second, all of
// them outside the old cost line). An identical descriptor returns the existing, tagged object.
void RunSamplerCreationsAreSampled(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    const CEFlowD3D11SamplerCreationStats before = CreationStats();
    for (int i = 0; i < 64; ++i) {
        ComPtr<ID3D11SamplerState> sampler = af.CreateSampler(D3D11_TEXTURE_ADDRESS_WRAP);
        ASSERT_EQ(sampler.Get(), af.linear.Get()) << "the runtime hands out the existing object";
    }
    const CEFlowD3D11SamplerCreationStats after = CreationStats();
    EXPECT_EQ(after.creates - before.creates, 64u);
    EXPECT_EQ(after.tagsKept - before.tagsKept, 64u);
    EXPECT_EQ(after.tagsWritten, before.tagsWritten);
    EXPECT_EQ(after.timedCreates - before.timedCreates, 2u);
    af.Bind(af.linear.Get());
    af.DrawWith(af.mipmapped.Get());
    ExpectAF(af.Physical(), 16, "the kept object still gets AF");
}

ULONG References(IUnknown* object) {
    object->AddRef();
    return object->Release();
}

// A game binds the same pixel shader again and again, or alternates a few (BioShock Infinite:
// up to 180k shader binds per second). Every PSSetShader used to read the shader's private
// data first; now only a shader new to the context does, and the decisions are unchanged.
void RunShaderSwitchesSkipTheMetadataLookup(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    af.context->PSSetShader(af.ps2.Get(), nullptr, 0);
    af.context->PSSetShader(af.ps.Get(), nullptr, 0);
    const CEFlowD3D11SamplerCost warm = af.Cost();
    for (int i = 0; i < 64; ++i) {
        af.context->PSSetShader(af.ps.Get(), nullptr, 0);
    }
    const CEFlowD3D11SamplerCost repeated = af.Cost();
    EXPECT_EQ(repeated.shaderRepeats - warm.shaderRepeats, 64u);
    EXPECT_EQ(repeated.shaderLookups, warm.shaderLookups) << "the tracked shader needs no lookup";
    for (int i = 0; i < 64; ++i) {
        af.context->PSSetShader(i % 2 ? af.ps.Get() : af.ps2.Get(), nullptr, 0);
    }
    const CEFlowD3D11SamplerCost alternated = af.Cost();
    EXPECT_EQ(alternated.shaderMemoHits - repeated.shaderMemoHits, 64u);
    EXPECT_EQ(alternated.shaderLookups, warm.shaderLookups) << "a memoized shader needs no lookup";

    // The memoized plan decides as the looked-up one did: AF for the mipmapped texture, the
    // game's own sampler for a single-mip one, under either shader.
    af.Bind(af.linear.Get());
    for (ID3D11PixelShader* shader : {af.ps2.Get(), af.ps.Get(), af.ps2.Get()}) {
        af.context->PSSetShader(shader, nullptr, 0);
        af.DrawWith(af.mipmapped.Get());
        ExpectAF(af.Physical(), 16, "memoized shader, mipmapped texture");
        af.DrawWith(af.singleMip.Get());
        ce::flow::af::ExpectOriginal(af.Physical(), af.linear.Get(), "memoized shader, single mip");
    }
    // A null shader restores the game's sampler; the next switch is a memo hit again.
    af.DrawWith(af.mipmapped.Get());
    af.context->PSSetShader(nullptr, nullptr, 0);
    af.context->Draw(3, 0);
    ce::flow::af::ExpectOriginal(af.Physical(), af.linear.Get(), "no pixel shader");
    const uint64_t lookups = af.Cost().shaderLookups;
    af.context->PSSetShader(af.ps.Get(), nullptr, 0);
    af.context->Draw(3, 0);
    ExpectAF(af.Physical(), 16, "back from a null shader");
    EXPECT_EQ(af.Cost().shaderLookups, lookups);
}

TEST(FlowD3D11SamplerAFCost, RawShaderSwitchesSkipTheMetadataLookup) {
    RunShaderSwitchesSkipTheMetadataLookup(Path::kRaw);
}
TEST(FlowD3D11SamplerAFCost, WrapperShaderSwitchesSkipTheMetadataLookup) {
    RunShaderSwitchesSkipTheMetadataLookup(Path::kWrapper);
}

Microsoft::WRL::ComPtr<ID3DBlob> CompilePixelShader(const char* body) {
    char source[512];
    const int length = std::snprintf(source, sizeof(source),
                                     "Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
                                     "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target { %s }",
                                     body);
    return ce::flow::af::CompileSource(source, static_cast<size_t>(length), "main", "ps_5_0");
}

bool ShaderMemoHolds(const ID3D11PixelShader* shader) {
    const auto holds = reinterpret_cast<bool (*)(const ID3D11PixelShader*)>(
        GetProcAddress(ce::flow::af::FlowHook(), "CEFlow_D3D11ShaderMemoHolds"));
    return holds && holds(shader);
}

// The shader memo is the calling thread's and holds no reference: a working set well past the
// 64 shaders a context used to keep (BioShock Infinite missed half its switches with those)
// switches from the memo, and the game's shaders live exactly as long as the game holds them.
void RunShaderMemoHoldsNoShader(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    constexpr int kShaders = 256;
    std::vector<ComPtr<ID3D11PixelShader>> shaders;
    for (int i = 0; i < kShaders; ++i) {
        char body[96];
        std::snprintf(body, sizeof(body), "return t0.Sample(s0, uv) * %d.0;", i + 2);
        const auto code = CompilePixelShader(body);
        ASSERT_TRUE(code) << i;
        ComPtr<ID3D11PixelShader> shader;
        ASSERT_TRUE(
            SUCCEEDED(af.device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader)));
        shaders.push_back(shader);
    }
    const CEFlowD3D11SamplerCost before = af.Cost();
    for (const auto& shader : shaders) {
        af.context->PSSetShader(shader.Get(), nullptr, 0);
    }
    const CEFlowD3D11SamplerCost first = af.Cost();
    EXPECT_EQ(first.shaderLookups - before.shaderLookups, static_cast<uint64_t>(kShaders));
    for (const auto& shader : shaders) {
        af.context->PSSetShader(shader.Get(), nullptr, 0);
    }
    const CEFlowD3D11SamplerCost second = af.Cost();
    EXPECT_EQ(second.shaderMemoHits - first.shaderMemoHits, static_cast<uint64_t>(kShaders))
        << "a 256-shader working set switches from the memo";
    EXPECT_EQ(second.shaderLookups, first.shaderLookups);
    EXPECT_EQ(second.shaderEvictions, before.shaderEvictions);
    // Beyond the test's own reference only the bound shader's: the runtime's and CE's tracked state.
    ULONG held = 0;
    for (const auto& shader : shaders) {
        held += References(shader.Get()) - 1;
    }
    EXPECT_LE(held, 2u) << "the memo must not keep shaders alive";
}

TEST(FlowD3D11SamplerAFCost, RawShaderMemoHoldsNoShader) {
    RunShaderMemoHoldsNoShader(Path::kRaw);
}
TEST(FlowD3D11SamplerAFCost, WrapperShaderMemoHoldsNoShader) {
    RunShaderMemoHoldsNoShader(Path::kWrapper);
}

// Without a reference, a destroyed shader's address can go to a new one. The shader's metadata
// handle clears it from every memo as the runtime destroys it, so the new shader is looked up
// and decided by its own plan. Alternating AF-safe `sample` and unsafe `sample_l` shaders make
// a stale entry visible: it would give the new shader the old one's verdict.
void RunDestroyedShadersLeaveTheMemo(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    const auto sampleCode = CompilePixelShader("return t0.Sample(s0, uv);");
    const auto levelCode = CompilePixelShader("return t0.SampleLevel(s0, uv, 0);");
    ASSERT_TRUE(sampleCode && levelCode);
    af.Bind(af.linear.Get());
    std::vector<const ID3D11PixelShader*> seen;
    int destroyed = 0;
    int reused = 0;
    for (int i = 0; i < 32; ++i) {
        const bool safe = i % 2 == 0;
        ID3DBlob* code = safe ? sampleCode.Get() : levelCode.Get();
        ComPtr<ID3D11PixelShader> shader;
        ASSERT_TRUE(
            SUCCEEDED(af.device->CreatePixelShader(code->GetBufferPointer(), code->GetBufferSize(), nullptr, &shader)));
        bool gone = false;
        auto* probe = new ce::flow::af::DestructionProbe(&gone);
        ASSERT_TRUE(SUCCEEDED(shader->SetPrivateDataInterface(ce::flow::af::kDestructionProbeGuid, probe)));
        probe->Release();
        const ID3D11PixelShader* address = shader.Get();
        for (const ID3D11PixelShader* earlier : seen) {
            reused += earlier == address ? 1 : 0;
        }
        seen.push_back(address);

        const CEFlowD3D11SamplerCost before = af.Cost();
        af.context->PSSetShader(shader.Get(), nullptr, 0);
        const CEFlowD3D11SamplerCost after = af.Cost();
        EXPECT_EQ(after.shaderLookups - before.shaderLookups, 1u) << "a new shader is looked up, " << i;
        EXPECT_EQ(after.shaderMemoHits, before.shaderMemoHits) << i;
        EXPECT_TRUE(ShaderMemoHolds(address)) << i;
        // A sampler bind reconciles too: on a raw context the runtime replaces CE's draw hooks
        // after the first flush (llm-wiki/dx11-forced-af.md, open questions), so only the
        // binds decide there.
        af.Bind(af.clamp.Get());
        af.Bind(af.linear.Get());
        af.DrawWith(af.mipmapped.Get());
        if (safe) {
            ExpectAF(af.Physical(), 16, ("sample " + std::to_string(i)).c_str());
        } else {
            ce::flow::af::ExpectOriginal(af.Physical(), af.linear.Get(), ("sample_l " + std::to_string(i)).c_str());
        }
        af.context->PSSetShader(af.ps.Get(), nullptr, 0);
        shader.Reset();
        af.context->Flush();
        if (gone) {
            ++destroyed;
            EXPECT_FALSE(ShaderMemoHolds(address)) << "a destroyed shader must leave every memo, " << i;
        }
    }
    std::printf("[  FLOW  ] destroyed=%d/32 reusedAddresses=%d\n", destroyed, reused);
    EXPECT_GT(destroyed, 0) << "the runtime never destroyed a released shader: nothing was checked";
}

TEST(FlowD3D11SamplerAFCost, RawDestroyedShadersLeaveTheMemo) {
    RunDestroyedShadersLeaveTheMemo(Path::kRaw);
}
TEST(FlowD3D11SamplerAFCost, WrapperDestroyedShadersLeaveTheMemo) {
    RunDestroyedShadersLeaveTheMemo(Path::kWrapper);
}

// A pass that binds adjacent slots sends them to the context as one ranged PSSetSamplers call,
// CE's AF replacement included; it used to make one call per slot. Each pass here rebinds four
// slots (only sampler binds run passes, so 64 of them hold exactly two sampled ones).
void RunAdjacentSlotsBindInOneCall(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    ID3D11ShaderResourceView* view = af.mipmapped.Get();
    af.context->PSSetShaderResources(0, 1, &view);
    ID3D11SamplerState* const sets[2][4] = {{af.linear.Get(), af.clamp.Get(), af.linear.Get(), af.clamp.Get()},
                                            {af.clamp.Get(), af.linear.Get(), af.clamp.Get(), af.linear.Get()}};
    af.context->PSSetSamplers(0, 4, sets[1]);
    af.context->Draw(3, 0);
    const CEFlowD3D11SamplerCost before = af.Cost();
    // One pass per bind and draw: the bind's on a raw context, the draw's where binds wait for it.
    for (int i = 0; i < 64; ++i) {
        af.context->PSSetSamplers(0, 4, sets[i % 2]);
        af.context->Draw(3, 0);
    }
    const CEFlowD3D11SamplerCost after = af.Cost();
    EXPECT_EQ(after.reconciledSlots - before.reconciledSlots, 64u * 4u) << "every pass takes its four changed slots";
    EXPECT_EQ(after.rebinds - before.rebinds, 64u * 4u);
    EXPECT_EQ(after.timedBindSlots - before.timedBindSlots, 8u);
    EXPECT_EQ(after.timedBindCalls - before.timedBindCalls, 2u) << "four adjacent slots, one call";
    // The last set is bound: slot 0 as CE's AF replacement (the shader samples t0 with s0), the
    // others as the game's own samplers.
    ExpectAF(af.Physical(0), 16, "slot 0");
    for (UINT slot = 1; slot < 4; ++slot) {
        EXPECT_EQ(af.PhysicalObject(slot).Get(), sets[1][slot]) << slot;
    }
    ID3D11SamplerState* readBack[4] = {};
    af.context->PSGetSamplers(0, 4, readBack);
    for (UINT slot = 0; slot < 4; ++slot) {
        EXPECT_EQ(readBack[slot], sets[1][slot]) << "the game reads back its own sampler, slot " << slot;
        if (readBack[slot]) {
            readBack[slot]->Release();
        }
    }
}

TEST(FlowD3D11SamplerAFCost, RawAdjacentSlotsBindInOneCall) {
    RunAdjacentSlotsBindInOneCall(Path::kRaw);
}
TEST(FlowD3D11SamplerAFCost, WrapperAdjacentSlotsBindInOneCall) {
    RunAdjacentSlotsBindInOneCall(Path::kWrapper);
}

TEST(FlowD3D11SamplerAFCost, RawSamplerCreationsAreSampled) {
    RunSamplerCreationsAreSampled(Path::kRaw);
}
TEST(FlowD3D11SamplerAFCost, WrapperSamplerCreationsAreSampled) {
    RunSamplerCreationsAreSampled(Path::kWrapper);
}

bool MemoVouches(const ID3D11SamplerState* sampler) {
    const auto vouches = reinterpret_cast<CEFlow_D3D11CreationMemoVouches_t>(
        GetProcAddress(ce::flow::af::FlowHook(), "CEFlow_D3D11CreationMemoVouches"));
    return vouches && vouches(sampler);
}

// The creation tag the sampler carries: its application descriptor, false when untagged.
bool CreationTag(ID3D11SamplerState* sampler, D3D11_SAMPLER_DESC* application) {
    const auto read = reinterpret_cast<bool (*)(ID3D11SamplerState*, uint64_t*, D3D11_SAMPLER_DESC*)>(
        GetProcAddress(ce::flow::af::FlowHook(), "CEFlow_GetD3D11SamplerCreationTag"));
    uint64_t hash = 0;
    return read && read(sampler, &hash, application) && hash != 0;
}

bool SameDesc(const D3D11_SAMPLER_DESC& a, const D3D11_SAMPLER_DESC& b) {
    return std::memcmp(&a, &b, sizeof(a)) == 0;
}

// The creating thread's memo supplies the creation policy's result and vouches for the tag of an
// object it confirmed before: no private-data read and no process-wide atomic for 99.9% of
// BioShock Infinite's ~157k creations per second. A new policy starts over once.
void RunRepeatedCreationsUseTheThreadMemo(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    const CEFlowD3D11SamplerCreationStats before = CreationStats();
    for (int i = 0; i < 64; ++i) {
        ASSERT_EQ(af.CreateSampler(D3D11_TEXTURE_ADDRESS_WRAP).Get(), af.linear.Get());
    }
    const CEFlowD3D11SamplerCreationStats repeated = CreationStats();
    EXPECT_EQ(repeated.descHits - before.descHits, 64u);
    EXPECT_EQ(repeated.tagSkips - before.tagSkips, 64u);
    EXPECT_EQ(repeated.tagsKept - before.tagsKept, 64u);
    EXPECT_EQ(repeated.tagsWritten, before.tagsWritten);
    EXPECT_TRUE(MemoVouches(af.linear.Get()));

    af.SetMipBias("1");
    const ComPtr<ID3D11SamplerState> biased = af.CreateSampler(D3D11_TEXTURE_ADDRESS_WRAP);
    ASSERT_TRUE(biased);
    ASSERT_NE(biased.Get(), af.linear.Get()) << "the bias makes it another object";
    const CEFlowD3D11SamplerCreationStats changed = CreationStats();
    EXPECT_EQ(changed.descHits, repeated.descHits) << "a new policy hash finds no entry";
    EXPECT_EQ(changed.tagsWritten - repeated.tagsWritten, 1u);
    EXPECT_EQ(af.CreateSampler(D3D11_TEXTURE_ADDRESS_WRAP).Get(), biased.Get());
    const CEFlowD3D11SamplerCreationStats again = CreationStats();
    EXPECT_EQ(again.descHits - changed.descHits, 1u);
    EXPECT_EQ(again.tagSkips - changed.tagSkips, 1u);
    D3D11_SAMPLER_DESC application = {};
    ASSERT_TRUE(CreationTag(biased.Get(), &application));
    EXPECT_TRUE(SameDesc(application, AFFixture::LinearDesc(D3D11_TEXTURE_ADDRESS_WRAP)));
    af.Bind(biased.Get());
    af.DrawWith(af.mipmapped.Get());
    ExpectAF(af.Physical(), 16, "memoized creation");
    EXPECT_FLOAT_EQ(af.Physical().MipLODBias, 1.0f) << "the creation policy applies once";
}

TEST(FlowD3D11SamplerAFCost, RawRepeatedCreationsUseTheThreadMemo) {
    RunRepeatedCreationsUseTheThreadMemo(Path::kRaw);
}
TEST(FlowD3D11SamplerAFCost, WrapperRepeatedCreationsUseTheThreadMemo) {
    RunRepeatedCreationsUseTheThreadMemo(Path::kWrapper);
}

// The memo holds no reference (a held sampler counts toward the device's 4096), so a sampler the
// game releases dies and its address can go to a new one. The sentinel clears the old object
// from every memo as the runtime destroys it: a new object at that address is tagged again,
// never vouched for (an untagged sampler would get the whole creation policy again at bind time:
// the mip bias twice).
void RunDestroyedSamplersLeaveTheMemo(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    af.SetMipBias("1");
    D3D11_SAMPLER_DESC desc = AFFixture::LinearDesc(D3D11_TEXTURE_ADDRESS_MIRROR);
    desc.MinLOD = 0.5f;  // a descriptor no other object in this test has
    std::vector<const ID3D11SamplerState*> seen;
    int destroyed = 0;
    int reused = 0;
    for (int i = 0; i < 64; ++i) {
        const CEFlowD3D11SamplerCreationStats before = CreationStats();
        ComPtr<ID3D11SamplerState> sampler;
        ASSERT_TRUE(SUCCEEDED(af.device->CreateSamplerState(&desc, &sampler)) && sampler) << i;
        const CEFlowD3D11SamplerCreationStats after = CreationStats();
        D3D11_SAMPLER_DESC application = {};
        ASSERT_TRUE(CreationTag(sampler.Get(), &application)) << "every game sampler carries its tag, " << i;
        EXPECT_TRUE(SameDesc(application, desc)) << i;
        EXPECT_TRUE(MemoVouches(sampler.Get())) << i;
        const ID3D11SamplerState* address = sampler.Get();
        for (const ID3D11SamplerState* earlier : seen) {
            reused += earlier == address ? 1 : 0;
        }
        bool gone = false;
        auto* probe = new ce::flow::af::DestructionProbe(&gone);
        ASSERT_TRUE(SUCCEEDED(sampler->SetPrivateDataInterface(ce::flow::af::kDestructionProbeGuid, probe)));
        probe->Release();
        if (i != 0 && destroyed == i) {
            // The previous object died, so this one is new: tagged, not vouched for.
            EXPECT_EQ(after.tagSkips, before.tagSkips) << i;
            EXPECT_EQ(after.tagsWritten - before.tagsWritten, 1u) << i;
        }
        seen.push_back(address);
        sampler.Reset();
        af.context->Flush();  // the runtime destroys released state objects at a flush
        if (gone) {
            ++destroyed;
            EXPECT_FALSE(MemoVouches(address)) << "a destroyed sampler must leave every memo, " << i;
        }
    }
    std::printf("[  FLOW  ] destroyed=%d/64 reusedAddresses=%d\n", destroyed, reused);
    EXPECT_GT(destroyed, 0) << "the runtime never destroyed a released sampler: nothing was checked";
    ComPtr<ID3D11SamplerState> bound;
    ASSERT_TRUE(SUCCEEDED(af.device->CreateSamplerState(&desc, &bound)));
    af.Bind(bound.Get());
    af.DrawWith(af.mipmapped.Get());
    EXPECT_FLOAT_EQ(af.Physical().MipLODBias, 1.0f) << "the creation policy applies once";
}

TEST(FlowD3D11SamplerAFCost, RawDestroyedSamplersLeaveTheMemo) {
    RunDestroyedSamplersLeaveTheMemo(Path::kRaw);
}
TEST(FlowD3D11SamplerAFCost, WrapperDestroyedSamplersLeaveTheMemo) {
    RunDestroyedSamplersLeaveTheMemo(Path::kWrapper);
}

// Two descriptors the runtime folds into one object (it zeroes BorderColor without border
// addressing) share one tag, the last creator's. Another thread rewriting it clears this
// thread's memo, so this thread's next creation tags the object again, as it did before the memo.
void RunRetagByAnotherThreadClearsTheMemo(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    D3D11_SAMPLER_DESC first = AFFixture::LinearDesc(D3D11_TEXTURE_ADDRESS_WRAP);
    first.MinLOD = 0.25f;
    D3D11_SAMPLER_DESC second = first;
    second.BorderColor[0] = second.BorderColor[1] = second.BorderColor[2] = second.BorderColor[3] = 1.0f;
    ComPtr<ID3D11SamplerState> object;
    ASSERT_TRUE(SUCCEEDED(af.device->CreateSamplerState(&first, &object)));
    D3D11_SAMPLER_DESC application = {};
    ASSERT_TRUE(CreationTag(object.Get(), &application));
    EXPECT_TRUE(SameDesc(application, first));
    ComPtr<ID3D11SamplerState> folded;
    std::thread([&] { af.device->CreateSamplerState(&second, &folded); }).join();
    ASSERT_EQ(folded.Get(), object.Get()) << "the runtime folds both descriptors into one object";
    ASSERT_TRUE(CreationTag(object.Get(), &application));
    EXPECT_TRUE(SameDesc(application, second)) << "the other thread's creation retagged it";

    const CEFlowD3D11SamplerCreationStats before = CreationStats();
    ComPtr<ID3D11SamplerState> again;
    ASSERT_TRUE(SUCCEEDED(af.device->CreateSamplerState(&first, &again)));
    ASSERT_EQ(again.Get(), object.Get());
    const CEFlowD3D11SamplerCreationStats after = CreationStats();
    EXPECT_EQ(after.descHits - before.descHits, 1u) << "the policy result is still this thread's";
    EXPECT_EQ(after.tagSkips, before.tagSkips) << "the memo must not vouch for a retagged object";
    EXPECT_EQ(after.tagsWritten - before.tagsWritten, 1u);
    ASSERT_TRUE(CreationTag(object.Get(), &application));
    EXPECT_TRUE(SameDesc(application, first));
}

TEST(FlowD3D11SamplerAFCost, RawRetagByAnotherThreadClearsTheMemo) {
    RunRetagByAnotherThreadClearsTheMemo(Path::kRaw);
}
TEST(FlowD3D11SamplerAFCost, WrapperRetagByAnotherThreadClearsTheMemo) {
    RunRetagByAnotherThreadClearsTheMemo(Path::kWrapper);
}

}  // namespace
