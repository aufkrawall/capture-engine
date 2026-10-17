#include "tests/flow/d3d11_af_fixture.h"

#include <cstdio>
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

// The shader memo holds a reference to each shader it keeps, so a context keeps at most 64 of
// the game's shaders alive beyond what the game holds, however many it switches through.
void RunShaderMemoIsBounded(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    constexpr int kShaders = 96;
    std::vector<ComPtr<ID3D11PixelShader>> shaders;
    for (int i = 0; i < kShaders; ++i) {
        char source[256];
        const int length = std::snprintf(source, sizeof(source),
                                         "Texture2D t0 : register(t0); SamplerState s0 : register(s0);\n"
                                         "float4 main(float4 p : SV_Position, float2 uv : TEXCOORD0) : SV_Target "
                                         "{ return t0.Sample(s0, uv) * %d.0; }",
                                         i + 2);
        const auto code = ce::flow::af::CompileSource(source, static_cast<size_t>(length), "main", "ps_5_0");
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
    const CEFlowD3D11SamplerCost after = af.Cost();
    EXPECT_EQ(after.shaderLookups - before.shaderLookups, static_cast<uint64_t>(kShaders));
    EXPECT_GE(after.shaderEvictions - before.shaderEvictions, static_cast<uint64_t>(kShaders - 64));
    // Beyond the test's own reference: the memo's (64 at most), and the bound shader's (the
    // runtime's and CE's tracked state).
    ULONG held = 0;
    for (const auto& shader : shaders) {
        held += References(shader.Get()) - 1;
    }
    EXPECT_LE(held, 64u + 2u) << "evicted shaders must be released";
    EXPECT_GE(held, 32u) << "the memo keeps shaders";
}

TEST(FlowD3D11SamplerAFCost, RawShaderMemoIsBounded) {
    RunShaderMemoIsBounded(Path::kRaw);
}
TEST(FlowD3D11SamplerAFCost, WrapperShaderMemoIsBounded) {
    RunShaderMemoIsBounded(Path::kWrapper);
}

TEST(FlowD3D11SamplerAFCost, RawSamplerCreationsAreSampled) {
    RunSamplerCreationsAreSampled(Path::kRaw);
}
TEST(FlowD3D11SamplerAFCost, WrapperSamplerCreationsAreSampled) {
    RunSamplerCreationsAreSampled(Path::kWrapper);
}

}  // namespace
