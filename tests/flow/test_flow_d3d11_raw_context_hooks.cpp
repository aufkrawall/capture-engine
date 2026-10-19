#include "tests/flow/d3d11_af_fixture.h"

#include <d3d11_1.h>
#include <d3d11_4.h>
#include <psapi.h>

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

// CE's raw D3D11 context hooks against d3d11.dll's own rewrites of a context's function table
// (hook/d3d11/d3d11_context_dispatch.h). On WARP, Flush and ExecuteCommandList switch the draw
// entries to `*_Amortized` variants and the next draw restores the static defaults;
// SetMultithreadProtected and SwapDeviceContextState rewrite the pixel-stage binds, ClearState,
// ExecuteCommandList, FinishCommandList and PSGetSamplers too. CE's hooks lived in that table
// until 2026-10-17 and were gone after the first flush.
namespace {
using ce::flow::af::AFFixture;
using ce::flow::af::ExpectAF;
using ce::flow::af::ExpectOriginal;
using ce::flow::af::Path;
using Microsoft::WRL::ComPtr;

constexpr unsigned kHookedSlots[] = {8, 9, 10, 12, 13, 20, 21, 38, 39, 40, 58, 75, 110, 114, 131};

bool InModule(HMODULE module, const void* address) {
    MODULEINFO info = {};
    if (!GetModuleInformation(GetCurrentProcess(), module, &info, sizeof(info))) {
        return false;
    }
    const auto base = reinterpret_cast<uintptr_t>(info.lpBaseOfDll);
    const auto at = reinterpret_cast<uintptr_t>(address);
    return at >= base && at < base + info.SizeOfImage;
}

// Every slot CE hooks is reached through CE's code (its detour), whatever the runtime did.
void ExpectHooksReachCE(ID3D11DeviceContext* context, const char* where) {
    void* const* const vtable = *reinterpret_cast<void* const* const*>(context);
    for (unsigned slot : kHookedSlots) {
        EXPECT_TRUE(InModule(ce::flow::af::FlowHook(), vtable[slot])) << where << ": slot " << slot << " is "
                                                                     << vtable[slot];
    }
}

CEFlowD3D11DispatchStats DispatchStats() {
    const auto read =
        reinterpret_cast<CEFlow_GetD3D11SamplerCost_t>(GetProcAddress(ce::flow::af::FlowHook(),
                                                                      "CEFlow_GetD3D11DispatchStats"));
    uint64_t values[5] = {};
    CEFlowD3D11DispatchStats out = {};
    if (read && read(values, 5) == 5u) {
        out = {values[0], values[1], values[2], values[3], values[4]};
    }
    return out;
}

constexpr char kLodShader[] = R"(
Texture2D t0 : register(t0);
SamplerState s0 : register(s0);
struct V { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
float4 ps(V v) : SV_Target { return t0.SampleLevel(s0, v.uv, 0); }
)";

// What the draw entry points need besides the fixture's state.
struct DrawInputs {
    explicit DrawInputs(ID3D11Device* device) {
        const uint16_t indices[3] = {0, 1, 2};
        D3D11_BUFFER_DESC desc = {};
        desc.ByteWidth = sizeof(indices);
        desc.Usage = D3D11_USAGE_IMMUTABLE;
        desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
        D3D11_SUBRESOURCE_DATA data = {indices, 0, 0};
        device->CreateBuffer(&desc, &data, &indexBuffer);
        const UINT args[5] = {3, 1, 0, 0, 0};  // both indirect layouts: 3 vertices/indices, one instance
        desc = {};
        desc.ByteWidth = sizeof(args);
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
        data = {args, 0, 0};
        device->CreateBuffer(&desc, &data, &argsBuffer);
    }
    bool Ready() const {
        return indexBuffer && argsBuffer;
    }
    ComPtr<ID3D11Buffer> indexBuffer;
    ComPtr<ID3D11Buffer> argsBuffer;
};

enum class DrawCall { kDraw, kIndexed, kInstanced, kIndexedInstanced, kInstancedIndirect, kIndexedInstancedIndirect };
constexpr DrawCall kDrawCalls[] = {DrawCall::kDraw,
                                   DrawCall::kIndexed,
                                   DrawCall::kInstanced,
                                   DrawCall::kIndexedInstanced,
                                   DrawCall::kInstancedIndirect,
                                   DrawCall::kIndexedInstancedIndirect};
const char* Name(DrawCall call) {
    switch (call) {
        case DrawCall::kDraw:
            return "Draw";
        case DrawCall::kIndexed:
            return "DrawIndexed";
        case DrawCall::kInstanced:
            return "DrawInstanced";
        case DrawCall::kIndexedInstanced:
            return "DrawIndexedInstanced";
        case DrawCall::kInstancedIndirect:
            return "DrawInstancedIndirect";
        case DrawCall::kIndexedInstancedIndirect:
            return "DrawIndexedInstancedIndirect";
    }
    return "?";
}

void Issue(ID3D11DeviceContext* context, const DrawInputs& inputs, DrawCall call) {
    context->IASetIndexBuffer(inputs.indexBuffer.Get(), DXGI_FORMAT_R16_UINT, 0);
    switch (call) {
        case DrawCall::kDraw:
            context->Draw(3, 0);
            break;
        case DrawCall::kIndexed:
            context->DrawIndexed(3, 0, 0);
            break;
        case DrawCall::kInstanced:
            context->DrawInstanced(3, 1, 0, 0);
            break;
        case DrawCall::kIndexedInstanced:
            context->DrawIndexedInstanced(3, 1, 0, 0, 0);
            break;
        case DrawCall::kInstancedIndirect:
            context->DrawInstancedIndirect(inputs.argsBuffer.Get(), 0);
            break;
        case DrawCall::kIndexedInstancedIndirect:
            context->DrawIndexedInstancedIndirect(inputs.argsBuffer.Get(), 0);
            break;
    }
}

// A flush leaves the runtime's draw entries on their amortized variants; a texture or shader change
// without a sampler rebind is then decided only by the draw. Failed before CE owned the dispatch:
// the replacement stayed bound for a single-mip texture and a sample_l shader.
TEST(FlowD3D11RawContextHooks, DrawsDecideAfterFlush) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, Path::kRaw);
    ASSERT_TRUE(af.Ready());
    DrawInputs inputs(af.device.Get());
    ASSERT_TRUE(inputs.Ready());
    const auto lodCode = ce::flow::af::CompileSource(kLodShader, sizeof(kLodShader) - 1, "ps", "ps_5_0");
    ComPtr<ID3D11PixelShader> lod;
    ASSERT_TRUE(lodCode && SUCCEEDED(af.device->CreatePixelShader(lodCode->GetBufferPointer(),
                                                                   lodCode->GetBufferSize(), nullptr, &lod)));
    af.Bind(af.linear.Get());
    af.DrawWith(af.mipmapped.Get());
    ExpectAF(af.Physical(), 16, "bound and drawn");

    for (DrawCall call : kDrawCalls) {
        const std::string name = Name(call);
        ID3D11ShaderResourceView* view = af.singleMip.Get();
        af.context->Flush();
        af.context->PSSetShaderResources(0, 1, &view);
        Issue(af.context.Get(), inputs, call);
        ExpectOriginal(af.Physical(), af.linear.Get(), (name + " after flush, single mip").c_str());

        view = af.mipmapped.Get();
        af.context->Flush();
        af.context->PSSetShaderResources(0, 1, &view);
        Issue(af.context.Get(), inputs, call);
        ExpectAF(af.Physical(), 16, (name + " after flush, mipmapped").c_str());

        af.context->Flush();
        af.context->PSSetShader(lod.Get(), nullptr, 0);
        Issue(af.context.Get(), inputs, call);
        ExpectOriginal(af.Physical(), af.linear.Get(), (name + " after flush, sample_l shader").c_str());

        af.context->Flush();
        af.context->PSSetShader(af.ps.Get(), nullptr, 0);
        Issue(af.context.Get(), inputs, call);
        ExpectAF(af.Physical(), 16, (name + " after flush, sample shader").c_str());
    }
    af.context->Flush();
    ExpectHooksReachCE(af.context.Get(), "after the last flush");
}

// SetMultithreadProtected rewrites nearly the whole table, binds and resets included. Failed before:
// the game's sampler reached the context unreplaced and its read-back returned CE's replacement.
TEST(FlowD3D11RawContextHooks, HooksSurviveMultithreadProtection) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, Path::kRaw);
    ASSERT_TRUE(af.Ready());
    ComPtr<ID3D11Multithread> multithread;
    ASSERT_TRUE(SUCCEEDED(af.context.As(&multithread)));
    af.Bind(af.linear.Get());
    af.DrawWith(af.mipmapped.Get());
    ExpectAF(af.Physical(), 16, "before");

    for (const BOOL protect : {TRUE, FALSE, TRUE}) {
        const std::string where = protect ? "protected" : "unprotected";
        multithread->SetMultithreadProtected(protect);
        ExpectHooksReachCE(af.context.Get(), where.c_str());
        af.Bind(af.clamp.Get());
        af.DrawWith(af.mipmapped.Get());
        ExpectAF(af.Physical(), 16, (where + ": bound and drawn").c_str());
        EXPECT_EQ(af.Physical().AddressU, D3D11_TEXTURE_ADDRESS_CLAMP) << where;
        ComPtr<ID3D11SamplerState> readBack;
        af.context->PSGetSamplers(0, 1, readBack.GetAddressOf());
        EXPECT_EQ(readBack.Get(), af.clamp.Get()) << where << ": the game reads back its own sampler";
        // ClearState must still drop the tracking, or the same rebind is swallowed.
        af.context->ClearState();
        af.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
        af.context->VSSetShader(af.vs.Get(), nullptr, 0);
        af.context->PSSetShader(af.ps.Get(), nullptr, 0);
        af.Bind(af.clamp.Get());
        af.DrawWith(af.mipmapped.Get());
        ExpectAF(af.Physical(), 16, (where + ": rebound after ClearState").c_str());
        af.DrawWith(af.singleMip.Get());
        ExpectOriginal(af.Physical(), af.clamp.Get(), (where + ": single mip").c_str());
    }
    multithread->SetMultithreadProtected(FALSE);
}

// SwapDeviceContextState and ExecuteCommandList rewrite the table as well; deferred contexts get CE
// tables of their own, which the runtime returns when it destroys them.
TEST(FlowD3D11RawContextHooks, TablesFollowContextsAndStateSwaps) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, Path::kRaw);
    ASSERT_TRUE(af.Ready());
    const CEFlowD3D11DispatchStats start = DispatchStats();
    EXPECT_GE(start.adopted, 1u) << "the immediate context dispatches through a CE table";
    EXPECT_EQ(start.unavailable, 0u);

    ComPtr<ID3D11Device1> device1;
    ComPtr<ID3D11DeviceContext1> context1;
    ASSERT_TRUE(SUCCEEDED(af.device.As(&device1)) && SUCCEEDED(af.context.As(&context1)))
        << "QueryInterface through CE's table";
    EXPECT_EQ(static_cast<void*>(context1.Get()), static_cast<void*>(af.context.Get()));
    const D3D_FEATURE_LEVEL level = af.device->GetFeatureLevel();
    ComPtr<ID3DDeviceContextState> other;
    ASSERT_TRUE(SUCCEEDED(device1->CreateDeviceContextState(0, &level, 1, D3D11_SDK_VERSION, __uuidof(ID3D11Device),
                                                            nullptr, &other)));
    ComPtr<ID3DDeviceContextState> previous;
    context1->SwapDeviceContextState(other.Get(), &previous);
    ExpectHooksReachCE(af.context.Get(), "after SwapDeviceContextState");
    ComPtr<ID3DDeviceContextState> swappedOut;
    context1->SwapDeviceContextState(previous.Get(), &swappedOut);

    ComPtr<ID3D11DeviceContext> deferred;
    ASSERT_TRUE(SUCCEEDED(af.device->CreateDeferredContext(0, &deferred)));
    CEFlowD3D11DispatchStats now = DispatchStats();
    EXPECT_EQ(now.adopted, start.adopted + 1) << "the deferred context";
    EXPECT_EQ(now.live, start.live + 1);
    deferred->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    deferred->VSSetShader(af.vs.Get(), nullptr, 0);
    deferred->PSSetShader(af.ps.Get(), nullptr, 0);
    ID3D11ShaderResourceView* view = af.mipmapped.Get();
    deferred->PSSetShaderResources(0, 1, &view);
    ID3D11SamplerState* sampler = af.linear.Get();
    deferred->PSSetSamplers(0, 1, &sampler);
    deferred->Draw(3, 0);
    ExpectAF(af.PhysicalOf(deferred.Get(), 0), 16, "deferred draw");
    ComPtr<ID3D11CommandList> list;
    ASSERT_TRUE(SUCCEEDED(deferred->FinishCommandList(FALSE, &list)));
    af.context->ExecuteCommandList(list.Get(), FALSE);  // also clears the immediate context's state
    ExpectHooksReachCE(af.context.Get(), "after ExecuteCommandList");
    af.context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    af.context->VSSetShader(af.vs.Get(), nullptr, 0);
    af.context->PSSetShader(af.ps.Get(), nullptr, 0);
    af.Bind(af.linear.Get());
    af.DrawWith(af.mipmapped.Get());
    ExpectAF(af.Physical(), 16, "after ExecuteCommandList");
    af.DrawWith(af.singleMip.Get());
    ExpectOriginal(af.Physical(), af.linear.Get(), "single mip after ExecuteCommandList");

    list.Reset();
    deferred.Reset();
    now = DispatchStats();
    EXPECT_EQ(now.retired, start.retired + 1) << "the destroyed deferred context returned its table";
    EXPECT_EQ(now.live, start.live);
}

// A tool that copies CE's table into one of its own and repoints the context still reaches the runtime
// and CE's detours: forwarders never read the vtable pointer, so they cannot loop through the copy.
TEST(FlowD3D11RawContextHooks, ForeignCopyOfTheTableStillDispatches) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    AFFixture af(game, Path::kRaw);
    ASSERT_TRUE(af.Ready());
    af.Bind(af.linear.Get());
    af.DrawWith(af.mipmapped.Get());
    ExpectAF(af.Physical(), 16, "before the copy");

    void** const ceTable = *reinterpret_cast<void***>(af.context.Get());
    std::vector<void*> copy(1 + 256, nullptr);
    std::copy_n(ceTable, 256, copy.data() + 1);
    *reinterpret_cast<void***>(af.context.Get()) = copy.data() + 1;
    af.context->Flush();
    af.DrawWith(af.singleMip.Get());
    ExpectOriginal(af.Physical(), af.linear.Get(), "through the copy, single mip");
    af.DrawWith(af.mipmapped.Get());
    ExpectAF(af.Physical(), 16, "through the copy, mipmapped");
    ComPtr<ID3D11DeviceContext1> context1;
    EXPECT_TRUE(SUCCEEDED(af.context.As(&context1))) << "QueryInterface through the copy";
    context1.Reset();
    *reinterpret_cast<void***>(af.context.Get()) = ceTable;
}

}  // namespace
