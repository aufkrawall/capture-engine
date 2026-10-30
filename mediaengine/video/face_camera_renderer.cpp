#include "face_camera_renderer.h"

#include "face_camera_shader.h"
#include "gpu_memory_breakdown.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <d3d11_3.h>
#include <d3dcompiler.h>
#include <mutex>
#include <utility>
#include <vector>
#include "common/logging/log_meter.h"
#include "common/platform/raii_helpers.h"
#include "common/platform/secure_dll_loading.h"
#include "mediaengine/engine/mediaengine.h"
#include "runtime_shader_compiler.h"
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

bool CreateNv12PlaneViews(ID3D11Device* device, ID3D11Texture2D* texture, ID3D11ShaderResourceView** lumaOut,
                          ID3D11ShaderResourceView** chromaOut) {
    ce::ComGuard<ID3D11Device3> device3;
    if (!device || !texture || !lumaOut || !chromaOut || FAILED(device->QueryInterface(IID_PPV_ARGS(device3.put()))) ||
        !device3) {
        return false;
    }
    D3D11_SHADER_RESOURCE_VIEW_DESC1 lumaDesc = {};
    lumaDesc.Format = DXGI_FORMAT_R8_UNORM;
    lumaDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    lumaDesc.Texture2D.MostDetailedMip = 0;
    lumaDesc.Texture2D.MipLevels = 1;
    lumaDesc.Texture2D.PlaneSlice = 0;
    D3D11_SHADER_RESOURCE_VIEW_DESC1 chromaDesc = lumaDesc;
    chromaDesc.Format = DXGI_FORMAT_R8G8_UNORM;
    chromaDesc.Texture2D.PlaneSlice = 1;

    ce::ComGuard<ID3D11ShaderResourceView1> lumaSrv1;
    ce::ComGuard<ID3D11ShaderResourceView1> chromaSrv1;
    if (FAILED(device3->CreateShaderResourceView1(texture, &lumaDesc, lumaSrv1.put())) ||
        FAILED(device3->CreateShaderResourceView1(texture, &chromaDesc, chromaSrv1.put()))) {
        return false;
    }
    *lumaOut = lumaSrv1.detach();
    *chromaOut = chromaSrv1.detach();
    return true;
}

bool CreateSrvTexture2D(ID3D11Device* device, uint32_t width, uint32_t height, DXGI_FORMAT format,
                        ID3D11Texture2D** texOut, ID3D11ShaderResourceView** srvOut) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, texOut)) &&
           SUCCEEDED(device->CreateShaderResourceView(*texOut, nullptr, srvOut));
}

struct FaceCameraShaderBytecode {
    std::vector<uint8_t> vertex;
    std::vector<uint8_t> pixel;
};

const FaceCameraShaderBytecode& GetFaceCameraShaderBytecode() {
    static FaceCameraShaderBytecode bytecode;
    static std::once_flag compileOnce;
    std::call_once(compileOnce, [&] {
        const ce::video_color::RuntimeShaderEntry entries[] = {{"VS_Main", "vs_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3},
                                                               {"PS_Main", "ps_4_0", D3DCOMPILE_OPTIMIZATION_LEVEL3}};
        const bool prepared = ce::video_color::PrepareShaderBytecode(
            ce::face_camera::kShaderSource, std::strlen(ce::face_camera::kShaderSource), entries, 2, "FaceCamera",
            [&](const ce::video_color::ShaderBytecodeView* shaders) {
                const auto* vertexBytes = static_cast<const uint8_t*>(shaders[0].data);
                const auto* pixelBytes = static_cast<const uint8_t*>(shaders[1].data);
                bytecode.vertex.assign(vertexBytes, vertexBytes + shaders[0].size);
                bytecode.pixel.assign(pixelBytes, pixelBytes + shaders[1].size);
                return true;
            });
        if (prepared)
            DLL_Log("[FaceCamera] Optimized shader bytecode prepared outside the video-frame path");
    });
    return bytecode;
}

}  // namespace

FaceCameraRenderer::FaceCameraRenderer(ce::face_camera::Config config)
    : config_(std::move(config)), capture_(std::make_unique<FaceCameraCapture>(config_)) {
    (void)GetFaceCameraShaderBytecode();
    LARGE_INTEGER frequency = {};
    QueryPerformanceFrequency(&frequency);
    qpcFrequency_ = frequency.QuadPart;
    DLL_Log(
        "[FaceCamera] Configuration: request=%ux%u auto=%d fps=%u position=%s shape=%s crop=%s "
        "width=%.2f%% margin=%.2f%% opacity=%.1f%% mirror=%d staleMs=%u",
        config_.requestedWidth, config_.requestedHeight,
        config_.requestedWidth == 0 && config_.requestedHeight == 0 ? 1 : 0, config_.requestedFps,
        ce::face_camera::PositionName(config_.position), ce::face_camera::ShapeName(config_.shape),
        ce::face_camera::CropName(config_.crop), config_.widthPercent, config_.marginPercent, config_.opacity * 100.0f,
        config_.mirror ? 1 : 0, config_.staleTimeoutMs);
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

    D3D11_SAMPLER_DESC samplerDesc{D3D11_FILTER_MIN_MAG_MIP_LINEAR,
                                   D3D11_TEXTURE_ADDRESS_CLAMP, D3D11_TEXTURE_ADDRESS_CLAMP,
                                   D3D11_TEXTURE_ADDRESS_CLAMP, 0.0f, 1, D3D11_COMPARISON_NEVER,
                                   {}, 0.0f, D3D11_FLOAT32_MAX};
    if (FAILED(device_->CreateSamplerState(&samplerDesc, &sampler_))) {
        CleanupRendererResources();
        return false;
    }

    const D3D11_RENDER_TARGET_BLEND_DESC targetBlend{
        TRUE, D3D11_BLEND_SRC_ALPHA, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD,
        D3D11_BLEND_ONE, D3D11_BLEND_INV_SRC_ALPHA, D3D11_BLEND_OP_ADD, D3D11_COLOR_WRITE_ENABLE_ALL};
    // Only the first target is used when independent blending is disabled.
    D3D11_BLEND_DESC blendDesc{FALSE, FALSE, {targetBlend, targetBlend, targetBlend, targetBlend,
                                               targetBlend, targetBlend, targetBlend, targetBlend}};
    if (FAILED(device_->CreateBlendState(&blendDesc, &blendState_))) {
        CleanupRendererResources();
        return false;
    }

    D3D11_RASTERIZER_DESC rasterizerDesc{D3D11_FILL_SOLID, D3D11_CULL_NONE, FALSE, 0, 0.0f,
                                         0.0f, TRUE, FALSE, FALSE, FALSE};
    if (FAILED(device_->CreateRasterizerState(&rasterizerDesc, &rasterizerState_))) {
        CleanupRendererResources();
        return false;
    }
    resourcesCreated_ = true;
    DLL_Log("[FaceCamera] GPU renderer ready: analytic shape/border, trilinear scaling, one draw per output frame");
    return true;
}

void FaceCameraRenderer::CameraSlot::Release() {
    if (chromaSrv)
        chromaSrv->Release();
    if (chromaTexture)
        chromaTexture->Release();
    if (srv)
        srv->Release();
    if (texture)
        texture->Release();
    *this = CameraSlot{};
}

uint64_t FaceCameraRenderer::GpuBytes() const {
    uint64_t bytes = 0;
    for (const auto& slot : history_) {
        if (!slot.texture)
            continue;
        const uint64_t base =
            ce::gpu_memory::TextureBytes(slot.texture) + ce::gpu_memory::TextureBytes(slot.chromaTexture);
        bytes += slot.mipLevels > 1 ? base * 4 / 3 : base;
    }
    return bytes;
}

void FaceCameraRenderer::ReleaseHistory() {
    for (auto& slot : history_)
        slot.Release();
    history_.clear();
    historyNext_ = 0;
    cameraTexture_ = nullptr;
    cameraSrv_ = nullptr;
    cameraChromaSrv_ = nullptr;
    cameraWidth_ = 0;
    cameraHeight_ = 0;
    cameraTextureWidth_ = 0;
    cameraTextureHeight_ = 0;
    cameraPixelFormat_ = ce::face_camera::PixelFormat::kBgra8;
    cameraYuvMatrix_ = ce::face_camera::YuvMatrix::kBt709;
    cameraYuvRange_ = ce::face_camera::YuvRange::kLimited;
}

bool FaceCameraRenderer::EnsureSlotTexture(CameraSlot& slot, uint32_t width, uint32_t height, DXGI_FORMAT format) {
    return EnsureSlotGpuTexture(slot, width, height, width, height, format);
}

bool FaceCameraRenderer::EnsureSlotGpuTexture(CameraSlot& slot, uint32_t activeWidth, uint32_t activeHeight,
                                              uint32_t textureWidth, uint32_t textureHeight, DXGI_FORMAT format) {
    format = TypedCameraFormat(format);
    ce::face_camera::PixelFormat pixelFormat = ce::face_camera::PixelFormat::kBgra8;
    if (!ce::face_camera::DxgiToPixelFormat(format, &pixelFormat))
        return false;
    if (slot.texture && !slot.chromaTexture && slot.width == activeWidth && slot.height == activeHeight &&
        slot.textureWidth == textureWidth && slot.textureHeight == textureHeight && slot.format == format &&
        slot.pixelFormat == pixelFormat) {
        return true;
    }
    if (cameraTexture_ == slot.texture)
        cameraTexture_ = nullptr, cameraSrv_ = nullptr, cameraChromaSrv_ = nullptr;
    slot.Release();

    UINT formatSupport = 0;
    const bool supportsMipGeneration =
        pixelFormat == ce::face_camera::PixelFormat::kBgra8 &&
        SUCCEEDED(device_->CheckFormatSupport(format, &formatSupport)) &&
        (formatSupport & D3D11_FORMAT_SUPPORT_MIP_AUTOGEN) != 0 &&
        (formatSupport & D3D11_FORMAT_SUPPORT_RENDER_TARGET) != 0;
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = textureWidth;
    desc.Height = textureHeight;
    desc.MipLevels = supportsMipGeneration ? 0 : 1;
    desc.ArraySize = 1;
    desc.Format = format;
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE | (supportsMipGeneration ? D3D11_BIND_RENDER_TARGET : 0);
    desc.MiscFlags = supportsMipGeneration ? D3D11_RESOURCE_MISC_GENERATE_MIPS : 0;
    if (FAILED(device_->CreateTexture2D(&desc, nullptr, &slot.texture))) {
        slot.Release();
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC yuy2Desc = {};
    yuy2Desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    yuy2Desc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    yuy2Desc.Texture2D.MipLevels = 1;
    const bool srvOk =
        pixelFormat == ce::face_camera::PixelFormat::kNv12
            ? CreateNv12PlaneViews(device_, slot.texture, &slot.srv, &slot.chromaSrv)
            : SUCCEEDED(device_->CreateShaderResourceView(
                  slot.texture, pixelFormat == ce::face_camera::PixelFormat::kYuy2 ? &yuy2Desc : nullptr, &slot.srv));
    if (!srvOk) {
        slot.Release();
        return false;
    }

    slot.width = activeWidth;
    slot.height = activeHeight;
    slot.textureWidth = textureWidth;
    slot.textureHeight = textureHeight;
    slot.format = format;
    slot.pixelFormat = pixelFormat;
    D3D11_TEXTURE2D_DESC created = {};
    slot.texture->GetDesc(&created);
    slot.mipLevels = created.MipLevels;
    if (!historyAllocationLogged_ || history_.size() != loggedHistoryFrames_) {
        historyAllocationLogged_ = true;
        loggedHistoryFrames_ = history_.size();
        DLL_Log("[FaceCamera] GPU camera history: %zu frame(s) of %ux%u fmt=%d mips=%u", history_.size(), activeWidth,
                activeHeight, format, created.MipLevels);
    }
    return true;
}

bool FaceCameraRenderer::EnsureSlotCpuTextures(CameraSlot& slot, ce::face_camera::PixelFormat pixelFormat,
                                               uint32_t width, uint32_t height) {
    if (!ce::face_camera::DimensionsValidForFormat(pixelFormat, width, height))
        return false;
    if (pixelFormat == ce::face_camera::PixelFormat::kBgra8)
        return EnsureSlotTexture(slot, width, height, DXGI_FORMAT_B8G8R8A8_UNORM);
    const bool wantNv12 = pixelFormat == ce::face_camera::PixelFormat::kNv12;
    const DXGI_FORMAT primaryFormat = wantNv12 ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    if (slot.texture && (slot.chromaTexture != nullptr) == wantNv12 && slot.width == width && slot.height == height &&
        slot.format == primaryFormat && slot.pixelFormat == pixelFormat) {
        return true;
    }
    if (cameraTexture_ == slot.texture)
        cameraTexture_ = nullptr, cameraSrv_ = nullptr, cameraChromaSrv_ = nullptr;
    slot.Release();
    const uint32_t primaryWidth = wantNv12 ? width : width / 2u;
    if (!CreateSrvTexture2D(device_, primaryWidth, height, primaryFormat, &slot.texture, &slot.srv) ||
        (wantNv12 && !CreateSrvTexture2D(device_, width / 2u, height / 2u, DXGI_FORMAT_R8G8_UNORM, &slot.chromaTexture,
                                         &slot.chromaSrv))) {
        slot.Release();
        return false;
    }
    slot.width = slot.textureWidth = width;
    slot.height = slot.textureHeight = height;
    slot.format = primaryFormat;
    slot.pixelFormat = pixelFormat;
    slot.mipLevels = 1;
    return true;
}

bool FaceCameraRenderer::CopyGpuFrame(const FaceCameraFrame& frame, CameraSlot& slot) {
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
    ce::face_camera::PixelFormat pixelFormat = frame.format;
    if (!ce::face_camera::DxgiToPixelFormat(desc.Format, &pixelFormat) || desc.Width < frame.width ||
        desc.Height < frame.height) {
        return false;
    }
    const bool wholeSubresourceCopy = pixelFormat != ce::face_camera::PixelFormat::kBgra8;
    const uint32_t texWidth = wholeSubresourceCopy ? desc.Width : frame.width;
    const uint32_t texHeight = wholeSubresourceCopy ? desc.Height : frame.height;
    if (!EnsureSlotGpuTexture(slot, frame.width, frame.height, texWidth, texHeight, desc.Format))
        return false;
    const D3D11_BOX sourceBox = {0, 0, 0, frame.width, frame.height, 1};
    context_->CopySubresourceRegion(slot.texture, 0, 0, 0, 0, source.get(), frame.gpuSubresource,
                                    wholeSubresourceCopy ? nullptr : &sourceBox);
    slot.matrix = frame.matrix;
    slot.range = frame.range;
    return true;
}

bool FaceCameraRenderer::UploadCpuFrame(const FaceCameraFrame& frame, CameraSlot& slot) {
    if (frame.bgra.empty() || !ce::face_camera::DimensionsValidForFormat(frame.format, frame.width, frame.height))
        return false;
    const uint32_t rowBytes = ce::face_camera::DefaultRowBytes(frame.format, frame.width);
    if (frame.bgra.size() < static_cast<size_t>(rowBytes) * frame.height)
        return false;
    if (frame.format == ce::face_camera::PixelFormat::kNv12 &&
        frame.chroma.size() < static_cast<size_t>(frame.width) * (frame.height / 2u)) {
        return false;
    }
    if (!EnsureSlotCpuTextures(slot, frame.format, frame.width, frame.height))
        return false;
    context_->UpdateSubresource(slot.texture, 0, nullptr, frame.bgra.data(), rowBytes, 0);
    if (frame.format == ce::face_camera::PixelFormat::kNv12)
        context_->UpdateSubresource(slot.chromaTexture, 0, nullptr, frame.chroma.data(), frame.width, 0);
    if (!historyAllocationLogged_ || history_.size() != loggedHistoryFrames_) {
        historyAllocationLogged_ = true;
        loggedHistoryFrames_ = history_.size();
        DLL_Log("[FaceCamera] GPU camera history: %zu frame(s) of %ux%u fmt=%s mips=%u", history_.size(), slot.width,
                slot.height, ce::face_camera::DescribePixelFormat(slot.pixelFormat), slot.mipLevels);
    }
    slot.matrix = frame.matrix;
    slot.range = frame.range;
    return true;
}

void FaceCameraRenderer::UploadLatestFrame() {
    const auto frame = capture_->LatestFrame();
    if (!frame || frame->sequence == uploadedSequence_)
        return;
    if (history_.empty())
        history_.resize(ce::face_camera::kHistoryInitialFrames);
    // Every camera frame gets its own history slot (one upload per camera
    // frame, as before); the oldest slot is reused.
    CameraSlot& slot = history_[historyNext_ % history_.size()];
    bool updated = frame->gpuSample ? CopyGpuFrame(*frame, slot) : false;
    if (!updated)
        updated = UploadCpuFrame(*frame, slot);
    uploadedSequence_ = frame->sequence;
    if (!updated) {
        // Do not retry a failed immutable sample at output cadence. A later
        // camera sequence gets a fresh attempt while the retained history, if
        // any, remains available until its stale deadline.
        if (!transportFailureLogged_) {
            DLL_Log("[FaceCamera] Failed to transfer a camera frame to the encoder GPU; retaining prior frames");
            transportFailureLogged_ = true;
        }
        return;
    }
    if (slot.mipLevels > 1)
        context_->GenerateMips(slot.srv);
    if (lastCaptureQpc_ > 0 && frame->captureQpc > lastCaptureQpc_)
        cameraIntervalQpc_ = frame->captureQpc - lastCaptureQpc_;
    lastCaptureQpc_ = frame->captureQpc;
    slot.sequence = frame->sequence;
    slot.captureQpc = frame->captureQpc > 0 ? frame->captureQpc : 1;
    historyNext_ = (historyNext_ + 1) % history_.size();
    uploadedTickMs_ = frame->receivedTickMs;
    ++cameraUploadCount_;
    staleLogged_ = false;
}

bool FaceCameraRenderer::UpdateCameraTexture() {
    if (!capture_ || !resourcesCreated_)
        return false;
    UploadLatestFrame();
    std::vector<int64_t> captureQpc(history_.size(), 0);
    for (size_t i = 0; i < history_.size(); ++i)
        captureQpc[i] = history_[i].texture ? history_[i].captureQpc : 0;
    const auto selection =
        ce::face_camera::SelectHistoryFrame(captureQpc, contentTargetQpc_, cameraIntervalQpc_);
    if (selection.index == SIZE_MAX) {
        cameraTexture_ = nullptr;
        cameraSrv_ = nullptr;
        cameraChromaSrv_ = nullptr;
        return false;
    }
    // Growing the history below can relocate every slot. Keep the selected
    // frame's fields by value for both publication and lag accounting.
    const CameraSlot selected = history_[selection.index];
    cameraTexture_ = selected.texture;
    cameraSrv_ = selected.srv;
    cameraChromaSrv_ = selected.chromaSrv;
    cameraWidth_ = selected.width;
    cameraHeight_ = selected.height;
    cameraTextureWidth_ = selected.textureWidth > 0 ? selected.textureWidth : selected.width;
    cameraTextureHeight_ = selected.textureHeight > 0 ? selected.textureHeight : selected.height;
    cameraPixelFormat_ = selected.pixelFormat;
    cameraYuvMatrix_ = selected.matrix;
    cameraYuvRange_ = selected.range;
    if (selection.targetOlderThanHistory && selection.wantedFrames > history_.size()) {
        // The content delay exceeds the history: grow it; new slots fill first
        // (selection orders by capture time, not by slot index).
        historyNext_ = history_.size();
        history_.resize(selection.wantedFrames);
        DLL_Log("[FaceCamera] Camera history grown to %zu frames for a content lag of %.1f ms", history_.size(),
                qpcFrequency_ > 0 ? 1000.0 * static_cast<double>(lastCaptureQpc_ - contentTargetQpc_) /
                                        static_cast<double>(qpcFrequency_)
                                  : 0.0);
    }
    if (contentTargetQpc_ > 0 && qpcFrequency_ > 0) {
        const int64_t lagQpc = contentTargetQpc_ - selected.captureQpc;
        lagSummary_.Add(lagQpc * 1000000 / qpcFrequency_, selection.targetOlderThanHistory);
    }
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
        ReleaseHistory();
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

void FaceCameraRenderer::LogTargetViewRefused(const D3D11_TEXTURE2D_DESC& targetDesc, HRESULT hr) {
    // Every frame of that target loses the camera, so the line repeats with a count.
    const auto log = targetViewRefusedLog_.Observe(
        ce::log_meter::FieldKey(static_cast<uint32_t>(targetDesc.Format), static_cast<uint32_t>(hr)), GetTickCount64());
    if (log) {
        DLL_Log("[FaceCamera] No render target view for targetFmt=%d %ux%u HR=%x: camera not drawn%s",
                static_cast<int>(targetDesc.Format), targetDesc.Width, targetDesc.Height, static_cast<unsigned>(hr),
                ce::log_meter::SuppressedNote(log.suppressed).c_str());
    }
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
    viewDesc.Format = ce::video_format::GetOverlayRenderTargetViewFormat(targetDesc.Format);
    if (viewDesc.Format == DXGI_FORMAT_UNKNOWN) {
        LogTargetViewRefused(targetDesc, S_OK);
        return nullptr;
    }
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
    if (!replacement)
        return nullptr;
    const HRESULT hr = device_->CreateRenderTargetView(targetTexture, &viewDesc, &view);
    if (FAILED(hr)) {
        LogTargetViewRefused(targetDesc, hr);
        return nullptr;
    }
    if (replacement->view)
        replacement->view->Release();
    replacement->texture = targetTexture;
    replacement->view = view;
    replacement->lastUsed = targetRtvUseCounter_;
    return view;
}

bool FaceCameraRenderer::CompositePreparedFrame(ID3D11Texture2D* targetTexture, uint32_t targetWidth,
                                                uint32_t targetHeight, FaceCameraColorMode colorMode,
                                                float paperWhiteNits, int viewportOriginX, int viewportOriginY) {
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
    const float texWidth = static_cast<float>(cameraTextureWidth_ > 0 ? cameraTextureWidth_ : cameraWidth_);
    const float texHeight = static_cast<float>(cameraTextureHeight_ > 0 ? cameraTextureHeight_ : cameraHeight_);
    const float safeLumaWidth = std::max(texWidth, 2.0f);
    *constants = Constants{
        {preparedLayout_.normalizedLeft, preparedLayout_.normalizedTop, preparedLayout_.normalizedWidth,
         preparedLayout_.normalizedHeight},
        {preparedLayout_.sourceU0, preparedLayout_.sourceV0, preparedLayout_.sourceU1, preparedLayout_.sourceV1},
        {static_cast<float>(config_.shape), std::clamp(config_.cornerRadiusPercent, 0.0f, 50.0f) / 100.0f,
         std::clamp(config_.borderWidthPercent, 0.0f, 10.0f) / 100.0f, std::clamp(config_.opacity, 0.0f, 1.0f)},
        {static_cast<float>(colorMode), resolvedPaperWhiteNits, config_.mirror ? 1.0f : 0.0f, 0.0f},
        {static_cast<float>((config_.borderColorRgb >> 16) & 0xFFu) / 255.0f,
         static_cast<float>((config_.borderColorRgb >> 8) & 0xFFu) / 255.0f,
         static_cast<float>(config_.borderColorRgb & 0xFFu) / 255.0f, 1.0f},
        {preparedLayout_.displayedWidthPixels, preparedLayout_.displayedHeightPixels, 0.0f, 0.0f},
        {static_cast<float>(cameraPixelFormat_), static_cast<float>(cameraYuvMatrix_),
         static_cast<float>(cameraYuvRange_), 0.0f},
        {texWidth > 0.0f ? static_cast<float>(cameraWidth_) / texWidth : 1.0f,
         texHeight > 0.0f ? static_cast<float>(cameraHeight_) / texHeight : 1.0f, safeLumaWidth,
         2.0f / safeLumaWidth},
    };
    context_->Unmap(constantBuffer_, 0);

    ID3D11RenderTargetView* oldView = nullptr;
    ID3D11DepthStencilView* oldDepth = nullptr;
    D3D11_VIEWPORT oldViewport = {};
    UINT viewportCount = 1;
    context_->OMGetRenderTargets(1, &oldView, &oldDepth);
    context_->RSGetViewports(&viewportCount, &oldViewport);

    D3D11_VIEWPORT viewport = {};
    viewport.TopLeftX = -static_cast<float>(viewportOriginX);
    viewport.TopLeftY = -static_cast<float>(viewportOriginY);
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
    ID3D11ShaderResourceView* const shaderViews[2] = {cameraSrv_, cameraChromaSrv_};
    context_->PSSetShaderResources(0, 2, shaderViews);
    context_->PSSetSamplers(0, 1, &sampler_);
    context_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP);
    context_->Draw(4, 0);

    ID3D11ShaderResourceView* const nullViews[2] = {nullptr, nullptr};
    context_->PSSetShaderResources(0, 2, nullViews);
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
            "camera=%ux%u staleMs=%llu history=%zu slotLagUs=avg %lld min %lld max %lld timed=%llu "
            "olderThanHistory=%llu",
            static_cast<unsigned long long>(compositeCount_), static_cast<unsigned long long>(cameraUploadCount_),
            targetWidth, targetHeight, preparedLayout_.right - preparedLayout_.left,
            preparedLayout_.bottom - preparedLayout_.top, cameraWidth_, cameraHeight_,
            static_cast<unsigned long long>(uploadedTickMs_ ? GetTickCount64() - uploadedTickMs_ : 0),
            history_.size(), static_cast<long long>(lagSummary_.AverageUs()), static_cast<long long>(lagSummary_.minUs),
            static_cast<long long>(lagSummary_.maxUs), static_cast<unsigned long long>(lagSummary_.count),
            static_cast<unsigned long long>(lagSummary_.olderThanHistory));
        lagSummary_ = {};
    }
    return true;
}

void FaceCameraRenderer::CleanupRendererResources() {
    ClearTargetRenderViewCache();
    ReleaseHistory();
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
    vertexShader_ = nullptr;
    pixelShader_ = nullptr;
    constantBuffer_ = nullptr;
    sampler_ = nullptr;
    blendState_ = nullptr;
    rasterizerState_ = nullptr;
    uploadedSequence_ = 0;
    uploadedTickMs_ = 0;
    lastCaptureQpc_ = 0;
    cameraIntervalQpc_ = 0;
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
