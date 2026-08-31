#include "face_camera_renderer.h"
#include "face_camera_shader.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3dcompiler.h>
#include <mutex>
#include <utility>
#include <vector>
#include "../common/raii_helpers.h"
#include "../common/secure_dll_loading.h"
#include "mediaengine.h"
#include "video_format_policy.h"

namespace {

DXGI_FORMAT TypedCameraFormat(DXGI_FORMAT format) {
    if (format == DXGI_FORMAT_B8G8R8A8_TYPELESS)
        return DXGI_FORMAT_B8G8R8A8_UNORM;
    if (format == DXGI_FORMAT_B8G8R8X8_TYPELESS)
        return DXGI_FORMAT_B8G8R8X8_UNORM;
    if (format == DXGI_FORMAT_R8G8B8A8_TYPELESS)
        return DXGI_FORMAT_R8G8B8A8_UNORM;
    return format;
}

const char* ShapeName(ce::face_camera::Shape shape) {
    switch (shape) {
        case ce::face_camera::Shape::kRectangle: return "rectangle";
        case ce::face_camera::Shape::kRoundedRectangle: return "rounded";
        case ce::face_camera::Shape::kCircle: return "circle";
    }
    return "unknown";
}

const char* PositionName(ce::face_camera::Position position) {
    switch (position) {
        case ce::face_camera::Position::kTopLeft: return "top_left";
        case ce::face_camera::Position::kTopCenter: return "top_center";
        case ce::face_camera::Position::kTopRight: return "top_right";
        case ce::face_camera::Position::kCenterLeft: return "center_left";
        case ce::face_camera::Position::kCenter: return "center";
        case ce::face_camera::Position::kCenterRight: return "center_right";
        case ce::face_camera::Position::kBottomLeft: return "bottom_left";
        case ce::face_camera::Position::kBottomCenter: return "bottom_center";
        case ce::face_camera::Position::kBottomRight: return "bottom_right";
        case ce::face_camera::Position::kCustom: return "custom";
    }
    return "unknown";
}

const char* CropName(ce::face_camera::Crop crop) {
    return crop == ce::face_camera::Crop::kStretch ? "stretch" : "fill";
}

struct FaceCameraShaderBytecode {
    std::vector<uint8_t> vertex;
    std::vector<uint8_t> pixel;
};

const FaceCameraShaderBytecode& GetFaceCameraShaderBytecode() {
    static FaceCameraShaderBytecode bytecode;
    static std::once_flag compileOnce;
    std::call_once(compileOnce, [&] {
        ce::ModuleGuard compilerModule(ce::security::LoadSystemLibrary(L"d3dcompiler_47.dll"));
        if (!compilerModule) {
            DLL_Log("[FaceCamera] Failed to load the system D3D shader compiler");
            return;
        }
        using CompileFn = HRESULT(WINAPI*)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                           LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
        auto compile = reinterpret_cast<CompileFn>(GetProcAddress(compilerModule.get(), "D3DCompile"));
        if (!compile)
            return;

        ce::ComGuard<ID3DBlob> vertexBlob;
        ce::ComGuard<ID3DBlob> pixelBlob;
        ce::ComGuard<ID3DBlob> errors;
        HRESULT hr = compile(ce::face_camera::kShaderSource, std::strlen(ce::face_camera::kShaderSource),
                             "face_camera", nullptr, nullptr, "VS_Main", "vs_4_0",
                             D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, vertexBlob.put(), errors.put());
        if (FAILED(hr)) {
            DLL_Log("[FaceCamera] Vertex shader compilation failed: %s",
                    errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no compiler detail");
            return;
        }
        errors.reset();
        hr = compile(ce::face_camera::kShaderSource, std::strlen(ce::face_camera::kShaderSource), "face_camera",
                     nullptr, nullptr, "PS_Main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, pixelBlob.put(),
                     errors.put());
        if (FAILED(hr)) {
            DLL_Log("[FaceCamera] Pixel shader compilation failed: %s",
                    errors ? static_cast<const char*>(errors->GetBufferPointer()) : "no compiler detail");
            return;
        }

        const auto* vertexBytes = static_cast<const uint8_t*>(vertexBlob->GetBufferPointer());
        const auto* pixelBytes = static_cast<const uint8_t*>(pixelBlob->GetBufferPointer());
        bytecode.vertex.assign(vertexBytes, vertexBytes + vertexBlob->GetBufferSize());
        bytecode.pixel.assign(pixelBytes, pixelBytes + pixelBlob->GetBufferSize());
        DLL_Log("[FaceCamera] Optimized shader bytecode precompiled outside the video-frame path");
    });
    return bytecode;
}

}  // namespace

FaceCameraRenderer::FaceCameraRenderer(ce::face_camera::Config config)
    : config_(std::move(config)), capture_(std::make_unique<FaceCameraCapture>(config_)) {
    (void)GetFaceCameraShaderBytecode();
    DLL_Log(
        "[FaceCamera] Configuration: request=%ux%u auto=%d fps=%u position=%s shape=%s crop=%s "
        "width=%.2f%% margin=%.2f%% opacity=%.1f%% mirror=%d staleMs=%u",
        config_.requestedWidth, config_.requestedHeight,
        config_.requestedWidth == 0 && config_.requestedHeight == 0 ? 1 : 0, config_.requestedFps,
        PositionName(config_.position), ShapeName(config_.shape), CropName(config_.crop), config_.widthPercent,
        config_.marginPercent, config_.opacity * 100.0f, config_.mirror ? 1 : 0, config_.staleTimeoutMs);
}

FaceCameraRenderer::~FaceCameraRenderer() {
    Stop();
}

bool FaceCameraRenderer::StartCapture(ID3D11Device* device) {
    if (captureStarted_)
        return true;
    captureStartTickMs_ = GetTickCount64();
    captureStarted_ = capture_ && capture_->Start(device);
    if (!captureStarted_)
        captureStartTickMs_ = 0;
    return captureStarted_;
}

bool FaceCameraRenderer::InitRenderer(ID3D11Device* device, ID3D11DeviceContext* context) {
    if (resourcesCreated_)
        return true;
    if (!device || !context)
        return false;
    device_ = device;
    context_ = context;
    return CreateRenderingResources();
}

void FaceCameraRenderer::Stop() {
    if (capture_)
        capture_->Stop();
    captureStarted_ = false;
    captureStartTickMs_ = 0;
    CleanupRendererResources();
}

bool FaceCameraRenderer::CreateRenderingResources() {
    const auto& bytecode = GetFaceCameraShaderBytecode();
    if (bytecode.vertex.empty() || bytecode.pixel.empty())
        return false;
    if (FAILED(device_->CreateVertexShader(bytecode.vertex.data(), bytecode.vertex.size(), nullptr, &vertexShader_)) ||
        FAILED(device_->CreatePixelShader(bytecode.pixel.data(), bytecode.pixel.size(), nullptr, &pixelShader_))) {
        CleanupRendererResources();
        return false;
    }

    D3D11_BUFFER_DESC bufferDesc = {};
    bufferDesc.ByteWidth = sizeof(Constants);
    bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
    bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    if (FAILED(device_->CreateBuffer(&bufferDesc, nullptr, &constantBuffer_))) {
        CleanupRendererResources();
        return false;
    }

    D3D11_SAMPLER_DESC samplerDesc;
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    samplerDesc.AddressU = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressV = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    samplerDesc.MipLODBias = 0.0f;
    samplerDesc.MaxAnisotropy = 1;
    samplerDesc.ComparisonFunc = D3D11_COMPARISON_NEVER;
    for (float& borderComponent : samplerDesc.BorderColor)
        borderComponent = 0.0f;
    samplerDesc.MinLOD = 0.0f;
    samplerDesc.MaxLOD = D3D11_FLOAT32_MAX;
    if (FAILED(device_->CreateSamplerState(&samplerDesc, &sampler_))) {
        CleanupRendererResources();
        return false;
    }

    D3D11_BLEND_DESC blendDesc;
    blendDesc.AlphaToCoverageEnable = FALSE;
    blendDesc.IndependentBlendEnable = FALSE;
    for (D3D11_RENDER_TARGET_BLEND_DESC& target : blendDesc.RenderTarget) {
        target.BlendEnable = FALSE;
        target.SrcBlend = D3D11_BLEND_ONE;
        target.DestBlend = D3D11_BLEND_ZERO;
        target.BlendOp = D3D11_BLEND_OP_ADD;
        target.SrcBlendAlpha = D3D11_BLEND_ONE;
        target.DestBlendAlpha = D3D11_BLEND_ZERO;
        target.BlendOpAlpha = D3D11_BLEND_OP_ADD;
        target.RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    }
    blendDesc.RenderTarget[0].BlendEnable = TRUE;
    blendDesc.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    blendDesc.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    blendDesc.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    blendDesc.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device_->CreateBlendState(&blendDesc, &blendState_))) {
        CleanupRendererResources();
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizerDesc;
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.FrontCounterClockwise = FALSE;
    rasterizerDesc.DepthBias = 0;
    rasterizerDesc.DepthBiasClamp = 0.0f;
    rasterizerDesc.SlopeScaledDepthBias = 0.0f;
    rasterizerDesc.DepthClipEnable = TRUE;
    rasterizerDesc.ScissorEnable = FALSE;
    rasterizerDesc.MultisampleEnable = FALSE;
    rasterizerDesc.AntialiasedLineEnable = FALSE;
    if (FAILED(device_->CreateRasterizerState(&rasterizerDesc, &rasterizerState_))) {
        CleanupRendererResources();
        return false;
    }
    resourcesCreated_ = true;
    DLL_Log("[FaceCamera] GPU renderer ready: analytic shape/border, trilinear scaling, one draw per output frame");
    return true;
}

bool FaceCameraRenderer::EnsureCameraTexture(uint32_t width, uint32_t height, DXGI_FORMAT format) {
    format = TypedCameraFormat(format);
    if (cameraTexture_ && cameraWidth_ == width && cameraHeight_ == height && cameraFormat_ == format)
        return true;
    if (cameraSrv_) {
        cameraSrv_->Release();
        cameraSrv_ = nullptr;
    }
    if (cameraTexture_) {
        cameraTexture_->Release();
        cameraTexture_ = nullptr;
    }

    UINT formatSupport = 0;
    const bool supportsMipGeneration =
        SUCCEEDED(device_->CheckFormatSupport(format, &formatSupport)) &&
        (formatSupport & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN) != 0 &&
        (formatSupport & D3D11_FORMAT_SUPPORT_RENDER_TARGET) != 0;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = supportsMipGeneration ? 0 : 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (supportsMipGeneration ? D3D11_BIND_RENDER_TARGET : 0);
    desc.MiscFlags = supportsMipGeneration ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &cameraTexture_)) ||
        FAILED(device_->CreateShaderResourceView(cameraTexture_, nullptr, &cameraSrv_))) {
        if (cameraTexture_) {
            cameraTexture_->Release();
            cameraTexture_ = nullptr;
        }
        return false;
    }
    cameraWidth_ = width;
    cameraHeight_ = height;
    cameraFormat_ = format;
    D3D11_TEXTURE2D_DESC created = {};
    cameraTexture_->GetDesc(&created);
    DLL_Log("[FaceCamera] GPU camera texture allocated: %ux%u fmt=%d mips=%u", width, height, format,
            created.MipLevels);
    return true;
}

bool FaceCameraRenderer::CopyGpuFrame(const FaceCameraFrame& frame) {
    ce::ComGuard<IMFMediaBuffer> buffer;
    ce::ComGuard<IMFDXGIBuffer> dxgiBuffer;
    ce::ComGuard<ID3D11Texture2D> source;
    if (!frame.gpuSample || FAILED(frame.gpuSample->GetBufferByIndex(0, buffer.put())) || !buffer ||
        FAILED(buffer->QueryInterface(IID_PPV_ARGS(dxgiBuffer.put()))) || !dxgiBuffer ||
        FAILED(dxgiBuffer->GetResource(IID_PPV_ARGS(source.put()))) || !source) {
        return false;
    }
    D3D11_TEXTURE2D_DESC desc = {};
    source->GetDesc(&desc);
    if (!EnsureCameraTexture(frame.width, frame.height, desc.Format))
        return false;
    const D3D11_BOX sourceBox = {0, 0, 0, frame.width, frame.height, 1};
    context_->CopySubresourceRegion(cameraTexture_, 0, 0, 0, 0, source.get(), frame.gpuSubresource, &sourceBox);
    return true;
}

bool FaceCameraRenderer::UpdateCameraTexture() {
    if (!capture_ || !resourcesCreated_)
        return false;
    const auto frame = capture_->LatestFrame();
    if (!frame)
        return cameraTexture_ != nullptr;
    if (frame->sequence == uploadedSequence_)
        return cameraTexture_ != nullptr;

    bool updated = frame->gpuSample ? CopyGpuFrame(*frame) : false;
    if (!updated && !frame->bgra.empty() &&
        EnsureCameraTexture(frame->width, frame->height, DXGI_FORMAT_B8G8R8A8_UNORM)) {
        context_->UpdateSubresource(cameraTexture_, 0, nullptr, frame->bgra.data(), frame->width * 4u, 0);
        updated = true;
    }
    if (!updated) {
        // Do not retry a failed immutable sample at output cadence. A later
        // camera sequence gets a fresh attempt while the prior good texture,
        // if any, remains available until its stale deadline.
        uploadedSequence_ = frame->sequence;
        if (!transportFailureLogged_) {
            DLL_Log("[FaceCamera] Failed to transfer a camera frame to the encoder GPU; retaining prior frame");
            transportFailureLogged_ = true;
        }
        return cameraTexture_ != nullptr;
    }

    D3D11_TEXTURE2D_DESC desc = {};
    cameraTexture_->GetDesc(&desc);
    if (desc.MipLevels > 1)
        context_->GenerateMips(cameraSrv_);
    uploadedSequence_ = frame->sequence;
    uploadedTickMs_ = frame->receivedTickMs;
    ++cameraUploadCount_;
    staleLogged_ = false;
    return true;
}

bool FaceCameraRenderer::GetOverlayFrameRect(uint32_t targetWidth, uint32_t targetHeight, uint32_t displayedWidth,
                                             uint32_t displayedHeight, RECT* result) {
    if (!result || !UpdateCameraTexture() || !cameraTexture_)
        return false;
    const uint64_t now = GetTickCount64();
    if (ce::face_camera::IsFrameStale(uploadedTickMs_, now, config_.staleTimeoutMs)) {
        if (!staleLogged_) {
            DLL_Log("[FaceCamera] Camera frame exceeded stale_timeout_ms=%u; overlay hidden without blocking video",
                    config_.staleTimeoutMs);
            staleLogged_ = true;
        }
        // Retire the stale GPU image only when a fresh/repeated base frame is
        // actually being recomposed. The caller can then cache that
        // camera-free result before dropping its larger RGB repeat source.
        if (cameraSrv_) {
            cameraSrv_->Release();
            cameraSrv_ = nullptr;
        }
        if (cameraTexture_) {
            cameraTexture_->Release();
            cameraTexture_ = nullptr;
        }
        cameraWidth_ = 0;
        cameraHeight_ = 0;
        cameraFormat_ = DXGI_FORMAT_UNKNOWN;
        preparedLayout_ = {};
        return false;
    }
    preparedLayout_ = ce::face_camera::ResolveLayout(config_, targetWidth, targetHeight, displayedWidth,
                                                     displayedHeight, cameraWidth_, cameraHeight_);
    if (!preparedLayout_.valid)
        return false;
    *result = {preparedLayout_.left, preparedLayout_.top, preparedLayout_.right, preparedLayout_.bottom};
    return true;
}

bool FaceCameraRenderer::NeedsDynamicRepeatSource() const {
    // A retained texture still needs one final recomposition when its stale
    // deadline passes. GetOverlayFrameRect retires it only while producing
    // that camera-free successor frame.
    if (cameraTexture_)
        return true;
    if (!capture_)
        return false;
    const auto latest = capture_->LatestFrame();
    if (latest && latest->sequence != uploadedSequence_)
        return true;
    if (uploadedSequence_ != 0 || !capture_->MayProduceFrames())
        return false;
    // Retain an RGB repeat source briefly while the first asynchronous sample
    // arrives. A hung/no-frame driver must not impose a permanent full-frame
    // cache copy; a later sample is picked up on the next fresh game frame.
    return ce::face_camera::IsWithinInitialRepeatSourceGrace(captureStartTickMs_, GetTickCount64(),
                                                              config_.staleTimeoutMs);
}

void FaceCameraRenderer::ClearTargetRenderViewCache() {
    for (auto& entry : targetRtvCache_) {
        if (entry.view)
            entry.view->Release();
        entry = {};
    }
    targetRtvUseCounter_ = 0;
    targetRtvWidth_ = 0;
    targetRtvHeight_ = 0;
    targetRtvArraySize_ = 0;
    targetRtvFormat_ = DXGI_FORMAT_UNKNOWN;
    targetRtvSampleDesc_ = {};
}

ID3D11RenderTargetView* FaceCameraRenderer::GetTargetRenderView(ID3D11Texture2D* targetTexture,
                                                                const D3D11_TEXTURE2D_DESC& targetDesc) {
    const bool targetClassChanged = targetRtvWidth_ != targetDesc.Width || targetRtvHeight_ != targetDesc.Height ||
                                    targetRtvArraySize_ != targetDesc.ArraySize || targetRtvFormat_ != targetDesc.Format ||
                                    targetRtvSampleDesc_.Count != targetDesc.SampleDesc.Count ||
                                    targetRtvSampleDesc_.Quality != targetDesc.SampleDesc.Quality;
    if (targetClassChanged) {
        ClearTargetRenderViewCache();
        targetRtvWidth_ = targetDesc.Width;
        targetRtvHeight_ = targetDesc.Height;
        targetRtvArraySize_ = targetDesc.ArraySize;
        targetRtvFormat_ = targetDesc.Format;
        targetRtvSampleDesc_ = targetDesc.SampleDesc;
    }

    ++targetRtvUseCounter_;
    TargetRtvCacheEntry* replacement = nullptr;
    for (auto& entry : targetRtvCache_) {
        if (entry.texture == targetTexture && entry.view) {
            entry.lastUsed = targetRtvUseCounter_;
            return entry.view;
        }
        if (!replacement || !entry.view || entry.lastUsed < replacement->lastUsed) {
            replacement = &entry;
            if (!entry.view)
                break;
        }
    }

    D3D11_RENDER_TARGET_VIEW_DESC viewDesc = {};
    viewDesc.Format = ce::video_format::GetRgbShaderResourceViewFormat(targetDesc.Format);
    if (viewDesc.Format == DXGI_FORMAT_UNKNOWN)
        return nullptr;
    if (targetDesc.ArraySize > 1) {
        if (targetDesc.SampleDesc.Count > 1) {
            viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMSARRAY;
            viewDesc.Texture2DMSArray.ArraySize = 1;
        } else {
            viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            viewDesc.Texture2DArray.MipSlice = 0;
            viewDesc.Texture2DArray.ArraySize = 1;
        }
    } else if (targetDesc.SampleDesc.Count > 1) {
        viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DMS;
    } else {
        viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        viewDesc.Texture2D.MipSlice = 0;
    }

    ID3D11RenderTargetView* view = nullptr;
    if (!replacement || FAILED(device_->CreateRenderTargetView(targetTexture, &viewDesc, &view)))
        return nullptr;
    if (replacement->view)
        replacement->view->Release();
    replacement->texture = targetTexture;
    replacement->view = view;
    replacement->lastUsed = targetRtvUseCounter_;
    return view;
}

bool FaceCameraRenderer::CompositePreparedFrame(ID3D11Texture2D* targetTexture, uint32_t targetWidth,
                                                uint32_t targetHeight, FaceCameraColorMode colorMode,
                                                float paperWhiteNits) {
    if (!targetTexture || !preparedLayout_.valid || !cameraSrv_ || !resourcesCreated_)
        return false;
    D3D11_TEXTURE2D_DESC targetDesc = {};
    targetTexture->GetDesc(&targetDesc);
    const float resolvedPaperWhiteNits =
        std::clamp(std::isfinite(paperWhiteNits) ? paperWhiteNits : 203.0f, 80.0f, 1000.0f);
    if (!colorMappingLogged_ || lastColorMode_ != colorMode || lastTargetFormat_ != targetDesc.Format ||
        std::abs(lastPaperWhiteNits_ - resolvedPaperWhiteNits) >= 0.5f) {
        const char* mode = colorMode == FaceCameraColorMode::ScRgb
                               ? "scRGB-linear-P709"
                               : (colorMode == FaceCameraColorMode::Hdr10Pq ? "PQ-P2020" : "SDR-sRGB");
        DLL_Log("[FaceCamera Color] SDR camera mapping: targetFmt=%d target=%s paperWhite=%.1f-nit",
                targetDesc.Format, mode, resolvedPaperWhiteNits);
        colorMappingLogged_ = true;
        lastColorMode_ = colorMode;
        lastTargetFormat_ = targetDesc.Format;
        lastPaperWhiteNits_ = resolvedPaperWhiteNits;
    }
    ID3D11RenderTargetView* targetView = GetTargetRenderView(targetTexture, targetDesc);
    if (!targetView)
        return false;

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context_->Map(constantBuffer_, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;
    auto* constants = static_cast<Constants*>(mapped.pData);
    constants->destinationRect[0] = preparedLayout_.normalizedLeft;
    constants->destinationRect[1] = preparedLayout_.normalizedTop;
    constants->destinationRect[2] = preparedLayout_.normalizedWidth;
    constants->destinationRect[3] = preparedLayout_.normalizedHeight;
    constants->sourceRect[0] = preparedLayout_.sourceU0;
    constants->sourceRect[1] = preparedLayout_.sourceV0;
    constants->sourceRect[2] = preparedLayout_.sourceU1;
    constants->sourceRect[3] = preparedLayout_.sourceV1;
    constants->shape[0] = static_cast<float>(config_.shape);
    constants->shape[1] = std::clamp(config_.cornerRadiusPercent, 0.0f, 50.0f) / 100.0f;
    constants->shape[2] = std::clamp(config_.borderWidthPercent, 0.0f, 10.0f) / 100.0f;
    constants->shape[3] = std::clamp(config_.opacity, 0.0f, 1.0f);
    constants->color[0] = static_cast<float>(colorMode);
    constants->color[1] = resolvedPaperWhiteNits;
    constants->color[2] = config_.mirror ? 1.0f : 0.0f;
    constants->color[3] = 0.0f;
    constants->borderColor[0] = static_cast<float>((config_.borderColorRgb >> 16) & 0xFFu) / 255.0f;
    constants->borderColor[1] = static_cast<float>((config_.borderColorRgb >> 8) & 0xFFu) / 255.0f;
    constants->borderColor[2] = static_cast<float>(config_.borderColorRgb & 0xFFu) / 255.0f;
    constants->borderColor[3] = 1.0f;
    constants->displayedSize[0] = preparedLayout_.displayedWidthPixels;
    constants->displayedSize[1] = preparedLayout_.displayedHeightPixels;
    constants->displayedSize[2] = 0.0f;
    constants->displayedSize[3] = 0.0f;
    context_->Unmap(constantBuffer_, 0);

    ID3D11RenderTargetView* oldView = nullptr;
    ID3D11DepthStencilView* oldDepth = nullptr;
    D3D11_VIEWPORT oldViewport = {};
    UINT viewportCount = 1;
    context_->OMGetRenderTargets(1, &oldView, &oldDepth);
    context_->RSGetViewports(&viewportCount, &oldViewport);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(targetWidth);
    viewport.Height = static_cast<float>(targetHeight);
    viewport.MaxDepth = 1.0f;
    context_->OMSetRenderTargets(1, &targetView, nullptr);
    context_->RSSetViewports(1, &viewport);
    context_->RSSetState(rasterizerState_);
    const float blendFactor[4] = {};
    context_->OMSetBlendState(blendState_, blendFactor, 0xFFFFFFFFu);
    context_->VSSetShader(vertexShader_, nullptr, 0);
    context_->PSSetShader(pixelShader_, nullptr, 0);
    context_->VSSetConstantBuffers(0, 1, &constantBuffer_);
    context_->PSSetConstantBuffers(0, 1, &constantBuffer_);
    context_->PSSetShaderResources(0, 1, &cameraSrv_);
    context_->PSSetSamplers(0, 1, &sampler_);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->Draw(4, 0);

    ID3D11ShaderResourceView* nullView = nullptr;
    context_->PSSetShaderResources(0, 1, &nullView);
    context_->OMSetRenderTargets(1, &oldView, oldDepth);
    context_->RSSetViewports(viewportCount ? 1 : 0, viewportCount ? &oldViewport : nullptr);
    if (oldView)
        oldView->Release();
    if (oldDepth)
        oldDepth->Release();

    ++compositeCount_;
    if (compositeCount_ == 1 || compositeCount_ % 600 == 0) {
        DLL_Log(
            "[FaceCamera] GPU composite count=%llu cameraUploads=%llu target=%ux%u rect=%dx%d "
            "camera=%ux%u staleMs=%llu",
            static_cast<unsigned long long>(compositeCount_), static_cast<unsigned long long>(cameraUploadCount_),
            targetWidth, targetHeight, preparedLayout_.right - preparedLayout_.left,
            preparedLayout_.bottom - preparedLayout_.top, cameraWidth_, cameraHeight_,
            static_cast<unsigned long long>(uploadedTickMs_ ? GetTickCount64() - uploadedTickMs_ : 0));
    }
    return true;
}

void FaceCameraRenderer::CleanupRendererResources() {
    ClearTargetRenderViewCache();
    if (cameraSrv_)
        cameraSrv_->Release();
    if (cameraTexture_)
        cameraTexture_->Release();
    if (vertexShader_)
        vertexShader_->Release();
    if (pixelShader_)
        pixelShader_->Release();
    if (constantBuffer_)
        constantBuffer_->Release();
    if (sampler_)
        sampler_->Release();
    if (blendState_)
        blendState_->Release();
    if (rasterizerState_)
        rasterizerState_->Release();
    cameraSrv_ = nullptr;
    cameraTexture_ = nullptr;
    vertexShader_ = nullptr;
    pixelShader_ = nullptr;
    constantBuffer_ = nullptr;
    sampler_ = nullptr;
    blendState_ = nullptr;
    rasterizerState_ = nullptr;
    cameraWidth_ = 0;
    cameraHeight_ = 0;
    cameraFormat_ = DXGI_FORMAT_UNKNOWN;
    uploadedSequence_ = 0;
    uploadedTickMs_ = 0;
    compositeCount_ = 0;
    cameraUploadCount_ = 0;
    preparedLayout_ = {};
    transportFailureLogged_ = false;
    staleLogged_ = false;
    colorMappingLogged_ = false;
    lastTargetFormat_ = DXGI_FORMAT_UNKNOWN;
    lastPaperWhiteNits_ = 0.0f;
    resourcesCreated_ = false;
    device_ = nullptr;
    context_ = nullptr;
}
