#include "tests/flow/d3d11_af_fixture.h"

#include <d3d11_1.h>

#include <string>

// Sampler binds wait for the draw that uses them (d3d11_af_engine.h, Deferring) on CE's context
// wrapper and on raw contexts CE dispatches through its own table: BioShock Infinite spent 25-30%
// of its forced-AF cost on sampler-bind passes it redid before the draw (20261017_215604). Raw
// contexts on slot hooks stay eager. Every scenario runs on all three and checks what the game and
// the real context see at each point a pending bind could leak or vanish.
namespace {
using ce::flow::af::AFFixture;
using ce::flow::af::ExpectAF;
using ce::flow::af::ExpectOriginal;
using ce::flow::af::Path;
using Microsoft::WRL::ComPtr;

// What the game reads back from pixel sampler slot 0.
ComPtr<ID3D11SamplerState> ReadBack(ID3D11DeviceContext* context) {
    ComPtr<ID3D11SamplerState> sampler;
    context->PSGetSamplers(0, 1, sampler.GetAddressOf());
    return sampler;
}

// Three bind/draw patterns of eight draws each: deferring contexts run exactly one pass per draw,
// eager ones one per bind (and the draw again after a texture change), with the same decisions.
void RunDeferredBindsSavePasses(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    const bool defers = path != Path::kRawSlotHooks;
    ID3D11SamplerState* const samplers[2] = {af.linear.Get(), af.clamp.Get()};
    ID3D11ShaderResourceView* const views[2] = {af.mipmapped.Get(), af.singleMip.Get()};
    af.Bind(samplers[1]);
    af.DrawWith(views[0]);

    // Bind, draw.
    CEFlowD3D11SamplerCost before = af.Cost();
    for (int i = 0; i < 8; ++i) {
        af.Bind(samplers[i % 2]);
        af.context->Draw(3, 0);
        ExpectAF(af.Physical(), 16, ("bind, draw " + std::to_string(i)).c_str());
    }
    CEFlowD3D11SamplerCost after = af.Cost();
    EXPECT_EQ(after.passes - before.passes, 8u);
    EXPECT_EQ(after.deferredBinds - before.deferredBinds, defers ? 8u : 0u);
    EXPECT_EQ(after.eagerPasses - before.eagerPasses, defers ? 0u : 8u);
    EXPECT_EQ(after.eagerGroups - before.eagerGroups, defers ? 0u : 8u);
    EXPECT_EQ(after.eagerOverlaps, before.eagerOverlaps);
    EXPECT_EQ(after.supersededSlots, before.supersededSlots);

    // Bind, change the texture, draw: an eager context reconciles the slot again at the draw.
    before = after;
    for (int i = 0; i < 8; ++i) {
        af.Bind(samplers[i % 2]);
        af.DrawWith(views[(i + 1) % 2]);
        if ((i + 1) % 2 == 0) {
            ExpectAF(af.Physical(), 16, ("bind, mipmapped " + std::to_string(i)).c_str());
        } else {
            ExpectOriginal(af.Physical(), samplers[i % 2], ("bind, single mip " + std::to_string(i)).c_str());
        }
    }
    after = af.Cost();
    EXPECT_EQ(after.passes - before.passes, defers ? 8u : 16u);
    EXPECT_EQ(after.eagerOverlaps - before.eagerOverlaps, defers ? 0u : 8u);
    EXPECT_EQ(after.supersededSlots - before.supersededSlots, defers ? 0u : 8u);

    // Bind twice, draw: an eager context's second bind redoes the first.
    af.DrawWith(views[0]);
    before = af.Cost();
    for (int i = 0; i < 8; ++i) {
        af.Bind(samplers[0]);
        af.Bind(samplers[1]);
        af.context->Draw(3, 0);
    }
    after = af.Cost();
    EXPECT_EQ(after.passes - before.passes, defers ? 8u : 16u);
    EXPECT_EQ(after.deferredBinds - before.deferredBinds, defers ? 16u : 0u);
    EXPECT_EQ(after.supersededSlots - before.supersededSlots, defers ? 0u : 8u);
    ExpectAF(af.Physical(), 16, "decisions unchanged");
    EXPECT_EQ(af.Physical().AddressU, D3D11_TEXTURE_ADDRESS_CLAMP);
}

TEST(FlowD3D11SamplerAFDeferral, RawBindsWaitForTheDraw) {
    RunDeferredBindsSavePasses(Path::kRaw);
}
TEST(FlowD3D11SamplerAFDeferral, RawSlotHookBindsStayEager) {
    RunDeferredBindsSavePasses(Path::kRawSlotHooks);
}
TEST(FlowD3D11SamplerAFDeferral, WrapperBindsWaitForTheDraw) {
    RunDeferredBindsSavePasses(Path::kWrapper);
}

// Between a deferred bind and its draw the real context still holds the previous sampler; the game
// reads back what it bound, an unbind included.
void RunPendingBindsReadBackAsBound(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    const bool defers = path != Path::kRawSlotHooks;
    af.Bind(af.linear.Get());
    af.DrawWith(af.mipmapped.Get());
    ExpectAF(af.Physical(), 16, "bound and drawn");

    af.Bind(af.clamp.Get());
    EXPECT_EQ(af.Physical().AddressU, defers ? D3D11_TEXTURE_ADDRESS_WRAP : D3D11_TEXTURE_ADDRESS_CLAMP)
        << "only an eager context binds before the draw";
    EXPECT_EQ(ReadBack(af.context.Get()).Get(), af.clamp.Get()) << "pending bind";
    af.context->Draw(3, 0);
    ExpectAF(af.Physical(), 16, "pending bind drawn");
    EXPECT_EQ(af.Physical().AddressU, D3D11_TEXTURE_ADDRESS_CLAMP);
    EXPECT_EQ(ReadBack(af.context.Get()).Get(), af.clamp.Get()) << "drawn";

    af.Bind(nullptr);
    EXPECT_EQ(ReadBack(af.context.Get()).Get(), nullptr) << "pending unbind";
    af.context->Draw(3, 0);
    EXPECT_EQ(af.PhysicalObject(0).Get(), nullptr) << "the draw unbinds";
    EXPECT_EQ(ReadBack(af.context.Get()).Get(), nullptr);
}

TEST(FlowD3D11SamplerAFDeferral, RawPendingBindsReadBackAsBound) {
    RunPendingBindsReadBackAsBound(Path::kRaw);
}
TEST(FlowD3D11SamplerAFDeferral, RawSlotHookPendingBindsReadBackAsBound) {
    RunPendingBindsReadBackAsBound(Path::kRawSlotHooks);
}
TEST(FlowD3D11SamplerAFDeferral, WrapperPendingBindsReadBackAsBound) {
    RunPendingBindsReadBackAsBound(Path::kWrapper);
}

// After a boundary a sampler bind and a draw decide again (the bind itself where binds are eager).
void BindAndDraw(AFFixture& af, ID3D11SamplerState* sampler) {
    af.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    af.context->VSSetShader(af.vs.Get(), nullptr, 0);
    af.context->PSSetShader(af.ps.Get(), nullptr, 0);
    ID3D11ShaderResourceView* view = af.mipmapped.Get();
    af.context->PSSetShaderResources(0, 1, &view);
    af.Bind(sampler);
    af.context->Draw(3, 0);
}

// ClearState empties the real context and drops a pending bind with the tracking.
void RunClearStateDropsPendingBinds(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    BindAndDraw(af, af.linear.Get());
    af.Bind(af.clamp.Get());
    af.context->ClearState();
    EXPECT_EQ(af.PhysicalObject(0).Get(), nullptr);
    EXPECT_EQ(ReadBack(af.context.Get()).Get(), nullptr);
    BindAndDraw(af, af.clamp.Get());
    ExpectAF(af.Physical(), 16, "after ClearState");
    EXPECT_EQ(af.Physical().AddressU, D3D11_TEXTURE_ADDRESS_CLAMP);
}

TEST(FlowD3D11SamplerAFDeferral, RawClearStateDropsPendingBinds) {
    RunClearStateDropsPendingBinds(Path::kRaw);
}
TEST(FlowD3D11SamplerAFDeferral, RawSlotHookClearStateDropsPendingBinds) {
    RunClearStateDropsPendingBinds(Path::kRawSlotHooks);
}
TEST(FlowD3D11SamplerAFDeferral, WrapperClearStateDropsPendingBinds) {
    RunClearStateDropsPendingBinds(Path::kWrapper);
}

// SwapDeviceContextState saves the context's state into the previous state object: a pending bind
// must reach the real context first, or swapping back restores the sampler before it.
void RunSwapDeviceContextStateKeepsPendingBinds(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11DeviceContext1> context1;
    ASSERT_TRUE(SUCCEEDED(af.device.As(&device1)) && SUCCEEDED(af.context.As(&context1)));
    const D3D_FEATURE_LEVEL level = af.device->GetFeatureLevel();
    ComPtr<ID3DDeviceContextState> other;
    const HRESULT created =
        device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device), nullptr, &other);
    ASSERT_TRUE(SUCCEEDED(created) && other) << "hr=0x" << std::hex << created;
    BindAndDraw(af, af.linear.Get());
    af.Bind(af.clamp.Get());  // pending where binds wait for the draw
    ComPtr<ID3DDeviceContextState> previous;
    context1->SwapDeviceContextState(other.Get(), &previous);
    ComPtr<ID3DDeviceContextState> swappedOut;
    context1->SwapDeviceContextState(previous.Get(), &swappedOut);
    const D3D11_SAMPLER_DESC restored = af.Physical();
    EXPECT_EQ(restored.AddressU, D3D11_TEXTURE_ADDRESS_CLAMP) << "the game's last bind is in the saved state";
    ExpectAF(restored, 16, "saved state");
}

TEST(FlowD3D11SamplerAFDeferral, RawSwapDeviceContextStateKeepsPendingBinds) {
    RunSwapDeviceContextStateKeepsPendingBinds(Path::kRaw);
}
TEST(FlowD3D11SamplerAFDeferral, RawSlotHookSwapDeviceContextStateKeepsPendingBinds) {
    RunSwapDeviceContextStateKeepsPendingBinds(Path::kRawSlotHooks);
}
TEST(FlowD3D11SamplerAFDeferral, WrapperSwapDeviceContextStateKeepsPendingBinds) {
    RunSwapDeviceContextStateKeepsPendingBinds(Path::kWrapper);
}

// A command list carries no state into the immediate context. FinishCommandList(TRUE) and
// ExecuteCommandList(TRUE) keep a pending bind for the next draw; FALSE resets both contexts.
void RunCommandListsKeepOrDropPendingBinds(Path path) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, path);
    ASSERT_TRUE(af.Ready());
    ComPtr<ID3D11DeviceContext> deferred;
    ASSERT_TRUE(SUCCEEDED(af.device->CreateDeferredContext(0, &deferred)) && deferred);
    deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    deferred->VSSetShader(af.vs.Get(), nullptr, 0);
    deferred->PSSetShader(af.ps.Get(), nullptr, 0);
    ID3D11ShaderResourceView* view = af.mipmapped.Get();
    deferred->PSSetShaderResources(0, 1, &view);
    ID3D11SamplerState* sampler = af.linear.Get();
    deferred->PSSetSamplers(0, 1, &sampler);
    deferred->Draw(3, 0);
    sampler = af.clamp.Get();
    deferred->PSSetSamplers(0, 1, &sampler);  // pending where binds wait for the draw
    ComPtr<ID3D11CommandList> first;
    ASSERT_TRUE(SUCCEEDED(deferred->FinishCommandList(TRUE, &first)));
    EXPECT_EQ(ReadBack(deferred.Get()).Get(), af.clamp.Get()) << "kept across FinishCommandList(TRUE)";
    deferred->Draw(3, 0);
    const D3D11_SAMPLER_DESC kept = af.PhysicalOf(deferred.Get(), 0);
    ExpectAF(kept, 16, "drawn after FinishCommandList(TRUE)");
    EXPECT_EQ(kept.AddressU, D3D11_TEXTURE_ADDRESS_CLAMP);
    ComPtr<ID3D11CommandList> second;
    ASSERT_TRUE(SUCCEEDED(deferred->FinishCommandList(FALSE, &second)));
    EXPECT_EQ(ReadBack(deferred.Get()).Get(), nullptr) << "FinishCommandList(FALSE) resets the deferred context";

    // The immediate context: a pending bind survives ExecuteCommandList(TRUE) ...
    BindAndDraw(af, af.linear.Get());
    af.Bind(af.clamp.Get());
    af.context->ExecuteCommandList(first.Get(), TRUE);
    EXPECT_EQ(ReadBack(af.context.Get()).Get(), af.clamp.Get()) << "kept across ExecuteCommandList(TRUE)";
    af.context->Draw(3, 0);
    ExpectAF(af.Physical(), 16, "after ExecuteCommandList(TRUE)");
    EXPECT_EQ(af.Physical().AddressU, D3D11_TEXTURE_ADDRESS_CLAMP);
    // ... and ExecuteCommandList(FALSE) resets the context, the pending bind with it.
    af.Bind(af.linear.Get());
    af.context->ExecuteCommandList(second.Get(), FALSE);
    EXPECT_EQ(ReadBack(af.context.Get()).Get(), nullptr) << "ExecuteCommandList(FALSE) resets the context";
    BindAndDraw(af, af.clamp.Get());
    ExpectAF(af.Physical(), 16, "after ExecuteCommandList(FALSE)");
    EXPECT_EQ(af.Physical().AddressU, D3D11_TEXTURE_ADDRESS_CLAMP);
}

TEST(FlowD3D11SamplerAFDeferral, RawCommandListsKeepOrDropPendingBinds) {
    RunCommandListsKeepOrDropPendingBinds(Path::kRaw);
}
TEST(FlowD3D11SamplerAFDeferral, RawSlotHookCommandListsKeepOrDropPendingBinds) {
    RunCommandListsKeepOrDropPendingBinds(Path::kRawSlotHooks);
}
TEST(FlowD3D11SamplerAFDeferral, WrapperCommandListsKeepOrDropPendingBinds) {
    RunCommandListsKeepOrDropPendingBinds(Path::kWrapper);
}

}  // namespace
