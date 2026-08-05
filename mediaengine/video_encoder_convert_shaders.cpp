#include "video_encoder_internal.h"

bool VideoEncoder::EnsureSwapRBShader() {
    if (swapRBShaderCreated)
        return true;

    // ID3DBlob's implementation and vtable live in d3dcompiler_47.dll. Keep the
    // module loaded until every compiler-owned blob below has been consumed and
    // released (ModuleGuard is declared first, so it is destroyed last).
    ce::ModuleGuard d3dCompiler(ce::security::LoadSystemLibrary(L"d3dcompiler_47.dll"));
    if (!d3dCompiler) {
        DLL_Log("[SwapRB] Failed to load d3dcompiler_47.dll");
        return false;
    }

    typedef HRESULT(WINAPI * pD3DCompile)(LPCVOID, SIZE_T, LPCSTR, const D3D_SHADER_MACRO*, ID3DInclude*, LPCSTR,
                                          LPCSTR, UINT, UINT, ID3DBlob**, ID3DBlob**);
    pD3DCompile d3dCompile = (pD3DCompile)GetProcAddress(d3dCompiler.get(), "D3DCompile");
    if (!d3dCompile) {
        DLL_Log("[SwapRB] Failed to get D3DCompile");
        return false;
    }

    auto compileShader = [&](const char* entry, const char* target, ce::ComGuard<ID3DBlob>& output) -> HRESULT {
        ce::ComGuard<ID3DBlob> errors;
        const HRESULT compileHr = d3dCompile(
            ce::video_color::kRgbColorConversionShaderSource,
            strlen(ce::video_color::kRgbColorConversionShaderSource), nullptr, nullptr, nullptr, entry, target, 0, 0,
            output.addressof(), errors.addressof());
        if (errors) {
            DLL_Log("[RGBConvert] %s/%s compiler output: %s", entry, target,
                    static_cast<const char*>(errors->GetBufferPointer()));
        }
        return compileHr;
    };

    ce::ComGuard<ID3DBlob> vsBlob;
    ce::ComGuard<ID3DBlob> copyPsBlob;

    ce::ComGuard<ID3DBlob> p010YBlob;
    ce::ComGuard<ID3DBlob> p010UvBlob;
    HRESULT hr = compileShader("VS_Main", "vs_4_0", vsBlob);
    if (SUCCEEDED(hr))
        hr = compileShader("PS_Main", "ps_4_0", copyPsBlob);
    if (SUCCEEDED(hr))
        hr = compileShader("PS_P010Y", "ps_4_0", p010YBlob);
    if (SUCCEEDED(hr))
        hr = compileShader("PS_P010UV", "ps_4_0", p010UvBlob);
    if (FAILED(hr)) {
        DLL_Log("[RGBConvert] Runtime shader compilation failed: HR=%x", hr);
        return false;
    }

    ce::ComGuard<ID3D11VertexShader> copyVs;
    ce::ComGuard<ID3D11PixelShader> copyPs;
    ce::ComGuard<ID3D11PixelShader> p010Y;
    ce::ComGuard<ID3D11PixelShader> p010Uv;
    hr = d3d11Device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                         copyVs.addressof());
    if (SUCCEEDED(hr))
        hr = d3d11Device->CreatePixelShader(copyPsBlob->GetBufferPointer(), copyPsBlob->GetBufferSize(), nullptr,
                                            copyPs.addressof());
    if (SUCCEEDED(hr))
        hr = d3d11Device->CreatePixelShader(p010YBlob->GetBufferPointer(), p010YBlob->GetBufferSize(), nullptr,
                                            p010Y.addressof());
    if (SUCCEEDED(hr))
        hr = d3d11Device->CreatePixelShader(p010UvBlob->GetBufferPointer(), p010UvBlob->GetBufferSize(), nullptr,
                                            p010Uv.addressof());
    if (FAILED(hr)) {
        DLL_Log("[RGBConvert] Runtime shader creation failed: HR=%x", hr);
        return false;
    }

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D11_SAMPLER_DESC sampDesc = {};
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampDesc.AddressU = sampDesc.AddressV = sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ce::ComGuard<ID3D11SamplerState> copySampler;
    hr = d3d11Device->CreateSamplerState(&sampDesc, copySampler.addressof());
    if (FAILED(hr)) {
        DLL_Log("[SwapRB] CreateSamplerState failed: HR=%x", hr);
        return false;
    }
    sampDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    ce::ComGuard<ID3D11SamplerState> p010Sampler;
    hr = d3d11Device->CreateSamplerState(&sampDesc, p010Sampler.addressof());
    if (FAILED(hr)) {
        DLL_Log("[HDR P010] CreateSamplerState failed: HR=%x", hr);
        return false;
    }

    D3D11_BUFFER_DESC cbDesc = {};
    cbDesc.ByteWidth = 32;
    cbDesc.Usage = D3D11_USAGE_DYNAMIC;
    cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
    ce::ComGuard<ID3D11Buffer> constants;
    hr = d3d11Device->CreateBuffer(&cbDesc, nullptr, constants.addressof());
    if (FAILED(hr)) {
        DLL_Log("[SwapRB] Create constant buffer failed: HR=%x", hr);
        return false;
    }

    swapRBShaderVS = copyVs.release();
    swapRBShaderPS = copyPs.release();
    hdrP010LumaPS = p010Y.release();
    hdrP010ChromaPS = p010Uv.release();
    swapRBSampler = copySampler.release();
    hdrP010Sampler = p010Sampler.release();
    swapRBShaderCB = constants.release();
    swapRBShaderCreated = true;
    DLL_Log("[RGBConvert] Copy/scRGB/P010 shaders created successfully (compiler lifetime blob-scoped)");
    return true;
}

ID3D11Texture2D* VideoEncoder::RenderFullscreenCopy(ID3D11Texture2D* input, uint32_t w, uint32_t h,
                                                    DXGI_FORMAT inputSrvFormat, DXGI_FORMAT outputFormat,
                                                    ID3D11Texture2D*& cachedTexture, ID3D11RenderTargetView*& cachedRTV,
                                                    uint32_t& cachedWidth, uint32_t& cachedHeight,
                                                    const char* logPrefix,
                                                    ce::video_format::RgbColorTransform colorTransform,
                                                    float toneMapSdrWhiteNits) {
    if (!EnsureSwapRBShader())
        return nullptr;

    if (!cachedTexture || cachedWidth != w || cachedHeight != h) {
        if (cachedRTV) {
            cachedRTV->Release();
            cachedRTV = nullptr;
        }
        if (cachedTexture) {
            cachedTexture->Release();
            cachedTexture = nullptr;
        }

        D3D11_TEXTURE2D_DESC outDesc = {};
        outDesc.Width = w;
        outDesc.Height = h;
        outDesc.MipLevels = 1;
        outDesc.ArraySize = 1;
        outDesc.Format = outputFormat;
        outDesc.SampleDesc.Count = 1;
        outDesc.Usage = D3D11_USAGE_DEFAULT;
        outDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;

        HRESULT hr = d3d11Device->CreateTexture2D(&outDesc, nullptr, &cachedTexture);
        if (FAILED(hr)) {
            DLL_Log("[%s] Failed to create output texture fmt=%d: HR=%x", logPrefix, outputFormat, hr);
            return nullptr;
        }

        hr = d3d11Device->CreateRenderTargetView(cachedTexture, nullptr, &cachedRTV);
        if (FAILED(hr)) {
            DLL_Log("[%s] Failed to create RTV: HR=%x", logPrefix, hr);
            cachedTexture->Release();
            cachedTexture = nullptr;
            return nullptr;
        }

        cachedWidth = w;
        cachedHeight = h;
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = inputSrvFormat;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* srv = nullptr;
    D3D11_TEXTURE2D_DESC inputDesc = {};
    input->GetDesc(&inputDesc);
    HRESULT hr = d3d11Device->CreateShaderResourceView(input, &srvDesc, &srv);
    if (FAILED(hr)) {
        static std::atomic<int> srvFailLogCount{0};
        if (srvFailLogCount.fetch_add(1, std::memory_order_relaxed) < 10) {
            DLL_Log("[%s] Failed to create SRV: texFmt=%d srvFmt=%d bind=%x misc=%x HR=%x", logPrefix, inputDesc.Format,
                    inputSrvFormat, inputDesc.BindFlags, inputDesc.MiscFlags, hr);
        }
        return nullptr;
    }

    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = d3d11Context->Map(swapRBShaderCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr) || !mapped.pData) {
        DLL_Log("[%s] Failed to map shader constant buffer: HR=%x", logPrefix, hr);
        srv->Release();
        return nullptr;
    }
    memset(mapped.pData, 0, 32);
    uint32_t* cbData = static_cast<uint32_t*>(mapped.pData);
    cbData[0] = static_cast<uint32_t>(colorTransform);
    float* cbFloats = static_cast<float*>(mapped.pData);
    cbFloats[5] = std::clamp(toneMapSdrWhiteNits, 80.0f, 1000.0f);
    d3d11Context->Unmap(swapRBShaderCB, 0);

    D3D11_VIEWPORT vp = {};
    vp.Width = (float)w;
    vp.Height = (float)h;
    vp.MaxDepth = 1.0f;
    d3d11Context->RSSetViewports(1, &vp);
    d3d11Context->OMSetRenderTargets(1, &cachedRTV, nullptr);
    d3d11Context->VSSetShader(swapRBShaderVS, nullptr, 0);
    d3d11Context->PSSetShader(swapRBShaderPS, nullptr, 0);
    d3d11Context->PSSetShaderResources(0, 1, &srv);
    d3d11Context->PSSetSamplers(0, 1, &swapRBSampler);
    d3d11Context->PSSetConstantBuffers(0, 1, &swapRBShaderCB);
    d3d11Context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    d3d11Context->IASetInputLayout(nullptr);
    d3d11Context->Draw(3, 0);

    // Unbind render target and SRV
    ID3D11RenderTargetView* nullRTV = nullptr;
    d3d11Context->OMSetRenderTargets(1, &nullRTV, nullptr);
    ID3D11ShaderResourceView* nullSRV = nullptr;
    d3d11Context->PSSetShaderResources(0, 1, &nullSRV);
    srv->Release();

    cachedTexture->AddRef();  // Caller releases
    return cachedTexture;
}

bool VideoEncoder::ConvertHdrRgb10ToP010(ID3D11Texture2D* input, ID3D11Texture2D* output, UINT outputArraySlice) {
    if (!input || !output || !EnsureSwapRBShader()) {
        return false;
    }

    D3D11_TEXTURE2D_DESC inputDesc = {};
    D3D11_TEXTURE2D_DESC outputDesc = {};
    input->GetDesc(&inputDesc);
    output->GetDesc(&outputDesc);
    if (inputDesc.Format != DXGI_FORMAT_R10G10B10A2_UNORM || outputDesc.Format != DXGI_FORMAT_P010 ||
        outputArraySlice >= outputDesc.ArraySize || (outputDesc.Width & 1) != 0 || (outputDesc.Height & 1) != 0) {
        DLL_Log(
            "[HDR P010] Invalid direct conversion surfaces: inputFmt=%d outputFmt=%d output=%ux%u array=%u slice=%u",
            inputDesc.Format, outputDesc.Format, outputDesc.Width, outputDesc.Height, outputDesc.ArraySize,
            outputArraySlice);
        return false;
    }

    CachedHdrP010OutputViews* outputViews = nullptr;
    for (auto& cached : hdrP010OutputViewCache) {
        if (cached.texture == output && cached.arraySlice == outputArraySlice) {
            outputViews = &cached;
            break;
        }
    }
    if (!outputViews) {
        CachedHdrP010OutputViews cached = {};
        cached.texture = output;
        cached.arraySlice = outputArraySlice;

        auto createPlaneView = [&](DXGI_FORMAT format, UINT plane,
                                   ID3D11RenderTargetView1** view) -> HRESULT {
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

        HRESULT hr = createPlaneView(DXGI_FORMAT_R16_UNORM, 0, &cached.lumaView);
        if (SUCCEEDED(hr)) {
            hr = createPlaneView(DXGI_FORMAT_R16G16_UNORM, 1, &cached.chromaView);
        }
        if (FAILED(hr) || !cached.lumaView || !cached.chromaView) {
            DLL_Log(
                "[HDR P010] Failed to create plane RTVs: HR=%x bind=%x array=%u slice=%u; refusing corrupt VP "
                "fallback",
                hr, outputDesc.BindFlags, outputDesc.ArraySize, outputArraySlice);
            if (cached.lumaView)
                cached.lumaView->Release();
            if (cached.chromaView)
                cached.chromaView->Release();
            return false;
        }
        hdrP010OutputViewCache.push_back(cached);
        outputViews = &hdrP010OutputViewCache.back();
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC srvDesc = {};
    srvDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    srvDesc.ViewDimension = D3D11_SRV_DIMENSION_TEXTURE2D;
    srvDesc.Texture2D.MostDetailedMip = 0;
    srvDesc.Texture2D.MipLevels = 1;
    ID3D11ShaderResourceView* inputView = nullptr;
    HRESULT hr = d3d11Device->CreateShaderResourceView(input, &srvDesc, &inputView);
    if (FAILED(hr) || !inputView) {
        DLL_Log("[HDR P010] Failed to create RGB10 input SRV: HR=%x bind=%x", hr, inputDesc.BindFlags);
        return false;
    }

    struct CopyConstants {
        uint32_t colorTransform;
        uint32_t padding;
        float outputInvWidth;
        float outputInvHeight;
        float lumaSharpenStrength;
        float padding2[3];
    };
    static_assert(sizeof(CopyConstants) == 32);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    hr = d3d11Context->Map(swapRBShaderCB, 0, D3D11_MAP_WRITE_DISCARD, 0, &mapped);
    if (FAILED(hr) || !mapped.pData) {
        DLL_Log("[HDR P010] Failed to map shader constants: HR=%x", hr);
        inputView->Release();
        return false;
    }
    const float lumaSharpenStrength =
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        scalingEnabled ? std::clamp(savedConfig.scaling.sharpness / 400.0f, 0.0f, 0.25f) : 0.0f;
    const float fullRangeFlag = WantsFullOutputRange(savedConfig.colorRange) ? 1.0f : 0.0f;
    *static_cast<CopyConstants*>(mapped.pData) = {
        0, 0, 1.0f / static_cast<float>(outputDesc.Width), 1.0f / static_cast<float>(outputDesc.Height),
        lumaSharpenStrength, {fullRangeFlag, 0.0f, 0.0f}};
    d3d11Context->Unmap(swapRBShaderCB, 0);

    d3d11Context->VSSetShader(swapRBShaderVS, nullptr, 0);
    d3d11Context->PSSetShaderResources(0, 1, &inputView);
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
    d3d11Context->PSSetShader(hdrP010LumaPS, nullptr, 0);
    d3d11Context->Draw(3, 0);

    // NOLINTNEXTLINE(bugprone-integer-division) - P010 chroma is half-size and dimensions are even by construction
    viewport.Width = static_cast<float>(outputDesc.Width / 2);
    // NOLINTNEXTLINE(bugprone-integer-division) - P010 chroma is half-size and dimensions are even by construction
    viewport.Height = static_cast<float>(outputDesc.Height / 2);
    d3d11Context->RSSetViewports(1, &viewport);
    ID3D11RenderTargetView* chromaView = outputViews->chromaView;
    d3d11Context->OMSetRenderTargets(1, &chromaView, nullptr);
    d3d11Context->PSSetShader(hdrP010ChromaPS, nullptr, 0);
    d3d11Context->Draw(3, 0);

    ID3D11RenderTargetView* nullRtv = nullptr;
    d3d11Context->OMSetRenderTargets(1, &nullRtv, nullptr);
    ID3D11ShaderResourceView* nullSrv = nullptr;
    d3d11Context->PSSetShaderResources(0, 1, &nullSrv);
    inputView->Release();

    if (!hdrP010DirectLogged) {
        DLL_Log(
            "[HDR P010] Direct shader conversion active: input=RGB10_PQ_P2020 output=P010_BT2020NCL_%s "
            "matrix=shader chroma=top-left planes=R16/R16G16 scaling=%ux%u->%ux%u lumaSharpen=%.3f "
            "driverVP=0 cpuWait=0",
            WantsFullOutputRange(savedConfig.colorRange) ? "FULL" : "LIMITED", inputDesc.Width, inputDesc.Height,
            outputDesc.Width, outputDesc.Height, lumaSharpenStrength);
        hdrP010DirectLogged = true;
    }
    return true;
}

ID3D11Texture2D* VideoEncoder::SwapRBChannels(ID3D11Texture2D* input, uint32_t w, uint32_t h) {
    return RenderFullscreenCopy(input, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, swapRBTexture,
                                swapRBTextureRTV, swapRBTexWidth, swapRBTexHeight, "SwapRB");
}
