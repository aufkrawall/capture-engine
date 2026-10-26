#include "video_encoder_internal.h"

#include "runtime_shader_compiler.h"

bool VideoEncoder::EnsureSwapRBShader() {
    if (swapRBShaderCreated)
        return true;

    // Build-time bytecode (precompiled_shaders.h); only a build without it compiles here.
    const ce::video_color::RuntimeShaderEntry entries[] = {
        {"VS_Main", "vs_4_0"},   {"PS_Main", "ps_4_0"}, {"PS_P010Y", "ps_4_0"},
        {"PS_P010UV", "ps_4_0"}, {"PS_YuvY", "ps_4_0"}, {"PS_YuvUV", "ps_4_0"}};
    ce::ComGuard<ID3D11VertexShader> copyVs;
    ce::ComGuard<ID3D11PixelShader> pixel[5];
    HRESULT hr = S_OK;
    const bool created = ce::video_color::PrepareShaderBytecode(
        ce::video_color::kRgbColorConversionShaderSource, strlen(ce::video_color::kRgbColorConversionShaderSource),
        entries, 6, "RGBConvert", [&](const ce::video_color::ShaderBytecodeView* shaders) {
            hr = d3d11Device->CreateVertexShader(shaders[0].data, shaders[0].size, nullptr, copyVs.addressof());
            for (int i = 0; i < 5 && SUCCEEDED(hr); ++i)
                hr = d3d11Device->CreatePixelShader(shaders[i + 1].data, shaders[i + 1].size, nullptr,
                                                    pixel[i].addressof());
            return SUCCEEDED(hr);
        });
    if (!created) {
        DLL_Log("[RGBConvert] Conversion shader preparation failed: HR=%x", hr);
        return false;
    }
    ce::ComGuard<ID3D11PixelShader>& copyPs = pixel[0];
    ce::ComGuard<ID3D11PixelShader>& p010Y = pixel[1];
    ce::ComGuard<ID3D11PixelShader>& p010Uv = pixel[2];
    ce::ComGuard<ID3D11PixelShader>& yuvY = pixel[3];
    ce::ComGuard<ID3D11PixelShader>& yuvUv = pixel[4];

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
    sdrYuvLumaPS = yuvY.release();
    sdrYuvChromaPS = yuvUv.release();
    swapRBSampler = copySampler.release();
    hdrP010Sampler = p010Sampler.release();
    swapRBShaderCB = constants.release();
    swapRBShaderCreated = true;
    DLL_Log("[RGBConvert] Copy/scRGB/P010/SDR-YUV shaders created successfully");
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

    const bool drawn = ce::video_color::DrawRgbNormalize(d3d11Context, ConversionDrawKernels(), srv, cachedRTV, w, h,
                                                         colorTransform, toneMapSdrWhiteNits);
    srv->Release();
    if (!drawn) {
        DLL_Log("[%s] Failed to map shader constant buffer", logPrefix);
        return nullptr;
    }

    cachedTexture->AddRef();  // Caller releases
    return cachedTexture;
}

ce::video_color::RgbYuvDrawKernels VideoEncoder::ConversionDrawKernels() const {
    ce::video_color::RgbYuvDrawKernels kernels;
    kernels.vertexShader = swapRBShaderVS;
    kernels.normalizeShader = swapRBShaderPS;
    kernels.p010LumaShader = hdrP010LumaPS;
    kernels.p010ChromaShader = hdrP010ChromaPS;
    kernels.yuvLumaShader = sdrYuvLumaPS;
    kernels.yuvChromaShader = sdrYuvChromaPS;
    kernels.pointSampler = swapRBSampler;
    kernels.linearSampler = hdrP010Sampler;
    kernels.constants = swapRBShaderCB;
    return kernels;
}

ID3D11Texture2D* VideoEncoder::SwapRBChannels(ID3D11Texture2D* input, uint32_t w, uint32_t h) {
    return RenderFullscreenCopy(input, w, h, DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_B8G8R8A8_UNORM, swapRBTexture,
                                swapRBTextureRTV, swapRBTexWidth, swapRBTexHeight, "SwapRB");
}
