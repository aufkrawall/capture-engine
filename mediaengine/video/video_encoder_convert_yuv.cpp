#include "video_encoder_internal.h"

#include <algorithm>

// SDR RGB -> NV12/P010 with CE's own pixel shaders instead of
// ID3D11VideoContext::VideoProcessorBlt. On NVIDIA the VideoProcessor work of
// the NVENC encoder device completed only after the encoder's earlier frames:
// every GPU timeline sample stalled at the first 3D step that had to follow the
// Blt (sessions 20261016_003600 / 010712 / 011829: 16-22 ms on the GPU against
// 0.03 ms on the CPU), so each conversion waited for NVENC and NVENC for the
// conversion, and inject p4/p5 overloaded although NVENC had headroom. A pixel
// shader runs on the 3D queue only; NVENC then waits for it, never the reverse.
// HDR output already used this route (ConvertHdrRgb10ToP010).

VideoEncoder::CachedHdrP010OutputViews* VideoEncoder::GetYuvPlaneOutputViews(ID3D11Texture2D* output,
                                                                             UINT outputArraySlice,
                                                                             const char* logPrefix) {
    for (auto& cached : hdrP010OutputViewCache) {
        if (cached.texture == output && cached.arraySlice == outputArraySlice)
            return &cached;
    }
    D3D11_TEXTURE2D_DESC outputDesc = {};
    output->GetDesc(&outputDesc);
    const bool eightBit = outputDesc.Format == DXGI_FORMAT_NV12;
    CachedHdrP010OutputViews cached = {};
    cached.texture = output;
    cached.arraySlice = outputArraySlice;
    auto createPlaneView = [&](DXGI_FORMAT format, UINT plane, ID3D11RenderTargetView1** view) -> HRESULT {
        D3D11_RENDER_TARGET_VIEW_DESC1 desc = {};
        desc.Format = format;
        if (outputDesc.ArraySize > 1) {
            desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2DARRAY;
            desc.Texture2DArray.MipSlice = 0;
            desc.Texture2DArray.FirstArraySlice = outputArraySlice;
            desc.Texture2DArray.ArraySize = 1;
            desc.Texture2DArray.PlaneSlice = plane;
        } else {
            desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
            desc.Texture2D.MipSlice = 0;
            desc.Texture2D.PlaneSlice = plane;
        }
        return d3d11Device->CreateRenderTargetView1(output, &desc, view);
    };
    HRESULT hr = createPlaneView(eightBit ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R16_UNORM, 0, &cached.lumaView);
    if (SUCCEEDED(hr))
        hr = createPlaneView(eightBit ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_UNORM, 1, &cached.chromaView);
    if (FAILED(hr) || !cached.lumaView || !cached.chromaView) {
        DLL_Log("[%s] Failed to create plane RTVs: HR=%x fmt=%d bind=%x array=%u slice=%u", logPrefix, hr,
                outputDesc.Format, outputDesc.BindFlags, outputDesc.ArraySize, outputArraySlice);
        if (cached.lumaView)
            cached.lumaView->Release();
        if (cached.chromaView)
            cached.chromaView->Release();
        return nullptr;
    }
    hdrP010OutputViewCache.push_back(cached);
    return &hdrP010OutputViewCache.back();
}

bool VideoEncoder::CanConvertSdrYuvWithShader(ID3D11Texture2D* input, ID3D11Texture2D* output,
                                              const char** reason) const {
    D3D11_TEXTURE2D_DESC inputDesc = {};
    D3D11_TEXTURE2D_DESC outputDesc = {};
    input->GetDesc(&inputDesc);
    output->GetDesc(&outputDesc);
    const DXGI_FORMAT srvFormat = ce::video_format::GetRgbShaderResourceViewFormat(inputDesc.Format);
    if (outputDesc.Format != DXGI_FORMAT_P010 && outputDesc.Format != DXGI_FORMAT_NV12) {
        *reason = "output format";
    } else if (srvFormat == DXGI_FORMAT_UNKNOWN || ce::video_format::IsFp16RgbInputFormat(inputDesc.Format)) {
        *reason = "input format";  // linear FP16 keeps the VideoProcessor's transfer handling
    } else if ((inputDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) == 0 || inputDesc.SampleDesc.Count != 1) {
        *reason = "input not sampleable";
    } else if (inputDesc.Width != outputDesc.Width || inputDesc.Height != outputDesc.Height ||
               (outputDesc.Width & 1) != 0 || (outputDesc.Height & 1) != 0) {
        *reason = "scaling";  // the VideoProcessor's scaler stays the scaling path
    } else {
        *reason = nullptr;
        return true;
    }
    return false;
}

bool VideoEncoder::ConvertSdrRgbToYuv(ID3D11Texture2D* input, ID3D11Texture2D* output, UINT outputArraySlice,
                                      const std::string& configuredColorSpace) {
    if (!input || !output || !EnsureSwapRBShader())
        return false;
    D3D11_TEXTURE2D_DESC inputDesc = {};
    D3D11_TEXTURE2D_DESC outputDesc = {};
    input->GetDesc(&inputDesc);
    output->GetDesc(&outputDesc);
    CachedHdrP010OutputViews* outputViews = GetYuvPlaneOutputViews(output, outputArraySlice, "SDR YUV");
    if (!outputViews)
        return false;

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = ce::video_format::GetRgbShaderResourceViewFormat(inputDesc.Format);
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    ce::ComGuard<ID3D11ShaderResourceView> sourceView;
    HRESULT hr = d3d11Device->CreateShaderResourceView(input, &srvDesc, sourceView.addressof());
    if (FAILED(hr) || !sourceView) {
        DLL_Log("[SDR YUV] Failed to create input SRV: HR=%x fmt=%d bind=%x", hr, inputDesc.Format,
                inputDesc.BindFlags);
        return false;
    }

    const bool eightBit = outputDesc.Format == DXGI_FORMAT_NV12;
    const bool bt2020 = _stricmp(configuredColorSpace.c_str(), "bt2020") == 0;
    const bool fullRange = WantsFullOutputRange(savedConfig.colorRange);
    struct CopyConstants {
        uint32_t colorTransform;
        uint32_t yuvFlags;
        float outputInvWidth;
        float outputInvHeight;
        float lumaSharpenStrength;
        float padding[3];
    };
    static_assert(sizeof(CopyConstants) == 32);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = d3d11Context->Map(swapRBShaderCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr) || !mapped.pData) {
        DLL_Log("[SDR YUV] Failed to map shader constants: HR=%x", hr);
        return false;
    }
    const uint32_t yuvFlags = (eightBit ? 1u : 0u) | (bt2020 ? 2u : 0u) | (fullRange ? 4u : 0u);
    *static_cast<CopyConstants*>(mapped.pData) = {0,    yuvFlags, 1.0f / static_cast<float>(outputDesc.Width),
                                                  1.0f / static_cast<float>(outputDesc.Height), 0.0f, {}};
    d3d11Context->Unmap(swapRBShaderCB, 0);

    ID3D11ShaderResourceView* sourceViewRaw = sourceView.get();
    d3d11Context->VSSetShader(swapRBShaderVS, nullptr, 0);
    d3d11Context->PSSetShaderResources(0, 1, &sourceViewRaw);
    d3d11Context->PSSetSamplers(0, 1, &hdrP010Sampler);
    d3d11Context->PSSetConstantBuffers(0, 1, &swapRBShaderCB);
    d3d11Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3d11Context->IASetInputLayout(nullptr);

    D3D11_VIEWPORT viewport = {};
    viewport.Width = static_cast<float>(outputDesc.Width);
    viewport.Height = static_cast<float>(outputDesc.Height);
    viewport.MaxDepth = 1.0f;
    d3d11Context->RSSetViewports(1, &viewport);
    ID3D11RenderTargetView* lumaView = outputViews->lumaView;
    d3d11Context->OMSetRenderTargets(1, &lumaView, nullptr);
    d3d11Context->PSSetShader(sdrYuvLumaPS, nullptr, 0);
    d3d11Context->Draw(3, 0);

    // NOLINTNEXTLINE(bugprone-integer-division) - 4:2:0 chroma is half-size and dimensions are even by construction
    viewport.Width = static_cast<float>(outputDesc.Width / 2);
    // NOLINTNEXTLINE(bugprone-integer-division) - 4:2:0 chroma is half-size and dimensions are even by construction
    viewport.Height = static_cast<float>(outputDesc.Height / 2);
    d3d11Context->RSSetViewports(1, &viewport);
    ID3D11RenderTargetView* chromaView = outputViews->chromaView;
    d3d11Context->OMSetRenderTargets(1, &chromaView, nullptr);
    d3d11Context->PSSetShader(sdrYuvChromaPS, nullptr, 0);
    d3d11Context->Draw(3, 0);

    ID3D11RenderTargetView* nullRtv = nullptr;
    d3d11Context->OMSetRenderTargets(1, &nullRtv, nullptr);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    d3d11Context->PSSetShaderResources(0, 1, &nullSrv);

    if (!sdrYuvShaderLogged) {
        DLL_Log("[SDR YUV] Direct shader conversion active: inputFmt=%d output=%s matrix=%s range=%s chroma=left "
                "planes=%s %ux%u driverVP=0 cpuWait=0",
                inputDesc.Format, eightBit ? "NV12" : "P010", bt2020 ? "BT2020NCL" : "BT709",
                fullRange ? "full" : "limited", eightBit ? "R8/R8G8" : "R16/R16G16", outputDesc.Width,
                outputDesc.Height);
        sdrYuvShaderLogged = true;
    }
    return true;
}
