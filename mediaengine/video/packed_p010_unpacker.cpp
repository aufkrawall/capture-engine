#include "packed_p010_unpacker.h"

#include <array>
#include <mutex>
#include <string>
#include <vector>

#include "common/platform/raii_helpers.h"
#include "mediaengine/engine/mediaengine.h"
#include "packed_p010_shader.h"
#include "runtime_shader_compiler.h"
#include "video_color_conversion_shader.h"

namespace {

template <typename T>
void SafeReleaseCom(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

constexpr size_t kShaderCount = 3;  // VS_Main, PS_UnpackP010Y, PS_UnpackP010UV
constexpr UINT kPackedSlotRegister = 5;

std::array<std::vector<uint8_t>, kShaderCount>& PreparedBytecode() {
    static std::array<std::vector<uint8_t>, kShaderCount> bytecode;
    return bytecode;
}

void DrawPlane(ID3D11DeviceContext* context, ID3D11PixelShader* shader, ID3D11RenderTargetView* target, UINT width,
               UINT height) {
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->PSSetShader(shader, nullptr, 0);
    context->Draw(3, 0);
}

}  // namespace

PackedP010Unpacker::~PackedP010Unpacker() {
    Release();
}

void PackedP010Unpacker::Release() {
    SafeReleaseCom(chromaShader_);
    SafeReleaseCom(lumaShader_);
    SafeReleaseCom(vertexShader_);
    SafeReleaseCom(device_);
}

bool PackedP010Unpacker::PrepareShaders() {
    static std::once_flag compileOnce;
    std::call_once(compileOnce, [] {
        const ULONGLONG start = GetTickCount64();
        const std::string source =
            std::string(ce::video_color::kRgbColorConversionShaderSource) + ce::video_color::kPackedP010ShaderSource;
        const ce::video_color::RuntimeShaderEntry entries[kShaderCount] = {
            {"VS_Main", "vs_4_0"}, {"PS_UnpackP010Y", "ps_4_0"}, {"PS_UnpackP010UV", "ps_4_0"}};
        const bool compiled = ce::video_color::CompileRuntimeShaders(
            source.c_str(), source.size(), entries, kShaderCount, "PackedP010", [](ID3DBlob* const* blobs) {
                for (size_t i = 0; i < kShaderCount; ++i) {
                    const auto* bytes = static_cast<const uint8_t*>(blobs[i]->GetBufferPointer());
                    PreparedBytecode()[i].assign(bytes, bytes + blobs[i]->GetBufferSize());
                }
                return true;
            });
        DLL_Log("[PackedP010] Unpack shader preparation %s: elapsed=%llums (before video frames)",
                compiled ? "ready" : "failed", static_cast<unsigned long long>(GetTickCount64() - start));
    });
    return !PreparedBytecode()[0].empty();
}

bool PackedP010Unpacker::Init(ID3D11Device* device) {
    if (!device)
        return false;
    if (device_ == device && vertexShader_ && lumaShader_ && chromaShader_)
        return true;
    Release();
    const auto& bytecode = PreparedBytecode();
    // Missing preparation is a startup failure, never a request to compile on a video frame.
    for (const auto& blob : bytecode) {
        if (blob.empty())
            return false;
    }
    ce::ComGuard<ID3D11VertexShader> vs;
    ce::ComGuard<ID3D11PixelShader> luma;
    ce::ComGuard<ID3D11PixelShader> chroma;
    if (FAILED(device->CreateVertexShader(bytecode[0].data(), bytecode[0].size(), nullptr, vs.addressof())) ||
        FAILED(device->CreatePixelShader(bytecode[1].data(), bytecode[1].size(), nullptr, luma.addressof())) ||
        FAILED(device->CreatePixelShader(bytecode[2].data(), bytecode[2].size(), nullptr, chroma.addressof()))) {
        DLL_Log("[PackedP010] Prepared unpack shader creation failed");
        return false;
    }
    device->AddRef();
    device_ = device;
    vertexShader_ = vs.release();
    lumaShader_ = luma.release();
    chromaShader_ = chroma.release();
    DLL_Log("[PackedP010] Unpack shaders created");
    return true;
}

bool PackedP010Unpacker::Unpack(ID3D11DeviceContext* context, ID3D11ShaderResourceView* packed,
                                ID3D11RenderTargetView* luma, ID3D11RenderTargetView* chroma, UINT width,
                                UINT height) {
    if (!context || !packed || !luma || !chroma || !vertexShader_ || width < 2 || height < 2)
        return false;
    // Exact codes: no blending, no scissor.
    context->RSSetState(nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr);
    context->VSSetShader(vertexShader_, nullptr, 0);
    context->PSSetShaderResources(kPackedSlotRegister, 1, &packed);
    DrawPlane(context, lumaShader_, luma, width, height);
    // 4:2:0 chroma is half-size; dimensions are even by construction.
    DrawPlane(context, chromaShader_, chroma, width / 2, height / 2);
    ID3D11RenderTargetView* nullRtv = nullptr;
    context->OMSetRenderTargets(1, &nullRtv, nullptr);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    context->PSSetShaderResources(kPackedSlotRegister, 1, &nullSrv);
    return true;
}
