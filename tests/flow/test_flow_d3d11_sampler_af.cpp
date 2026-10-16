#include "tests/flow/flow_test_support.h"

#include "common/ipc/shared_defs.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cfloat>
#include <cstring>

// Forced AF on a raw D3D11 immediate context (no CE wrapper), as a D3D11-to-D3D12
// translator's context reaches it: a real WARP device through CE's vtable hooks.
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

D3D11_SAMPLER_DESC BoundPixelSampler(ID3D11DeviceContext* context) {
    ComPtr<ID3D11SamplerState> sampler;
    context->PSGetSamplers(0, 1, &sampler);
    D3D11_SAMPLER_DESC desc = {};
    if (sampler)
        sampler->GetDesc(&desc);
    return desc;
}

TEST(FlowD3D11SamplerAF, MemoizedChoicesKeepPerTextureAFWithoutReclassifying) {
    ce::flow::FlowGame game(ce::flow::CurrentTestName());
    auto* memory = game.CaptureMemory();
    ASSERT_NE(memory, nullptr);
    strcpy_s(memory->graphicsConfig.anisotropicFiltering, "16x");
    memory->configVersion.fetch_add(1, std::memory_order_release);

    const HMODULE hook = GetModuleHandleW(L"capture_hook_x64.dll");
    const auto install = reinterpret_cast<bool (*)(ID3D11Device*, ID3D11Device**, ID3D11DeviceContext**)>(
        GetProcAddress(hook, "CEFlow_InstallRawD3D11Hooks"));
    const auto refresh = reinterpret_cast<void (*)()>(GetProcAddress(hook, "CEFlow_RefreshGraphicsConfig"));
    const auto getCost =
        reinterpret_cast<CEFlow_GetD3D11SamplerCost_t>(GetProcAddress(hook, "CEFlow_GetD3D11SamplerCost"));
    ASSERT_NE(install, nullptr);
    ASSERT_NE(refresh, nullptr);
    ASSERT_NE(getCost, nullptr);
    auto cost = [getCost](CEFlowD3D11SamplerCost* out) {
        uint64_t values[8] = {};
        ASSERT_EQ(getCost(values, 8), 8u);
        *out = {values[0], values[1], values[2], values[3], values[4], values[5], values[6], values[7]};
    };

    const HMODULE runtime = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    ASSERT_NE(runtime, nullptr);
    const auto create = reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(runtime, "D3D11CreateDevice"));
    ASSERT_NE(create, nullptr);
    ComPtr<ID3D11Device> createdDevice;
    ASSERT_EQ(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &createdDevice, nullptr,
                     nullptr),
              S_OK);
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ASSERT_TRUE(install(createdDevice.Get(), &device, &context));

    const auto vsCode = Compile("vs", "vs_5_0");
    const auto psCode = Compile("ps", "ps_5_0");
    ASSERT_NE(vsCode, nullptr);
    ASSERT_NE(psCode, nullptr);
    ComPtr<ID3D11VertexShader> vs;
    ComPtr<ID3D11PixelShader> ps;
    ASSERT_EQ(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs), S_OK);
    ASSERT_EQ(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps), S_OK);
    const auto mipmapped = CreateTextureView(device.Get(), 7);
    const auto singleMip = CreateTextureView(device.Get(), 1);
    ASSERT_NE(mipmapped, nullptr);
    ASSERT_NE(singleMip, nullptr);
    D3D11_SAMPLER_DESC linearDesc = {};
    linearDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    linearDesc.AddressU = linearDesc.AddressV = linearDesc.AddressW = D3D11_TEXTURE_ADDRESS_WRAP;
    linearDesc.MaxAnisotropy = 1;
    linearDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    linearDesc.MaxLOD = FLT_MAX;
    ComPtr<ID3D11SamplerState> linear;
    ASSERT_EQ(device->CreateSamplerState(&linearDesc, &linear), S_OK);
    D3D11_SAMPLER_DESC created = {};
    linear->GetDesc(&created);

    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->VSSetShader(vs.Get(), nullptr, 0);
    context->PSSetShader(ps.Get(), nullptr, 0);
    ID3D11SamplerState* sampler = linear.Get();
    context->PSSetSamplers(0, 1, &sampler);

    // Alternate a mipmapped texture (forced AF) with a single-mip one (protected).
    auto drawWith = [&](ID3D11ShaderResourceView* view) {
        context->PSSetShaderResources(0, 1, &view);
        context->Draw(3, 0);
    };
    drawWith(mipmapped.Get());
    D3D11_SAMPLER_DESC bound = BoundPixelSampler(context.Get());
    EXPECT_EQ(bound.Filter, D3D11_FILTER_ANISOTROPIC);
    EXPECT_EQ(bound.MaxAnisotropy, 16u);
    drawWith(singleMip.Get());
    bound = BoundPixelSampler(context.Get());
    EXPECT_EQ(bound.Filter, created.Filter);
    EXPECT_EQ(bound.MaxAnisotropy, created.MaxAnisotropy);

    CEFlowD3D11SamplerCost warm = {};
    cost(&warm);
    EXPECT_GE(warm.slowSlots, 2u) << "both verdicts classify once";
    constexpr unsigned kDraws = 64;
    for (unsigned draw = 0; draw < kDraws; ++draw) {
        const bool af = draw % 2 == 0;
        drawWith(af ? mipmapped.Get() : singleMip.Get());
        bound = BoundPixelSampler(context.Get());
        EXPECT_EQ(bound.Filter, af ? D3D11_FILTER_ANISOTROPIC : created.Filter) << "draw " << draw;
        EXPECT_EQ(bound.MaxAnisotropy, af ? 16u : created.MaxAnisotropy) << "draw " << draw;
    }
    CEFlowD3D11SamplerCost steady = {};
    cost(&steady);
    EXPECT_EQ(steady.slowSlots, warm.slowSlots) << "memoized choices must not reclassify";
    EXPECT_EQ(steady.fastSlots - warm.fastSlots, kDraws);
    EXPECT_EQ(steady.rebinds - warm.rebinds, kDraws);
    EXPECT_EQ(steady.passes - warm.passes, kDraws);
    EXPECT_EQ(steady.viewClasses - warm.viewClasses, kDraws) << "one cached class read per changed texture";

    // Games rebind samplers per draw (HOTS: hundreds per frame). Each logical sampler keeps its
    // own choice, so alternating two samplers in one slot reclassifies neither.
    D3D11_SAMPLER_DESC clampDesc = linearDesc;
    clampDesc.AddressU = clampDesc.AddressV = clampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ComPtr<ID3D11SamplerState> clamp;
    ASSERT_EQ(device->CreateSamplerState(&clampDesc, &clamp), S_OK);
    D3D11_SAMPLER_DESC clampCreated = {};
    clamp->GetDesc(&clampCreated);
    ID3D11SamplerState* const samplers[2] = {linear.Get(), clamp.Get()};
    auto drawPair = [&](int which, ID3D11ShaderResourceView* view) {
        context->PSSetSamplers(0, 1, &samplers[which]);
        drawWith(view);
    };
    drawPair(1, mipmapped.Get());
    drawPair(1, singleMip.Get());
    CEFlowD3D11SamplerCost beforeAlternation = {};
    cost(&beforeAlternation);
    for (unsigned draw = 0; draw < kDraws; ++draw) {
        const int which = static_cast<int>(draw % 2);
        const bool af = (draw / 2) % 2 == 0;
        drawPair(which, af ? mipmapped.Get() : singleMip.Get());
        bound = BoundPixelSampler(context.Get());
        const D3D11_SAMPLER_DESC& own = which ? clampCreated : created;
        EXPECT_EQ(bound.Filter, af ? D3D11_FILTER_ANISOTROPIC : own.Filter) << "draw " << draw;
        EXPECT_EQ(bound.MaxAnisotropy, af ? 16u : own.MaxAnisotropy) << "draw " << draw;
        EXPECT_EQ(bound.AddressU, own.AddressU) << "draw " << draw;
    }
    CEFlowD3D11SamplerCost alternated = {};
    cost(&alternated);
    EXPECT_EQ(alternated.slowSlots, beforeAlternation.slowSlots) << "alternating samplers must keep their choices";
    EXPECT_EQ(alternated.evictions, 0u);
    context->PSSetSamplers(0, 1, &sampler);
    drawWith(mipmapped.Get());
    cost(&steady);

    // Other stages are not tracked: their binds never reach CE's state.
    for (unsigned bind = 0; bind < 8; ++bind) {
        ID3D11ShaderResourceView* view = bind % 2 ? mipmapped.Get() : singleMip.Get();
        context->VSSetShaderResources(0, 1, &view);
        context->CSSetSamplers(0, 1, &sampler);
    }
    CEFlowD3D11SamplerCost afterOtherStages = {};
    cost(&afterOtherStages);
    EXPECT_EQ(afterOtherStages.srvBinds, steady.srvBinds);
    EXPECT_EQ(afterOtherStages.samplerBinds, steady.samplerBinds);

    // A new AF level clears the replacement cache; the memo must follow it.
    strcpy_s(memory->graphicsConfig.anisotropicFiltering, "4x");
    memory->configVersion.fetch_add(1, std::memory_order_release);
    refresh();
    drawWith(singleMip.Get());
    drawWith(mipmapped.Get());
    bound = BoundPixelSampler(context.Get());
    EXPECT_EQ(bound.Filter, D3D11_FILTER_ANISOTROPIC);
    EXPECT_EQ(bound.MaxAnisotropy, 4u);
    CEFlowD3D11SamplerCost reconfigured = {};
    cost(&reconfigured);
    EXPECT_GT(reconfigured.slowSlots, afterOtherStages.slowSlots);

    // A sampler bind that changes nothing physical is absorbed without a bind.
    const uint64_t rebindsBefore = reconfigured.rebinds;
    context->PSSetSamplers(0, 1, &sampler);
    CEFlowD3D11SamplerCost absorbed = {};
    cost(&absorbed);
    EXPECT_EQ(absorbed.rebinds, rebindsBefore);
    bound = BoundPixelSampler(context.Get());
    EXPECT_EQ(bound.MaxAnisotropy, 4u);

    ID3D11ShaderResourceView* noView = nullptr;
    context->PSSetShaderResources(0, 1, &noView);
    context->ClearState();
    context->Flush();
}

}  // namespace
