#include "frame_scaler.h"

#include <cstring>
#include <vector>

#include "frame_scaler_shader.h"
#include "gpu_memory_breakdown.h"
#include "mediaengine/engine/mediaengine.h"
#include "runtime_shader_compiler.h"

namespace ce::video_scale {

namespace {

struct ScaleConstants {
    uint32_t tapGroups;
    int32_t sourceLast;
    uint32_t padding[2];
};
static_assert(sizeof(ScaleConstants) == 16);

DXGI_FORMAT SurfaceFormat(IntermediatePrecision precision) {
    return precision == IntermediatePrecision::kUnorm10 ? DXGI_FORMAT_R10G10B10A2_UNORM
                                                        : DXGI_FORMAT_R16G16B16A16_UNORM;
}

const char* DescribePrecision(IntermediatePrecision precision) {
    return precision == IntermediatePrecision::kUnorm10 ? "RGB10A2" : "RGBA16";
}

enum FailureSite : uint64_t {
    kShaderFailure = 1,
    kTableFailure,
    kConstantsFailure,
    kSurfaceFailure,
    kRequestRefused,
};

}  // namespace

bool FrameScaler::PlanKey::operator==(const PlanKey& other) const {
    return sourceWidth == other.sourceWidth && sourceHeight == other.sourceHeight &&
           outputWidth == other.outputWidth && outputHeight == other.outputHeight && lobes == other.lobes &&
           sideLobeGain == other.sideLobeGain && precision == other.precision && ownOutput == other.ownOutput;
}

void FrameScaler::LogFailure(uint64_t site, const char* what, long hr) {
    const auto log = failureLog_.Observe(ce::log_meter::FieldKey(site, hr), GetTickCount64());
    if (log) {
        DLL_Log("[Scaler] %s failed: HR=%x%s", what, static_cast<unsigned>(hr),
                ce::log_meter::SuppressedNote(log.suppressed).c_str());
    }
}

bool FrameScaler::EnsureShaders(ID3D11Device* device) {
    if (!device)
        return false;
    if (owner_ != device)
        Reset();
    if (vertexShader_ && horizontalShader_ && verticalShader_)
        return true;
    const ce::video_color::RuntimeShaderEntry entries[] = {
        {"VS_Main", "vs_4_0"}, {"PS_ScaleH", "ps_4_0"}, {"PS_ScaleV", "ps_4_0"}};
    HRESULT hr = S_OK;
    ce::ComGuard<ID3D11VertexShader> vertexShader;
    ce::ComGuard<ID3D11PixelShader> horizontalShader;
    ce::ComGuard<ID3D11PixelShader> verticalShader;
    const bool created = ce::video_color::PrepareShaderBytecode(
        kFrameScalerShaderSource, std::strlen(kFrameScalerShaderSource), entries, 3, "Scaler",
        [&](const ce::video_color::ShaderBytecodeView* shaders) {
            hr = device->CreateVertexShader(shaders[0].data, shaders[0].size, nullptr, vertexShader.addressof());
            if (SUCCEEDED(hr))
                hr = device->CreatePixelShader(shaders[1].data, shaders[1].size, nullptr,
                                               horizontalShader.addressof());
            if (SUCCEEDED(hr))
                hr = device->CreatePixelShader(shaders[2].data, shaders[2].size, nullptr, verticalShader.addressof());
            return SUCCEEDED(hr);
        });
    if (!created) {
        LogFailure(kShaderFailure, "Shader preparation", hr);
        return false;
    }
    owner_ = device;
    vertexShader_ = std::move(vertexShader);
    horizontalShader_ = std::move(horizontalShader);
    verticalShader_ = std::move(verticalShader);
    return true;
}

bool FrameScaler::CreateAxis(ID3D11Device* device, const AxisTable& table, AxisResources* axis) {
    const std::vector<float> packed = PackAxisTable(table);
    const UINT elements = static_cast<UINT>(packed.size() / 4u);
    D3D11_BUFFER_DESC tableDesc = {};
    tableDesc.ByteWidth = static_cast<UINT>(packed.size() * sizeof(float));
    tableDesc.Usage = D3D11_USAGE_IMMUTABLE;
    tableDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA tableData = {packed.data(), 0, 0};
    HRESULT hr = device->CreateBuffer(&tableDesc, &tableData, axis->table.addressof());
    if (SUCCEEDED(hr)) {
        D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
        viewDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
        viewDesc.ViewDimension = D3D11_SRV_DIMENSION_BUFFER;
        viewDesc.Buffer.FirstElement = 0;
        viewDesc.Buffer.NumElements = elements;
        hr = device->CreateShaderResourceView(axis->table.get(), &viewDesc, axis->tableView.addressof());
    }
    if (FAILED(hr)) {
        LogFailure(kTableFailure, "Weight table creation", hr);
        return false;
    }

    const ScaleConstants constants = {table.taps / 4u, static_cast<int32_t>(table.inSize) - 1, {0u, 0u}};
    D3D11_BUFFER_DESC constantsDesc = {};
    constantsDesc.ByteWidth = sizeof(constants);
    constantsDesc.Usage = D3D11_USAGE_IMMUTABLE;
    constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA constantsData = {&constants, 0, 0};
    hr = device->CreateBuffer(&constantsDesc, &constantsData, axis->constants.addressof());
    if (FAILED(hr)) {
        LogFailure(kConstantsFailure, "Constant buffer creation", hr);
        return false;
    }
    return true;
}

bool FrameScaler::CreateSurface(ID3D11Device* device, uint32_t width, uint32_t height, Surface* surface) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = 1;
    desc.ArraySize = 1;
    desc.Format = SurfaceFormat(plan_.precision);
    desc.SampleDesc.Count = 1;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    HRESULT hr = device->CreateTexture2D(&desc, nullptr, surface->texture.addressof());
    if (SUCCEEDED(hr))
        hr = device->CreateRenderTargetView(surface->texture.get(), nullptr, surface->target.addressof());
    if (SUCCEEDED(hr))
        hr = device->CreateShaderResourceView(surface->texture.get(), nullptr, surface->view.addressof());
    if (FAILED(hr)) {
        LogFailure(kSurfaceFailure, "Surface creation", hr);
        return false;
    }
    return true;
}

bool FrameScaler::EnsurePlan(ID3D11Device* device, const Request& request) {
    PlanKey key;
    key.sourceWidth = request.sourceWidth;
    key.sourceHeight = request.sourceHeight;
    key.outputWidth = request.outputWidth;
    key.outputHeight = request.outputHeight;
    key.lobes = request.kernel.lobes;
    key.sideLobeGain = request.kernel.sideLobeGain;
    key.ownOutput = request.target == nullptr;
    const ScalePlan plan = PlanScale(request.sourceWidth, request.sourceHeight, request.outputWidth,
                                     request.outputHeight, request.kernel, request.sourceBits, request.outputBits);
    key.precision = plan.precision;
    if (planReady_ && key == key_)
        return true;

    planReady_ = false;
    horizontal_ = AxisResources{};
    vertical_ = AxisResources{};
    intermediate_ = Surface{};
    output_ = Surface{};
    plan_ = plan;
    if ((plan_.scaleX && !CreateAxis(device, plan_.horizontal, &horizontal_)) ||
        (plan_.scaleY && !CreateAxis(device, plan_.vertical, &vertical_)))
        return false;
    if (plan_.scaleX && plan_.scaleY &&
        !CreateSurface(device, plan_.intermediateWidth, plan_.intermediateHeight, &intermediate_))
        return false;
    if (key.ownOutput && !CreateSurface(device, request.outputWidth, request.outputHeight, &output_))
        return false;
    key_ = key;
    planReady_ = true;

    const bool twoPasses = plan_.scaleX && plan_.scaleY;
    DLL_Log("[Scaler] Plan: %ux%u -> %ux%u kernel=%s sideLobes=%.2f antiring=1 taps=%ux%u passes=%d order=%s "
            "intermediate=%ux%u %s output=%s sourceReads/frame=%llu",
            request.sourceWidth, request.sourceHeight, request.outputWidth, request.outputHeight,
            DescribeKernel(request.kernel), static_cast<double>(request.kernel.sideLobeGain), plan_.horizontal.taps,
            plan_.vertical.taps, twoPasses ? 2 : 1,
            !twoPasses ? "single" : (plan_.order == PassOrder::kHorizontalFirst ? "h-first" : "v-first"),
            twoPasses ? plan_.intermediateWidth : 0u, twoPasses ? plan_.intermediateHeight : 0u,
            DescribePrecision(plan_.precision), key.ownOutput ? DescribePrecision(plan_.precision) : "caller-target",
            static_cast<unsigned long long>(plan_.estimatedTaps));
    return true;
}

void FrameScaler::DrawPass(ID3D11DeviceContext* context, bool horizontal, ID3D11ShaderResourceView* source,
                           ID3D11RenderTargetView* target, uint32_t width, uint32_t height) {
    AxisResources& axis = horizontal ? horizontal_ : vertical_;
    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(width);
    viewport.Height = static_cast<float>(height);
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &target, nullptr);
    context->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFFu);
    ID3D11ShaderResourceView* views[2] = {source, axis.tableView.get()};
    context->PSSetShaderResources(0, 2, views);
    ID3D11Buffer* constants = axis.constants.get();
    context->PSSetConstantBuffers(0, 1, &constants);
    context->PSSetShader(horizontal ? horizontalShader_.get() : verticalShader_.get(), nullptr, 0);
    context->Draw(3, 0);
    // The target of this pass is the next pass's source.
    ID3D11RenderTargetView* nullTarget = nullptr;
    context->OMSetRenderTargets(1, &nullTarget, nullptr);
    ID3D11ShaderResourceView* nullViews[2] = {nullptr, nullptr};
    context->PSSetShaderResources(0, 2, nullViews);
}

bool FrameScaler::Scale(ID3D11Device* device, ID3D11DeviceContext* context, const Request& request,
                        ID3D11ShaderResourceView** scaled) {
    if (scaled)
        *scaled = nullptr;
    const bool sameSize =
        request.sourceWidth == request.outputWidth && request.sourceHeight == request.outputHeight;
    if (!device || !context || !request.source || request.sourceWidth == 0 || request.sourceHeight == 0 ||
        request.outputWidth == 0 || request.outputHeight == 0 || sameSize || (!request.target && !scaled)) {
        LogFailure(kRequestRefused, "Request validation", E_INVALIDARG);
        return false;
    }
    if (!EnsureShaders(device) || !EnsurePlan(device, request))
        return false;

    ID3D11RenderTargetView* finalTarget = request.target ? request.target : output_.target.get();
    context->VSSetShader(vertexShader_.get(), nullptr, 0);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetInputLayout(nullptr);
    if (plan_.scaleX && plan_.scaleY) {
        const bool horizontalFirst = plan_.order == PassOrder::kHorizontalFirst;
        DrawPass(context, horizontalFirst, request.source, intermediate_.target.get(), plan_.intermediateWidth,
                 plan_.intermediateHeight);
        DrawPass(context, !horizontalFirst, intermediate_.view.get(), finalTarget, request.outputWidth,
                 request.outputHeight);
    } else {
        DrawPass(context, plan_.scaleX, request.source, finalTarget, request.outputWidth, request.outputHeight);
    }
    if (scaled && !request.target)
        *scaled = output_.view.get();
    return true;
}

void FrameScaler::Reset() {
    owner_ = nullptr;
    vertexShader_.reset();
    horizontalShader_.reset();
    verticalShader_.reset();
    planReady_ = false;
    key_ = PlanKey{};
    plan_ = ScalePlan{};
    horizontal_ = AxisResources{};
    vertical_ = AxisResources{};
    intermediate_ = Surface{};
    output_ = Surface{};
}

uint64_t FrameScaler::GpuBytes() const {
    return ce::gpu_memory::TextureBytes(intermediate_.texture.get()) +
           ce::gpu_memory::TextureBytes(output_.texture.get());
}

}  // namespace ce::video_scale
