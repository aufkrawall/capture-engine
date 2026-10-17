#include "tests/flow/d3d11_af_fixture.h"

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

TEST(FlowD3D11SamplerAFCost, RawSamplerCreationsAreSampled) {
    RunSamplerCreationsAreSampled(Path::kRaw);
}
TEST(FlowD3D11SamplerAFCost, WrapperSamplerCreationsAreSampled) {
    RunSamplerCreationsAreSampled(Path::kWrapper);
}

}  // namespace
