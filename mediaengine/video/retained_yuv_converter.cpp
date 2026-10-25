#include "retained_yuv_converter.h"

#include <cstring>
#include <string>

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

}  // namespace

RetainedYuvConverter::~RetainedYuvConverter() {
    Release();
}

void RetainedYuvConverter::ReleaseIntermediate() {
    SafeReleaseCom(intermediateSrv_);
    SafeReleaseCom(intermediateRtv_);
    SafeReleaseCom(intermediate_);
    intermediateWidth_ = 0;
    intermediateHeight_ = 0;
}

void RetainedYuvConverter::ReleaseChromaScratch() {
    SafeReleaseCom(chromaScratchSrv_);
    SafeReleaseCom(chromaScratchRtv_);
    SafeReleaseCom(chromaScratch_);
    chromaScratchWidth_ = 0;
    chromaScratchHeight_ = 0;
}

void RetainedYuvConverter::Release() {
    ReleaseIntermediate();
    ReleaseChromaScratch();
    SafeReleaseCom(kernels_.hdrPackShader);
    SafeReleaseCom(kernels_.sdrPackShader);
    SafeReleaseCom(kernels_.constants);
    SafeReleaseCom(kernels_.linearSampler);
    SafeReleaseCom(kernels_.pointSampler);
    SafeReleaseCom(kernels_.yuvChromaShader);
    SafeReleaseCom(kernels_.yuvLumaShader);
    SafeReleaseCom(kernels_.p010ChromaShader);
    SafeReleaseCom(kernels_.p010LumaShader);
    SafeReleaseCom(kernels_.normalizeShader);
    SafeReleaseCom(kernels_.vertexShader);
    SafeReleaseCom(device_);
    // The counts describe one converter lifetime; a later release (pool
    // teardown, then engine shutdown) must not report them again.
    conversions_ = 0;
    packedConversions_ = 0;
}

bool RetainedYuvConverter::EnsureKernels(ID3D11Device* device) {
    if (!device)
        return false;
    if (device_ == device && kernels_.vertexShader && kernels_.constants)
        return true;
    Release();

    // Same source, entry points and targets as VideoEncoder::EnsureSwapRBShader,
    // plus the packed P010 luma passes appended to that source.
    const std::string source =
        std::string(ce::video_color::kRgbColorConversionShaderSource) + ce::video_color::kPackedP010ShaderSource;
    const ce::video_color::RuntimeShaderEntry entries[] = {
        {"VS_Main", "vs_4_0"},   {"PS_Main", "ps_4_0"},        {"PS_P010Y", "ps_4_0"},
        {"PS_P010UV", "ps_4_0"}, {"PS_YuvY", "ps_4_0"},        {"PS_YuvUV", "ps_4_0"},
        {"PS_PackSdrP010", "ps_4_0"}, {"PS_PackHdrP010", "ps_4_0"}};
    constexpr int kPixelShaders = 7;
    ce::ComGuard<ID3D11VertexShader> vs;
    ce::ComGuard<ID3D11PixelShader> pixel[kPixelShaders];
    const bool created = ce::video_color::PrepareShaderBytecode(
        source.c_str(), source.size(), entries, kPixelShaders + 1, "RetainedYuv",
        [&](const ce::video_color::ShaderBytecodeView* shaders) {
            if (FAILED(device->CreateVertexShader(shaders[0].data, shaders[0].size, nullptr, vs.addressof())))
                return false;
            for (int i = 0; i < kPixelShaders; ++i) {
                if (FAILED(device->CreatePixelShader(shaders[i + 1].data, shaders[i + 1].size, nullptr,
                                                     pixel[i].addressof())))
                    return false;
            }
            return true;
        });
    if (!created)
        return false;

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D11_SAMPLER_DESC sampler = {};
    sampler.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ce::ComGuard<ID3D11SamplerState> pointSampler;
    ce::ComGuard<ID3D11SamplerState> linearSampler;
    if (FAILED(device->CreateSamplerState(&sampler, pointSampler.addressof())))
        return false;
    sampler.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    if (FAILED(device->CreateSamplerState(&sampler, linearSampler.addressof())))
        return false;
    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = 32;
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ce::ComGuard<ID3D11Buffer> constants;
    if (FAILED(device->CreateBuffer(&cbDesc, nullptr, constants.addressof())))
        return false;

    device->AddRef();
    device_ = device;
    kernels_.vertexShader = vs.release();
    kernels_.normalizeShader = pixel[0].release();
    kernels_.p010LumaShader = pixel[1].release();
    kernels_.p010ChromaShader = pixel[2].release();
    kernels_.yuvLumaShader = pixel[3].release();
    kernels_.yuvChromaShader = pixel[4].release();
    kernels_.sdrPackShader = pixel[5].release();
    kernels_.hdrPackShader = pixel[6].release();
    kernels_.pointSampler = pointSampler.release();
    kernels_.linearSampler = linearSampler.release();
    kernels_.constants = constants.release();
    DLL_Log("[RetainedYuv] Ingress conversion shaders created (encoder shader source and passes)");
    return true;
}

bool RetainedYuvConverter::EnsureIntermediate(UINT width, UINT height) {
    if (intermediate_ && intermediateWidth_ == width && intermediateHeight_ == height)
        return true;
    ReleaseIntermediate();
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &intermediate_);
    if (SUCCEEDED(hr))
        hr = device_->CreateRenderTargetView(intermediate_, nullptr, &intermediateRtv_);
    if (SUCCEEDED(hr))
        hr = device_->CreateShaderResourceView(intermediate_, nullptr, &intermediateSrv_);
    if (FAILED(hr)) {
        DLL_Log("[RetainedYuv] RGB10 intermediate creation failed: %ux%u HR=%x", width, height, hr);
        ReleaseIntermediate();
        return false;
    }
    intermediateWidth_ = width;
    intermediateHeight_ = height;
    return true;
}

bool RetainedYuvConverter::EnsureChromaScratch(UINT width, UINT height) {
    if (chromaScratch_ && chromaScratchWidth_ == width && chromaScratchHeight_ == height)
        return true;
    ReleaseChromaScratch();
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R16G16_UNORM;  // the P010 chroma plane view format, so the codes are identical
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device_->CreateTexture2D(&desc, nullptr, &chromaScratch_);
    if (SUCCEEDED(hr))
        hr = device_->CreateRenderTargetView(chromaScratch_, nullptr, &chromaScratchRtv_);
    if (SUCCEEDED(hr))
        hr = device_->CreateShaderResourceView(chromaScratch_, nullptr, &chromaScratchSrv_);
    if (FAILED(hr)) {
        DLL_Log("[RetainedYuv] Packed P010 chroma scratch creation failed: %ux%u HR=%x", width, height, hr);
        ReleaseChromaScratch();
        return false;
    }
    chromaScratchWidth_ = width;
    chromaScratchHeight_ = height;
    DLL_Log("[RetainedYuv] Packed P010 chroma scratch created: %ux%u (%.1f MB, one per converter)", width, height,
            static_cast<double>(width) * height * 4.0 / (1024.0 * 1024.0));
    return true;
}

bool RetainedYuvConverter::Convert(ID3D11DeviceContext* context, const Request& request) {
    const bool packed = request.packedTarget != nullptr;
    const bool targetsValid = packed ? !request.eightBit : (request.lumaTarget && request.chromaTarget);
    if (!context || !device_ || !request.source || !targetsValid || request.width < 2 || request.height < 2)
        return false;
    ID3D11ShaderResourceView* yuvInput = request.source;
    if (request.normalize != ce::video_format::RgbColorTransform::kNone) {
        // The tone-map transforms never reach here (retained_frame_route.h),
        // so the SDR white the encoder would pass is irrelevant to the result.
        if (!EnsureIntermediate(request.width, request.height) ||
            !ce::video_color::DrawRgbNormalize(context, kernels_, request.source, intermediateRtv_, request.width,
                                               request.height, request.normalize, 80.0f))
            return false;
        yuvInput = intermediateSrv_;
    }
    if (packed) {
        if (!EnsureChromaScratch(request.width / 2, request.height / 2))
            return false;
        const ce::video_color::PackedP010Targets targets = {request.packedTarget, chromaScratchRtv_,
                                                            chromaScratchSrv_, request.width, request.height};
        const bool converted =
            request.hdrOutput
                ? ce::video_color::DrawHdrRgb10ToPackedP010(context, kernels_, yuvInput, targets, 0.0f,
                                                            request.fullRange)
                : ce::video_color::DrawSdrRgbToPackedP010(
                      context, kernels_, yuvInput, targets,
                      ce::video_color::SdrYuvFlags(false, request.bt2020, request.fullRange));
        if (converted) {
            ++conversions_;
            ++packedConversions_;
        }
        return converted;
    }
    const ce::video_color::YuvPlaneTargets targets = {request.lumaTarget, request.chromaTarget, request.width,
                                                      request.height};
    const bool converted =
        request.hdrOutput
            ? ce::video_color::DrawHdrRgb10ToP010(context, kernels_, yuvInput, targets, 0.0f, request.fullRange)
            : ce::video_color::DrawSdrRgbToYuv(
                  context, kernels_, yuvInput, targets,
                  ce::video_color::SdrYuvFlags(request.eightBit, request.bt2020, request.fullRange));
    if (converted)
        ++conversions_;
    return converted;
}
