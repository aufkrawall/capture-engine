#pragma once

// The D3D11 forced-AF flow fixture: one WARP device and context on either path the shared
// engine serves (hook/d3d11/d3d11_af_engine.h), with the hook's test exports. Used by
// test_flow_d3d11_sampler_af.cpp (decisions) and test_flow_d3d11_sampler_af_cost.cpp (work).

#include "tests/flow/flow_test_support.h"

#include "common/ipc/shared_defs.h"

#include <d3d11.h>
#include <d3dcompiler.h>
#include <wrl/client.h>

#include <cfloat>
#include <cstring>

namespace ce::flow::af {

inline constexpr char kShaders[] = R"(
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
float4 ps2(V v) : SV_Target { return t0.Sample(s0, v.uv) * 0.5; }
)";

inline Microsoft::WRL::ComPtr<ID3DBlob> CompileSource(const char* source, size_t length, const char* entry,
                                                      const char* target) {
    const HMODULE compiler = LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    const auto compile =
        compiler ? reinterpret_cast<decltype(&D3DCompile)>(GetProcAddress(compiler, "D3DCompile")) : nullptr;
    Microsoft::WRL::ComPtr<ID3DBlob> code;
    Microsoft::WRL::ComPtr<ID3DBlob> errors;
    if (compile)
        compile(source, length, nullptr, nullptr, nullptr, entry, target, 0, 0, &code, &errors);
    return code;
}

inline Microsoft::WRL::ComPtr<ID3DBlob> Compile(const char* entry, const char* target) {
    return CompileSource(kShaders, sizeof(kShaders) - 1, entry, target);
}

inline Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> CreateTextureView(ID3D11Device* device, UINT mipLevels) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = desc.Height = 64;
    desc.MipLevels = mipLevels;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    Microsoft::WRL::ComPtr<ID3D11Texture2D> texture;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> view;
    if (SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &texture)))
        device->CreateShaderResourceView(texture.Get(), nullptr, &view);
    return view;
}

inline HMODULE FlowHook() {
    return GetModuleHandleW(L"capture_hook_x64.dll");
}

enum class Path { kRaw, kWrapper };

// One D3D11 device and context on the requested path, with the hook's test exports.
class AFFixture {
public:
    AFFixture(ce::flow::FlowGame& game, Path path) : memory_(game.CaptureMemory()) {
        const HMODULE hook = FlowHook();
        install_ = reinterpret_cast<bool (*)(ID3D11Device*, ID3D11Device**, ID3D11DeviceContext**)>(
            GetProcAddress(hook, "CEFlow_InstallRawD3D11Hooks"));
        refresh_ = reinterpret_cast<void (*)()>(GetProcAddress(hook, "CEFlow_RefreshGraphicsConfig"));
        cost_ = reinterpret_cast<CEFlow_GetD3D11SamplerCost_t>(GetProcAddress(
            hook, path == Path::kRaw ? "CEFlow_GetD3D11SamplerCost" : "CEFlow_GetD3D11WrapperSamplerCost"));
        physical_ =
            reinterpret_cast<CEFlow_GetPhysicalPixelSampler_t>(GetProcAddress(hook, "CEFlow_GetPhysicalPixelSampler"));
        if (!memory_ || !install_ || !refresh_ || !cost_ || !physical_) {
            return;
        }
        SetAF("16x");
        const HMODULE runtime = LoadLibraryExW(L"d3d11.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
        const auto create =
            runtime ? reinterpret_cast<decltype(&D3D11CreateDevice)>(GetProcAddress(runtime, "D3D11CreateDevice"))
                    : nullptr;
        // The flow process's own D3D11CreateDevice resolves to CE's wrapper.
        Microsoft::WRL::ComPtr<ID3D11Device> created;
        if (!create || FAILED(create(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION, &created,
                                     nullptr, nullptr))) {
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
        const auto ps2Code = Compile("ps2", "ps_5_0");
        if (!vsCode || !psCode || !ps2Code ||
            FAILED(device->CreateVertexShader(vsCode->GetBufferPointer(), vsCode->GetBufferSize(), nullptr, &vs)) ||
            FAILED(device->CreatePixelShader(psCode->GetBufferPointer(), psCode->GetBufferSize(), nullptr, &ps)) ||
            FAILED(device->CreatePixelShader(ps2Code->GetBufferPointer(), ps2Code->GetBufferSize(), nullptr, &ps2))) {
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
        // The flow build recomputes the engine's incremental slot masks at every read.
        const auto mismatches =
            reinterpret_cast<uint64_t (*)()>(GetProcAddress(FlowHook(), "CEFlow_D3D11AFSlotMaskMismatches"));
        EXPECT_TRUE(mismatches) << "CEFlow_D3D11AFSlotMaskMismatches missing";
        if (mismatches) {
            EXPECT_EQ(mismatches(), 0u) << "tracked/overridden slot masks went stale";
        }
    }
    AFFixture(const AFFixture&) = delete;
    AFFixture& operator=(const AFFixture&) = delete;

    bool Ready() const {
        return ready_;
    }

    void SetAF(const char* level) {
        strcpy_s(memory_->graphicsConfig.anisotropicFiltering, level);
        memory_->configVersion.fetch_add(1, std::memory_order_release);
        refresh_();
    }

    void SetMipBias(const char* bias) {
        strcpy_s(memory_->graphicsConfig.mipBias, bias);
        strcpy_s(memory_->graphicsConfig.mipBiasMode, "offset");
        memory_->configVersion.fetch_add(1, std::memory_order_release);
        refresh_();
    }

    static D3D11_SAMPLER_DESC LinearDesc(D3D11_TEXTURE_ADDRESS_MODE address) {
        D3D11_SAMPLER_DESC desc = {};
        desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
        desc.AddressU = desc.AddressV = desc.AddressW = address;
        desc.MaxAnisotropy = 1;
        desc.ComparisonFunc = D3D11_COMPARISON_NEVER;
        desc.MaxLOD = FLT_MAX;
        return desc;
    }

    Microsoft::WRL::ComPtr<ID3D11SamplerState> CreateSampler(D3D11_TEXTURE_ADDRESS_MODE address) {
        const D3D11_SAMPLER_DESC desc = LinearDesc(address);
        Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
        device->CreateSamplerState(&desc, &sampler);
        return sampler;
    }

    void Bind(ID3D11SamplerState* sampler) {
        context->PSSetSamplers(0, 1, &sampler);
    }
    void DrawWith(ID3D11ShaderResourceView* view) {
        context->PSSetShaderResources(0, 1, &view);
        context->Draw(3, 0);
    }

    // What the real context holds in pixel sampler `slot` (CE's replacement where AF applies).
    D3D11_SAMPLER_DESC Physical(UINT slot = 0) const {
        Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
        D3D11_SAMPLER_DESC desc = {};
        if (physical_(context.Get(), slot, &sampler) && sampler) {
            sampler->GetDesc(&desc);
        }
        return desc;
    }
    Microsoft::WRL::ComPtr<ID3D11SamplerState> PhysicalObject(UINT slot) const {
        Microsoft::WRL::ComPtr<ID3D11SamplerState> sampler;
        physical_(context.Get(), slot, &sampler);
        return sampler;
    }

    CEFlowD3D11SamplerCost Cost() const {
        constexpr uint32_t kValues = static_cast<uint32_t>(sizeof(CEFlowD3D11SamplerCost) / sizeof(uint64_t));
        uint64_t values[kValues] = {};
        CEFlowD3D11SamplerCost out = {};
        if (cost_(values, kValues) == kValues) {
            std::memcpy(&out, values, sizeof(out));
        }
        return out;
    }

    Microsoft::WRL::ComPtr<ID3D11Device> device;
    Microsoft::WRL::ComPtr<ID3D11DeviceContext> context;
    Microsoft::WRL::ComPtr<ID3D11VertexShader> vs;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps;
    Microsoft::WRL::ComPtr<ID3D11PixelShader> ps2;  // a second shader with the same sampler plan
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> mipmapped;
    Microsoft::WRL::ComPtr<ID3D11ShaderResourceView> singleMip;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> linear;
    Microsoft::WRL::ComPtr<ID3D11SamplerState> clamp;

private:
    SharedMemoryLayout* memory_ = nullptr;
    bool (*install_)(ID3D11Device*, ID3D11Device**, ID3D11DeviceContext**) = nullptr;
    void (*refresh_)() = nullptr;
    CEFlow_GetD3D11SamplerCost_t cost_ = nullptr;
    CEFlow_GetPhysicalPixelSampler_t physical_ = nullptr;
    bool ready_ = false;
};

inline void ExpectAF(const D3D11_SAMPLER_DESC& bound, UINT level, const char* where) {
    EXPECT_EQ(bound.Filter, D3D11_FILTER_ANISOTROPIC) << where;
    EXPECT_EQ(bound.MaxAnisotropy, level) << where;
}

// The game's own sampler as the runtime reports it (CE's creation policy may normalize fields).
inline void ExpectOriginal(const D3D11_SAMPLER_DESC& bound, ID3D11SamplerState* own, const char* where) {
    D3D11_SAMPLER_DESC created = {};
    own->GetDesc(&created);
    EXPECT_EQ(bound.Filter, created.Filter) << where;
    EXPECT_EQ(bound.MaxAnisotropy, created.MaxAnisotropy) << where;
    EXPECT_EQ(bound.AddressU, created.AddressU) << where;
}

inline CEFlowD3D11ReplacementStats ReplacementStats() {
    const auto read =
        reinterpret_cast<CEFlow_GetD3D11SamplerCost_t>(GetProcAddress(FlowHook(), "CEFlow_GetD3D11ReplacementStats"));
    uint64_t values[5] = {};
    CEFlowD3D11ReplacementStats out = {};
    if (read && read(values, 5) == 5u) {
        out = {values[0], values[1], values[2], values[3], values[4]};
    }
    return out;
}

}  // namespace ce::flow::af
