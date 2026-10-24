#include "yuv_overlay_patch.h"

#include <d3d11_3.h>

#include <algorithm>
#include <cstring>
#include <mutex>
#include <string>

#include "common/platform/raii_helpers.h"
#include "mediaengine/engine/mediaengine.h"
#include "rgb_yuv_draw.h"
#include "runtime_shader_compiler.h"
#include "video_color_conversion_shader.h"
#include "yuv_overlay_patch_shader.h"

namespace {

struct CopyConstants {
    uint32_t colorTransform;
    uint32_t yuvFlags;
    float outputInvWidth;
    float outputInvHeight;
    float lumaSharpenStrength;
    float sdrWhiteNits;  // PqP2020ToYcbcr: >= 1 selects full range
    float padding[2];
};
static_assert(sizeof(CopyConstants) == 32);

struct PatchConstants {
    int32_t cameraRect[4];
    int32_t cursorRect[4];
    int32_t frameSize[2];
    uint32_t patchHdr;
    float rgbLevels;
    float storedToCode;
    float padding[3];
};
static_assert(sizeof(PatchConstants) == 64);

template <typename T>
void SafeReleaseCom(T*& value) {
    if (value) {
        value->Release();
        value = nullptr;
    }
}

bool IsEmpty(const RECT& rect) {
    return rect.right <= rect.left || rect.bottom <= rect.top;
}

void SetRect(int32_t (&out)[4], const YuvOverlayPatcher::Layer& layer) {
    const bool present = layer.view && !IsEmpty(layer.rect);
    out[0] = present ? layer.rect.left : 0;
    out[1] = present ? layer.rect.top : 0;
    out[2] = present ? layer.rect.right : 0;
    out[3] = present ? layer.rect.bottom : 0;
}

template <typename T>
bool WriteBuffer(ID3D11DeviceContext* context, ID3D11Buffer* buffer, const T& value) {
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(buffer, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)) || !mapped.pData)
        return false;
    std::memcpy(mapped.pData, &value, sizeof(value));
    context->Unmap(buffer, 0);
    return true;
}

void DrawRect(ID3D11DeviceContext* context, const RECT& rect) {
    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = static_cast<float>(rect.left);
    viewport.TopLeftY = static_cast<float>(rect.top);
    viewport.Width = static_cast<float>(rect.right - rect.left);
    viewport.Height = static_cast<float>(rect.bottom - rect.top);
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);
    context->Draw(3, 0);
}

}  // namespace

YuvOverlayPatcher::~YuvOverlayPatcher() {
    Release();
}

void YuvOverlayPatcher::Release() {
    SafeReleaseCom(patchConstants_);
    SafeReleaseCom(copyConstants_);
    SafeReleaseCom(chromaShader_);
    SafeReleaseCom(lumaShader_);
    SafeReleaseCom(vertexShader_);
    device_ = nullptr;
    context_ = nullptr;
}

bool YuvOverlayPatcher::PrepareShaders() {
    if (!bytecode_[0].empty())
        return true;
    static std::array<std::vector<uint8_t>, 3> cached;
    static std::once_flag compileOnce;
    std::call_once(compileOnce, [] {
        const ULONGLONG start = GetTickCount64();
        const std::string source = std::string(ce::video_color::kRgbColorConversionShaderSource) +
                                   ce::video_color::kYuvOverlayPatchShaderSource;
        const ce::video_color::RuntimeShaderEntry entries[] = {
            {"VS_Main", "vs_5_0"}, {"PS_PatchLuma", "ps_5_0"}, {"PS_PatchChroma", "ps_5_0"}};
        const bool compiled = ce::video_color::CompileRuntimeShaders(
            source.c_str(), source.size(), entries, 3, "YuvOverlayPatch", [](ID3DBlob* const* blobs) {
                for (size_t i = 0; i < cached.size(); ++i) {
                    const auto* bytes = static_cast<const uint8_t*>(blobs[i]->GetBufferPointer());
                    cached[i].assign(bytes, bytes + blobs[i]->GetBufferSize());
                }
                return true;
            });
        DLL_Log("[YuvOverlayPatch] Shader preparation %s: elapsed=%llums (configuration, before video frames)",
                compiled ? "ready" : "failed", static_cast<unsigned long long>(GetTickCount64() - start));
    });
    if (cached[0].empty())
        return false;
    bytecode_ = cached;
    return true;
}

bool YuvOverlayPatcher::Init(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (!device || !context)
        return false;
    if (device_ == device && context_ == context && vertexShader_ && lumaShader_ && chromaShader_)
        return true;
    Release();

    // Missing preparation is a startup failure, never a request to compile on a video frame.
    if (bytecode_[0].empty() || bytecode_[1].empty() || bytecode_[2].empty())
        return false;
    ce::ComGuard<ID3D11VertexShader> vs;
    ce::ComGuard<ID3D11PixelShader> luma;
    ce::ComGuard<ID3D11PixelShader> chroma;
    if (FAILED(device->CreateVertexShader(bytecode_[0].data(), bytecode_[0].size(), nullptr, vs.addressof())) ||
        FAILED(device->CreatePixelShader(bytecode_[1].data(), bytecode_[1].size(), nullptr, luma.addressof())) ||
        FAILED(device->CreatePixelShader(bytecode_[2].data(), bytecode_[2].size(), nullptr, chroma.addressof()))) {
        DLL_Log("[YuvOverlayPatch] Prepared shader creation failed");
        return false;
    }

    D3D11_BUFFER_DESC desc = {};
    desc.Usage = D3D11_USAGE_DYNAMIC;
    desc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    desc.ByteWidth = sizeof(CopyConstants);
    ce::ComGuard<ID3D11Buffer> copyCb;
    ce::ComGuard<ID3D11Buffer> patchCb;
    if (FAILED(device->CreateBuffer(&desc, nullptr, copyCb.addressof())))
        return false;
    desc.ByteWidth = sizeof(PatchConstants);
    if (FAILED(device->CreateBuffer(&desc, nullptr, patchCb.addressof())))
        return false;

    device_ = device;
    context_ = context;
    vertexShader_ = vs.release();
    lumaShader_ = luma.release();
    chromaShader_ = chroma.release();
    copyConstants_ = copyCb.release();
    patchConstants_ = patchCb.release();
    DLL_Log("[YuvOverlayPatch] Retained-YUV overlay patch shaders created");
    return true;
}

RECT YuvOverlayPatcher::ClipToFrame(const RECT& rect, UINT width, UINT height) {
    RECT clipped = rect;
    clipped.left = std::clamp<LONG>(rect.left, 0, static_cast<LONG>(width));
    clipped.top = std::clamp<LONG>(rect.top, 0, static_cast<LONG>(height));
    clipped.right = std::clamp<LONG>(rect.right, 0, static_cast<LONG>(width));
    clipped.bottom = std::clamp<LONG>(rect.bottom, 0, static_cast<LONG>(height));
    return clipped;
}

RECT YuvOverlayPatcher::ChromaFootprint(const RECT& lumaRect, UINT width, UINT height) {
    // Chroma sample c reads luma columns 2c-1..2c+1 (both sitings) and rows
    // 2c-1..2c+1 (HDR) or 2c..2c+1 (SDR); take the wider HDR bound.
    auto floorHalf = [](LONG value) { return value >= 0 ? value / 2 : -((1 - value) / 2); };
    RECT footprint = {floorHalf(lumaRect.left - 1), floorHalf(lumaRect.top - 1), floorHalf(lumaRect.right) + 1,
                      floorHalf(lumaRect.bottom) + 1};
    return ClipToFrame(footprint, width / 2, height / 2);
}

bool YuvOverlayPatcher::Apply(ID3D11Texture2D* source, ID3D11RenderTargetView* lumaTarget,
                              ID3D11RenderTargetView* chromaTarget, UINT width, UINT height, const Layer& camera,
                              const Layer& cursor, const Params& params) {
    if (!source || !lumaTarget || !chromaTarget || !device_ || !context_ || width < 2 || height < 2)
        return false;
    const RECT cameraClip = camera.view ? ClipToFrame(camera.rect, width, height) : RECT{};
    const RECT cursorClip = cursor.view ? ClipToFrame(cursor.rect, width, height) : RECT{};
    if (IsEmpty(cameraClip) && IsEmpty(cursorClip))
        return true;

    ce::ComGuard<ID3D11Device3> device3;
    if (FAILED(device_->QueryInterface(IID_PPV_ARGS(device3.addressof()))))
        return false;
    D3D11_TEXTURE2D_DESC sourceDesc = {};
    source->GetDesc(&sourceDesc);
    const bool eightBit = sourceDesc.Format == DXGI_FORMAT_NV12;
    auto planeView = [&](DXGI_FORMAT format, UINT plane, ce::ComGuard<ID3D11ShaderResourceView1>& view) {
        D3D11_SHADER_RESOURCE_VIEW_DESC1 desc = {};
        desc.Format = format;
        desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MostDetailedMip = 0;
        desc.Texture2D.MipLevels = 1;
        desc.Texture2D.PlaneSlice = plane;
        return device3->CreateShaderResourceView1(source, &desc, view.addressof());
    };
    ce::ComGuard<ID3D11ShaderResourceView1> sourceLuma;
    ce::ComGuard<ID3D11ShaderResourceView1> sourceChroma;
    const HRESULT lumaHr = planeView(eightBit ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R16_UNORM, 0, sourceLuma);
    const HRESULT chromaHr = planeView(eightBit ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_UNORM, 1, sourceChroma);
    if (FAILED(lumaHr) || FAILED(chromaHr)) {
        DLL_Log("[YuvOverlayPatch] Plane SRV creation failed: fmt=%d bind=%x HR=%x/%x", sourceDesc.Format,
                sourceDesc.BindFlags, lumaHr, chromaHr);
        return false;
    }

    CopyConstants copy = {};
    copy.yuvFlags = ce::video_color::SdrYuvFlags(eightBit, params.bt2020, params.fullRange);
    copy.sdrWhiteNits = params.hdr && params.fullRange ? 1.0f : 0.0f;
    PatchConstants patch = {};
    SetRect(patch.cameraRect, camera);
    SetRect(patch.cursorRect, cursor);
    patch.frameSize[0] = static_cast<int32_t>(width);
    patch.frameSize[1] = static_cast<int32_t>(height);
    patch.patchHdr = params.hdr ? 1u : 0u;
    patch.rgbLevels = params.rgbLevels > 0.0f ? params.rgbLevels : 1023.0f;
    patch.storedToCode = eightBit ? 255.0f : 65535.0f / 64.0f;
    if (!WriteBuffer(context_, copyConstants_, copy) || !WriteBuffer(context_, patchConstants_, patch))
        return false;

    ID3D11ShaderResourceView* views[4] = {sourceLuma.get(), sourceChroma.get(), camera.view, cursor.view};
    ID3D11Buffer* buffers[2] = {copyConstants_, patchConstants_};
    context_->VSSetShader(vertexShader_, nullptr, 0);
    context_->PSSetConstantBuffers(0, 2, buffers);
    context_->PSSetShaderResources(1, 4, views);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context_->IASetInputLayout(nullptr);
    context_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    context_->RSSetState(nullptr);

    // A pixel covered by both rectangles is drawn twice with the same inputs
    // and therefore the same result.
    context_->OMSetRenderTargets(1, &lumaTarget, nullptr);
    context_->PSSetShader(lumaShader_, nullptr, 0);
    if (!IsEmpty(cameraClip))
        DrawRect(context_, cameraClip);
    if (!IsEmpty(cursorClip))
        DrawRect(context_, cursorClip);

    context_->OMSetRenderTargets(1, &chromaTarget, nullptr);
    context_->PSSetShader(chromaShader_, nullptr, 0);
    if (!IsEmpty(cameraClip))
        DrawRect(context_, ChromaFootprint(cameraClip, width, height));
    if (!IsEmpty(cursorClip))
        DrawRect(context_, ChromaFootprint(cursorClip, width, height));

    ID3D11RenderTargetView* nullRtv = nullptr;
    context_->OMSetRenderTargets(1, &nullRtv, nullptr);
    ID3D11ShaderResourceView* nullViews[4] = {};
    context_->PSSetShaderResources(1, 4, nullViews);
    return true;
}
