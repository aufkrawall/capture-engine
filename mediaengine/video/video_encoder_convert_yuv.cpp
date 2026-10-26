#include "video_encoder_internal.h"

#include "shader_conversion_route.h"

// RGB -> NV12/P010 with CE's own pixel shaders instead of
// ID3D11VideoContext::VideoProcessorBlt (route: shader_conversion_route.h).
// On NVIDIA the VideoProcessor work of the NVENC encoder device completed only
// after the encoder's earlier frames:
// every GPU timeline sample stalled at the first 3D step that had to follow the
// Blt (sessions 20261016_003600 / 010712 / 011829: 16-22 ms on the GPU against
// 0.03 ms on the CPU), so each conversion waited for NVENC and NVENC for the
// conversion, and inject p4/p5 overloaded although NVENC had headroom. A pixel
// shader runs on the 3D queue only; NVENC then waits for it, never the reverse.
// HDR output already used this route. Scaled output, FP16 SDR sources and
// unsampleable sources went to the VideoProcessor until 2026-10-26; they now
// take the shader scaler (frame_scaler.h), the RGB10 normalization and a
// sampleable copy.

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

bool VideoEncoder::ConvertRgbWithShaders(ID3D11Texture2D* source, ID3D11Texture2D* output, UINT outputArraySlice,
                                         bool outputIsHDR) {
    if (!source || !output || !EnsureSwapRBShader())
        return false;
    D3D11_TEXTURE2D_DESC sourceDesc = {};
    D3D11_TEXTURE2D_DESC outputDesc = {};
    source->GetDesc(&sourceDesc);
    output->GetDesc(&outputDesc);
    ce::video_format::ShaderConversionRouteInputs routeInputs;
    routeInputs.sourceFormat = sourceDesc.Format;
    routeInputs.sourceWidth = sourceDesc.Width;
    routeInputs.sourceHeight = sourceDesc.Height;
    routeInputs.sourceHasShaderResourceBind = (sourceDesc.BindFlags & D3D11_BIND_SHADER_RESOURCE) != 0;
    routeInputs.sourceSampleCount = sourceDesc.SampleDesc.Count;
    routeInputs.outputFormat = outputDesc.Format;
    routeInputs.outputWidth = outputDesc.Width;
    routeInputs.outputHeight = outputDesc.Height;
    routeInputs.sourceIsHdr = currentIsHDR;
    routeInputs.outputIsHdr = outputIsHDR;
    const ce::video_format::ShaderConversionRoute route = ce::video_format::DecideShaderConversionRoute(routeInputs);
    const char* logPrefix = outputIsHDR ? "HDR P010" : "SDR YUV";
    if (!route.convertible) {
        const auto log = shaderRouteRefusalLog.Observe(
            ce::log_meter::FieldKey(reinterpret_cast<uintptr_t>(route.refusal), static_cast<uint32_t>(sourceDesc.Format),
                                    static_cast<uint32_t>(outputDesc.Format)),
            GetTickCount64());
        if (log) {
            DLL_Log("[%s] Frame refused (%s): source=%ux%u fmt=%d hdr=%d output=%ux%u fmt=%d%s", logPrefix,
                    route.refusal, sourceDesc.Width, sourceDesc.Height, sourceDesc.Format, currentIsHDR ? 1 : 0,
                    outputDesc.Width, outputDesc.Height, outputDesc.Format,
                    ce::log_meter::SuppressedNote(log.suppressed).c_str());
        }
        return false;
    }

    ID3D11Texture2D* input = source;
    if (route.sampleableCopy) {
        input = sampleableSourceCopy.Prepare(d3d11Device, d3d11Context, source);
        if (!input)
            return false;
    }
    ce::ComGuard<ID3D11Texture2D> normalized;
    if (route.normalize != ce::video_format::RgbColorTransform::kNone) {
        D3D11_TEXTURE2D_DESC inputDesc = {};
        input->GetDesc(&inputDesc);
        normalized.reset(RenderFullscreenCopy(
            input, inputDesc.Width, inputDesc.Height, ce::video_format::GetRgbShaderResourceViewFormat(inputDesc.Format),
            DXGI_FORMAT_R10G10B10A2_UNORM, rgb10IntermediateTexture, rgb10IntermediateRTV, rgb10IntermediateWidth,
            rgb10IntermediateHeight, "RGB10", route.normalize, sdrWhiteNits));
        if (!normalized)
            return false;
        input = normalized.get();
        if (!outputIsHDR && currentIsHDR && !hdrToSdrLogged) {
            DLL_Log(
                "[HDR->SDR] Whole-frame shader tone map active: transform=%s sourceWhite=%.1f-nit "
                "output=BT709-G22 headroom=80%% passes=1 intermediate=RGB10 gamut=luminance-preserving "
                "driverVPTransfer=0 cpuWait=0 frameAlpha=opaque",
                ce::video_format::DescribeRgbColorTransform(route.normalize), sdrWhiteNits);
            hdrToSdrLogged = true;
        }
    }

    D3D11_TEXTURE2D_DESC inputDesc = {};
    input->GetDesc(&inputDesc);
    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = ce::video_format::GetRgbShaderResourceViewFormat(inputDesc.Format);
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    ce::ComGuard<ID3D11ShaderResourceView> rgbView;
    HRESULT hr = d3d11Device->CreateShaderResourceView(input, &srvDesc, rgbView.addressof());
    if (FAILED(hr) || !rgbView) {
        DLL_Log("[%s] Failed to create input SRV: HR=%x fmt=%d bind=%x", logPrefix, hr, inputDesc.Format,
                inputDesc.BindFlags);
        return false;
    }

    // Planes read the output-size image: the scaler's when the sizes differ.
    ID3D11ShaderResourceView* planeSource = rgbView.get();
    const ce::video_scale::KernelSpec kernel =
        ce::video_scale::KernelSpecFromConfig(savedConfig.scaling.quality, savedConfig.scaling.sharpness);
    if (route.scale) {
        ce::video_scale::FrameScaler::Request request;
        request.source = rgbView.get();
        request.sourceWidth = inputDesc.Width;
        request.sourceHeight = inputDesc.Height;
        request.outputWidth = outputDesc.Width;
        request.outputHeight = outputDesc.Height;
        request.kernel = kernel;
        request.sourceBits = route.scaleSourceBits;
        request.outputBits = route.outputBits;
        if (!frameScaler.Scale(d3d11Device, d3d11Context, request, &planeSource))
            return false;
    }

    CachedHdrP010OutputViews* outputViews = GetYuvPlaneOutputViews(output, outputArraySlice, logPrefix);
    if (!outputViews)
        return false;
    const bool fullRange = WantsFullOutputRange(savedConfig.colorRange);
    const bool eightBit = outputDesc.Format == DXGI_FORMAT_NV12;
    const bool bt2020 = outputIsHDR || _stricmp(savedConfig.colorSpace.c_str(), "bt2020") == 0;
    const ce::video_color::YuvPlaneTargets targets = {outputViews->lumaView, outputViews->chromaView,
                                                      outputDesc.Width, outputDesc.Height};
    const bool drawn =
        outputIsHDR
            ? ce::video_color::DrawHdrRgb10ToP010(d3d11Context, ConversionDrawKernels(), planeSource, targets,
                                                  fullRange)
            : ce::video_color::DrawSdrRgbToYuv(d3d11Context, ConversionDrawKernels(), planeSource, targets,
                                               ce::video_color::SdrYuvFlags(eightBit, bt2020, fullRange));
    if (!drawn) {
        DLL_Log("[%s] Failed to map shader constants", logPrefix);
        return false;
    }

    bool& routeLogged = outputIsHDR ? hdrP010DirectLogged : sdrYuvShaderLogged;
    if (!routeLogged) {
        DLL_Log("[%s] Direct shader conversion active: source=%ux%u fmt=%d copy=%s normalize=%s scaler=%s "
                "output=%s %ux%u matrix=%s range=%s chroma=%s driverVP=0 cpuWait=0",
                logPrefix, sourceDesc.Width, sourceDesc.Height, sourceDesc.Format,
                route.resolveMultisample ? "resolve" : (route.sampleableCopy ? "copy" : "none"),
                ce::video_format::DescribeRgbColorTransform(route.normalize),
                route.scale ? ce::video_scale::DescribeKernel(kernel) : "none", eightBit ? "NV12" : "P010",
                outputDesc.Width, outputDesc.Height, bt2020 ? "BT2020NCL" : "BT709", fullRange ? "full" : "limited",
                outputIsHDR ? "top-left" : "left");
        routeLogged = true;
    }
    return true;
}
