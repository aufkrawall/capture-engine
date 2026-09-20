#include "sharpen_d3d11.h"

#include <cstring>

#include "sharpen_constants.h"
#include "sharpen_shader_bytecode.h"

using Microsoft::WRL::ComPtr;

namespace ce::sharpen {
namespace {

// Every render target view the pass can legally write through.
constexpr UINT kMaxSavedRTVs = 8;

struct SavedPipelineState {
    ComPtr<ID3D11RasterizerState> rasterizer;
    ComPtr<ID3D11BlendState> blend;
    float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    UINT sampleMask = 0xFFFFFFFFu;
    ComPtr<ID3D11DepthStencilState> depth;
    UINT stencilRef = 0;
    ID3D11RenderTargetView* renderTargets[kMaxSavedRTVs] = {};
    ComPtr<ID3D11DepthStencilView> depthView;
    ComPtr<ID3D11VertexShader> vertexShader;
    ComPtr<ID3D11PixelShader> pixelShader;
    ComPtr<ID3D11GeometryShader> geometryShader;
    ComPtr<ID3D11HullShader> hullShader;
    ComPtr<ID3D11DomainShader> domainShader;
    ComPtr<ID3D11InputLayout> inputLayout;
    D3D11_PRIMITIVE_TOPOLOGY topology = D3D11_PRIMITIVE_TOPOLOGY_UNDEFINED;
    ComPtr<ID3D11Buffer> vertexBuffer;
    UINT vertexStride = 0;
    UINT vertexOffset = 0;
    ComPtr<ID3D11Buffer> indexBuffer;
    DXGI_FORMAT indexFormat = DXGI_FORMAT_UNKNOWN;
    UINT indexOffset = 0;
    ComPtr<ID3D11Buffer> pixelConstantBuffer;
    ComPtr<ID3D11ShaderResourceView> pixelResource;
    UINT viewportCount = 0;
    D3D11_VIEWPORT viewports[D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE] = {};

    ~SavedPipelineState() {
        for (ID3D11RenderTargetView* view : renderTargets) {
            if (view)
                view->Release();
        }
    }
};

void SavePipelineState(ID3D11DeviceContext* context, SavedPipelineState& saved) {
    context->RSGetState(&saved.rasterizer);
    context->OMGetBlendState(&saved.blend, saved.blendFactor, &saved.sampleMask);
    context->OMGetDepthStencilState(&saved.depth, &saved.stencilRef);
    context->OMGetRenderTargets(kMaxSavedRTVs, saved.renderTargets, &saved.depthView);
    context->VSGetShader(&saved.vertexShader, nullptr, nullptr);
    context->PSGetShader(&saved.pixelShader, nullptr, nullptr);
    context->GSGetShader(&saved.geometryShader, nullptr, nullptr);
    context->HSGetShader(&saved.hullShader, nullptr, nullptr);
    context->DSGetShader(&saved.domainShader, nullptr, nullptr);
    context->IAGetInputLayout(&saved.inputLayout);
    context->IAGetPrimitiveTopology(&saved.topology);
    context->IAGetVertexBuffers(0, 1, &saved.vertexBuffer, &saved.vertexStride, &saved.vertexOffset);
    context->IAGetIndexBuffer(&saved.indexBuffer, &saved.indexFormat, &saved.indexOffset);
    context->PSGetConstantBuffers(0, 1, &saved.pixelConstantBuffer);
    context->PSGetShaderResources(0, 1, &saved.pixelResource);
    saved.viewportCount = D3D11_VIEWPORT_AND_SCISSORRECT_OBJECT_COUNT_PER_PIPELINE;
    context->RSGetViewports(&saved.viewportCount, saved.viewports);
}

void RestorePipelineState(ID3D11DeviceContext* context, SavedPipelineState& saved) {
    // The source copy must stop being bound before the target it was copied
    // from can be a render target again in the game's own state.
    ID3D11ShaderResourceView* nullResource = nullptr;
    context->PSSetShaderResources(0, 1, &nullResource);

    context->RSSetState(saved.rasterizer.Get());
    context->OMSetBlendState(saved.blend.Get(), saved.blendFactor, saved.sampleMask);
    context->OMSetDepthStencilState(saved.depth.Get(), saved.stencilRef);
    context->OMSetRenderTargets(kMaxSavedRTVs, saved.renderTargets, saved.depthView.Get());
    context->VSSetShader(saved.vertexShader.Get(), nullptr, 0);
    context->PSSetShader(saved.pixelShader.Get(), nullptr, 0);
    context->GSSetShader(saved.geometryShader.Get(), nullptr, 0);
    context->HSSetShader(saved.hullShader.Get(), nullptr, 0);
    context->DSSetShader(saved.domainShader.Get(), nullptr, 0);
    context->IASetInputLayout(saved.inputLayout.Get());
    context->IASetPrimitiveTopology(saved.topology);
    ID3D11Buffer* vertexBuffer = saved.vertexBuffer.Get();
    context->IASetVertexBuffers(0, 1, &vertexBuffer, &saved.vertexStride, &saved.vertexOffset);
    context->IASetIndexBuffer(saved.indexBuffer.Get(), saved.indexFormat, saved.indexOffset);
    ID3D11Buffer* constantBuffer = saved.pixelConstantBuffer.Get();
    context->PSSetConstantBuffers(0, 1, &constantBuffer);
    ID3D11ShaderResourceView* resource = saved.pixelResource.Get();
    context->PSSetShaderResources(0, 1, &resource);
    if (saved.viewportCount > 0)
        context->RSSetViewports(saved.viewportCount, saved.viewports);
}

}  // namespace

bool FormatAppliesSrgbConversion(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            return true;
        default:
            return false;
    }
}

TargetEncoding ResolveDxgiEncoding(DXGI_FORMAT format, bool isHDR) {
    switch (format) {
        // FP16 is linear light whether or not the swapchain declares HDR.
        case DXGI_FORMAT_R16G16B16A16_FLOAT:
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return TargetEncoding::ScrgbLinear;
        // Storage format is not content metadata: the same 10-bit surface is
        // Rec.709 SDR or HDR10/PQ depending on the swapchain's color space.
        case DXGI_FORMAT_R10G10B10A2_UNORM:
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return isHDR ? TargetEncoding::Pq : TargetEncoding::Unorm;
        case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
        case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
            return TargetEncoding::Srgb;
        case DXGI_FORMAT_R8G8B8A8_UNORM:
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8A8_UNORM:
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
        case DXGI_FORMAT_B8G8R8X8_UNORM:
        case DXGI_FORMAT_B8G8R8X8_TYPELESS:
            return TargetEncoding::Unorm;
        default:
            // An unrecognized target is refused rather than guessed at.
            return TargetEncoding::Unknown;
    }
}

bool D3D11Pass::EnsurePipelineState(ID3D11Device* device) {
    if (rasterizerState_ && blendState_ && depthState_)
        return true;

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - the D3D descriptor is zero-initialized before its enum fields are assigned
    D3D11_RASTERIZER_DESC rasterizerDesc = {};
    rasterizerDesc.FillMode = D3D11_FILL_SOLID;
    rasterizerDesc.CullMode = D3D11_CULL_NONE;
    rasterizerDesc.DepthClipEnable = TRUE;
    // A scissor rect left over from the game must not clip the pass.
    rasterizerDesc.ScissorEnable = FALSE;
    rasterizerDesc.MultisampleEnable = FALSE;
    if (FAILED(device->CreateRasterizerState(&rasterizerDesc, &rasterizerState_)))
        return false;

    // The filter replaces the frame; it never blends with what is already there.
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - the D3D descriptor is zero-initialized before its enum fields are assigned
    D3D11_BLEND_DESC blendDesc = {};
    blendDesc.RenderTarget[0].BlendEnable = FALSE;
    blendDesc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    if (FAILED(device->CreateBlendState(&blendDesc, &blendState_)))
        return false;

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - the D3D descriptor is zero-initialized before its enum fields are assigned
    D3D11_DEPTH_STENCIL_DESC depthDesc = {};
    depthDesc.DepthEnable = FALSE;
    depthDesc.StencilEnable = FALSE;
    if (FAILED(device->CreateDepthStencilState(&depthDesc, &depthState_)))
        return false;

    return true;
}

bool D3D11Pass::EnsureDeviceObjects(ID3D11Device* device) {
    if (ownerDevice_ != device) {
        Shutdown();
        ownerDevice_ = device;
    }

    if (!vertexShader_ &&
        FAILED(device->CreateVertexShader(g_SharpenVS_5_0, sizeof(g_SharpenVS_5_0), nullptr, &vertexShader_))) {
        return false;
    }
    if (!casShader_ &&
        FAILED(device->CreatePixelShader(g_SharpenPS_Cas_5_0, sizeof(g_SharpenPS_Cas_5_0), nullptr, &casShader_))) {
        return false;
    }
    if (!rcasShader_ &&
        FAILED(device->CreatePixelShader(g_SharpenPS_Rcas_5_0, sizeof(g_SharpenPS_Rcas_5_0), nullptr, &rcasShader_))) {
        return false;
    }

    if (!constantBuffer_) {
        D3D11_BUFFER_DESC bufferDesc = {};
        bufferDesc.ByteWidth = sizeof(ShaderConstants);
        bufferDesc.Usage = D3D11_USAGE_DYNAMIC;
        bufferDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        bufferDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(device->CreateBuffer(&bufferDesc, nullptr, &constantBuffer_)))
            return false;
    }

    return EnsurePipelineState(device);
}

bool D3D11Pass::EnsureSourceCopy(ID3D11Device* device, const D3D11_TEXTURE2D_DESC& targetDesc,
                                 DXGI_FORMAT viewFormat) {
    const bool matches = sourceCopy_ && sourceView_ && copyWidth_ == targetDesc.Width &&
                         copyHeight_ == targetDesc.Height && copyFormat_ == targetDesc.Format &&
                         copyViewFormat_ == viewFormat;
    if (matches)
        return true;

    sourceView_.Reset();
    sourceCopy_.Reset();
    copyWidth_ = 0;
    copyHeight_ = 0;
    copyFormat_ = DXGI_FORMAT_UNKNOWN;
    copyViewFormat_ = DXGI_FORMAT_UNKNOWN;

    // CopyResource requires identical formats or two formats from the same
    // typeless group, so the copy keeps the target's own storage format and
    // only the view reinterprets it.
    D3D11_TEXTURE2D_DESC copyDesc = {};
    copyDesc.Width = targetDesc.Width;
    copyDesc.Height = targetDesc.Height;
    copyDesc.MipLevels = 1;
    copyDesc.ArraySize = 1;
    copyDesc.Format = targetDesc.Format;
    copyDesc.SampleDesc.Count = 1;
    copyDesc.Usage = D3D11_USAGE_DEFAULT;
    copyDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    if (FAILED(device->CreateTexture2D(&copyDesc, nullptr, &sourceCopy_))) {
        HookLogImportant("Sharpen: DX11 source copy %ux%u fmt=%d could not be created", targetDesc.Width,
                         targetDesc.Height, static_cast<int>(targetDesc.Format));
        return false;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC viewDesc = {};
    viewDesc.Format = viewFormat;
    viewDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    viewDesc.Texture2D.MipLevels = 1;
    if (FAILED(device->CreateShaderResourceView(sourceCopy_.Get(), &viewDesc, &sourceView_))) {
        HookLogImportant("Sharpen: DX11 source view fmt=%d could not be created", static_cast<int>(viewFormat));
        sourceCopy_.Reset();
        return false;
    }

    copyWidth_ = targetDesc.Width;
    copyHeight_ = targetDesc.Height;
    copyFormat_ = targetDesc.Format;
    copyViewFormat_ = viewFormat;
    HookLogImportant("Sharpen: DX11 source copy ready %ux%u storage=%d view=%d", copyWidth_, copyHeight_,
                     static_cast<int>(copyFormat_), static_cast<int>(copyViewFormat_));
    return true;
}

bool D3D11Pass::Render(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11RenderTargetView* targetView,
                       const Request& request, Route route, TargetEncoding encoding) {
    if (HookIsShuttingDown() || !device || !context)
        return false;

    // The cheapest refusal first: an off switch must not cost a resource query.
    if (request.mode == Mode::Off) {
        if (logGate_.ShouldLog(false, "disabled"))
            HookLog("Sharpen: DX11 pass idle (disabled)");
        return false;
    }
    if (!targetView) {
        if (logGate_.ShouldLog(false, "target_not_writable"))
            HookLogImportant("Sharpen: DX11 pass skipped - no render target view for this present");
        return false;
    }

    ComPtr<ID3D11Resource> targetResource;
    targetView->GetResource(&targetResource);
    ComPtr<ID3D11Texture2D> targetTexture;
    if (!targetResource || FAILED(targetResource.As(&targetTexture)) || !targetTexture) {
        if (logGate_.ShouldLog(false, "frame_not_readable"))
            HookLogImportant("Sharpen: DX11 pass skipped - render target is not a 2D texture");
        return false;
    }

    D3D11_TEXTURE2D_DESC targetDesc = {};
    targetTexture->GetDesc(&targetDesc);

    D3D11_RENDER_TARGET_VIEW_DESC viewDesc = {};
    targetView->GetDesc(&viewDesc);
    const DXGI_FORMAT viewFormat =
        viewDesc.Format != DXGI_FORMAT_UNKNOWN ? viewDesc.Format : targetDesc.Format;

    Target target;
    target.route = route;
    target.encoding = encoding;
    target.width = targetDesc.Width;
    target.height = targetDesc.Height;
    target.viewAppliesSrgbConversion = FormatAppliesSrgbConversion(viewFormat);
    // A multisampled target cannot be copied into a single-sample source, and a
    // resolve would be a different pass with different cost.
    target.readable = targetDesc.SampleDesc.Count == 1;
    target.writable = viewDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2D ||
                      viewDesc.ViewDimension == D3D11_RTV_DIMENSION_TEXTURE2DARRAY;

    const Decision decision = Decide(request, target);
    if (logGate_.ShouldLog(decision.run, decision.reason)) {
        HookLogImportant("Sharpen: DX11 %s reason=%s %ux%u view=%d srgbView=%d samples=%u route=%d param=%.3f",
                         decision.run ? "running" : "idle", decision.reason, target.width, target.height,
                         static_cast<int>(viewFormat), target.viewAppliesSrgbConversion ? 1 : 0,
                         targetDesc.SampleDesc.Count, static_cast<int>(route),
                         static_cast<double>(decision.effectParameter));
    }
    if (!decision.run)
        return false;

    if (!EnsureDeviceObjects(device))
        return false;
    if (!EnsureSourceCopy(device, targetDesc, viewFormat))
        return false;

    ID3D11PixelShader* pixelShader = request.mode == Mode::Cas ? casShader_.Get() : rcasShader_.Get();
    if (!pixelShader)
        return false;

    const ShaderConstants constants = BuildShaderConstants(request.mode, decision, target.width, target.height);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(constantBuffer_.Get(), 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped)))
        return false;
    std::memcpy(mapped.pData, &constants, sizeof(constants));
    context->Unmap(constantBuffer_.Get(), 0);

    // The kernel reads a 3x3 neighbourhood, so it cannot run in place. The copy
    // is issued before any binding changes: nothing is bound as both source and
    // destination at any point.
    context->CopyResource(sourceCopy_.Get(), targetTexture.Get());

    SavedPipelineState saved;
    SavePipelineState(context, saved);

    const float blendFactor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    context->RSSetState(rasterizerState_.Get());
    context->OMSetBlendState(blendState_.Get(), blendFactor, 0xFFFFFFFFu);
    context->OMSetDepthStencilState(depthState_.Get(), 0);
    context->OMSetRenderTargets(1, &targetView, nullptr);
    context->VSSetShader(vertexShader_.Get(), nullptr, 0);
    context->PSSetShader(pixelShader, nullptr, 0);
    // A geometry shader left bound by the game would consume the generated
    // vertices before they ever reach the rasterizer, and a bound hull/domain
    // pair makes any non-patch topology - including this triangle list - an
    // invalid draw that the runtime silently discards.
    context->GSSetShader(nullptr, nullptr, 0);
    context->HSSetShader(nullptr, nullptr, 0);
    context->DSSetShader(nullptr, nullptr, 0);
    ID3D11Buffer* constantBufferView = constantBuffer_.Get();
    context->PSSetConstantBuffers(0, 1, &constantBufferView);
    ID3D11ShaderResourceView* sourceView = sourceView_.Get();
    context->PSSetShaderResources(0, 1, &sourceView);
    // The fullscreen triangle is generated from SV_VertexID: no input layout,
    // no vertex buffer, no index buffer.
    context->IASetInputLayout(nullptr);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ID3D11Buffer* nullVertexBuffer = nullptr;
    UINT nullStride = 0;
    UINT nullOffset = 0;
    context->IASetVertexBuffers(0, 1, &nullVertexBuffer, &nullStride, &nullOffset);
    context->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(target.width);
    viewport.Height = static_cast<float>(target.height);
    viewport.MaxDepth = 1.0f;
    context->RSSetViewports(1, &viewport);

    context->Draw(3, 0);

    RestorePipelineState(context, saved);
    return true;
}

void D3D11Pass::Abandon() {
    sourceView_.Detach();
    sourceCopy_.Detach();
    constantBuffer_.Detach();
    depthState_.Detach();
    blendState_.Detach();
    rasterizerState_.Detach();
    rcasShader_.Detach();
    casShader_.Detach();
    vertexShader_.Detach();
    copyWidth_ = 0;
    copyHeight_ = 0;
    copyFormat_ = DXGI_FORMAT_UNKNOWN;
    copyViewFormat_ = DXGI_FORMAT_UNKNOWN;
    ownerDevice_ = nullptr;
}

void D3D11Pass::Shutdown() {
    sourceView_.Reset();
    sourceCopy_.Reset();
    constantBuffer_.Reset();
    depthState_.Reset();
    blendState_.Reset();
    rasterizerState_.Reset();
    rcasShader_.Reset();
    casShader_.Reset();
    vertexShader_.Reset();
    copyWidth_ = 0;
    copyHeight_ = 0;
    copyFormat_ = DXGI_FORMAT_UNKNOWN;
    copyViewFormat_ = DXGI_FORMAT_UNKNOWN;
    ownerDevice_ = nullptr;
}

}  // namespace ce::sharpen
