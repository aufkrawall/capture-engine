#include "test_video_encoder_source_shared.h"

TEST(VideoEncoderSourceTest, DirectHdrP010ShaderWritesCanonicalRedCodes) {
    ce::ComGuard<ID3D11Device> baseDevice;
    ce::ComGuard<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel = {};
    const HRESULT deviceHr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                                baseDevice.addressof(), &featureLevel, context.addressof());
    ASSERT_TRUE(SUCCEEDED(deviceHr)) << std::hex << deviceHr;
    ASSERT_GE(featureLevel, D3D_FEATURE_LEVEL_11_0);

    ce::ComGuard<ID3D11Device3> device;
    ASSERT_TRUE(SUCCEEDED(baseDevice->QueryInterface(IID_PPV_ARGS(device.addressof()))));

    constexpr UINT kWidth = 2;
    constexpr UINT kHeight = 2;
    constexpr uint32_t kOpaqueRedRgb10 = 0xc00003ffu;
    const uint32_t sourcePixels[kWidth * kHeight] = {kOpaqueRedRgb10, kOpaqueRedRgb10, kOpaqueRedRgb10,
                                                     kOpaqueRedRgb10};
    D3D11_TEXTURE2D_DESC sourceDesc = {};
    sourceDesc.Width = kWidth;
    sourceDesc.Height = kHeight;
    sourceDesc.MipLevels = 1;
    sourceDesc.ArraySize = 1;
    sourceDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    sourceDesc.SampleDesc.Count = 1;
    sourceDesc.Usage = D3D11_USAGE_IMMUTABLE;
    sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sourceData = {};
    sourceData.pSysMem = sourcePixels;
    sourceData.SysMemPitch = kWidth * sizeof(uint32_t);
    ce::ComGuard<ID3D11Texture2D> sourceTexture;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&sourceDesc, &sourceData, sourceTexture.addressof())));

    D3D11_TEXTURE2D_DESC outputDesc = {};
    outputDesc.Width = kWidth;
    outputDesc.Height = kHeight;
    outputDesc.MipLevels = 1;
    outputDesc.ArraySize = 1;
    outputDesc.Format = DXGI_FORMAT_P010;
    outputDesc.SampleDesc.Count = 1;
    outputDesc.Usage = D3D11_USAGE_DEFAULT;
    outputDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ce::ComGuard<ID3D11Texture2D> outputTexture;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&outputDesc, nullptr, outputTexture.addressof())));

    auto createPlaneView = [&](DXGI_FORMAT format, UINT plane, ce::ComGuard<ID3D11RenderTargetView1>& view) {
        D3D11_RENDER_TARGET_VIEW_DESC1 desc = {};
        desc.Format = format;
        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = 0;
        desc.Texture2D.PlaneSlice = plane;
        return device->CreateRenderTargetView1(outputTexture.get(), &desc, view.addressof());
    };
    ce::ComGuard<ID3D11RenderTargetView1> lumaView;
    ce::ComGuard<ID3D11RenderTargetView1> chromaView;
    ASSERT_TRUE(SUCCEEDED(createPlaneView(DXGI_FORMAT_R16_UNORM, 0, lumaView)));
    ASSERT_TRUE(SUCCEEDED(createPlaneView(DXGI_FORMAT_R16G16_UNORM, 1, chromaView)));

    ce::ComGuard<ID3D11ShaderResourceView> sourceView;
    ASSERT_TRUE(SUCCEEDED(device->CreateShaderResourceView(sourceTexture.get(), nullptr, sourceView.addressof())));

    ce::ComGuard<ID3DBlob> vertexBlob;
    ce::ComGuard<ID3DBlob> lumaBlob;
    ce::ComGuard<ID3DBlob> chromaBlob;
    auto compile = [&](const char* entry, const char* target, ce::ComGuard<ID3DBlob>& blob) {
        ce::ComGuard<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(ce::video_color::kRgbColorConversionShaderSource,
                                      sizeof(ce::video_color::kRgbColorConversionShaderSource) - 1, nullptr, nullptr,
                                      nullptr, entry, target, 0, 0, blob.addressof(), errors.addressof());
        const std::string diagnostic =
            errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "";
        EXPECT_TRUE(SUCCEEDED(hr)) << entry << "/" << target << ": " << diagnostic;
        return hr;
    };
    ASSERT_TRUE(SUCCEEDED(compile("VS_Main", "vs_4_0", vertexBlob)));
    ASSERT_TRUE(SUCCEEDED(compile("PS_P010Y", "ps_4_0", lumaBlob)));
    ASSERT_TRUE(SUCCEEDED(compile("PS_P010UV", "ps_4_0", chromaBlob)));

    ce::ComGuard<ID3D11VertexShader> vertexShader;
    ce::ComGuard<ID3D11PixelShader> lumaShader;
    ce::ComGuard<ID3D11PixelShader> chromaShader;
    ASSERT_TRUE(SUCCEEDED(device->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(),
                                                     nullptr, vertexShader.addressof())));
    ASSERT_TRUE(SUCCEEDED(device->CreatePixelShader(lumaBlob->GetBufferPointer(), lumaBlob->GetBufferSize(), nullptr,
                                                    lumaShader.addressof())));
    ASSERT_TRUE(SUCCEEDED(device->CreatePixelShader(chromaBlob->GetBufferPointer(), chromaBlob->GetBufferSize(),
                                                    nullptr, chromaShader.addressof())));

    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ce::ComGuard<ID3D11SamplerState> sampler;
    ASSERT_TRUE(SUCCEEDED(device->CreateSamplerState(&samplerDesc, sampler.addressof())));

    struct CopyConstants {
        uint32_t colorTransform;
        uint32_t padding;
        float outputInvWidth;
        float outputInvHeight;
        float lumaSharpenStrength;
        float sdrWhiteNits;
        float padding2[2];
    };
    // sdrWhiteNits=0 selects limited range (default for P010); sdrWhiteNits>=1 selects full range.
    const CopyConstants constants = {0, 0, 1.0f / kWidth, 1.0f / kHeight, 0.0f, 0.0f, {0.0f, 0.0f}};
    D3D11_BUFFER_DESC constantsDesc = {};
    constantsDesc.ByteWidth = sizeof(constants);
    constantsDesc.Usage = D3D11_USAGE_IMMUTABLE;
    constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA constantsData = {};
    constantsData.pSysMem = &constants;
    ce::ComGuard<ID3D11Buffer> constantsBuffer;
    ASSERT_TRUE(SUCCEEDED(device->CreateBuffer(&constantsDesc, &constantsData, constantsBuffer.addressof())));

    ID3D11ShaderResourceView* sourceViewRaw = sourceView.get();
    ID3D11SamplerState* samplerRaw = sampler.get();
    ID3D11Buffer* constantsRaw = constantsBuffer.get();
    context->VSSetShader(vertexShader.get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, &sourceViewRaw);
    context->PSSetSamplers(0, 1, &samplerRaw);
    context->PSSetConstantBuffers(0, 1, &constantsRaw);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
    ID3D11RenderTargetView* lumaViewRaw = lumaView.get();
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &lumaViewRaw, nullptr);
    context->PSSetShader(lumaShader.get(), nullptr, 0);
    context->Draw(3, 0);

    viewport.Width = static_cast<float>(kWidth / 2);
    viewport.Height = static_cast<float>(kHeight / 2);
    ID3D11RenderTargetView* chromaViewRaw = chromaView.get();
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &chromaViewRaw, nullptr);
    context->PSSetShader(chromaShader.get(), nullptr, 0);
    context->Draw(3, 0);
    ID3D11RenderTargetView* nullRtv = nullptr;
    ID3D11ShaderResourceView* nullSrv = nullptr;
    context->OMSetRenderTargets(1, &nullRtv, nullptr);
    context->PSSetShaderResources(0, 1, &nullSrv);

    D3D11_TEXTURE2D_DESC stagingDesc = outputDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ce::ComGuard<ID3D11Texture2D> stagingTexture;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, stagingTexture.addressof())));
    context->CopyResource(stagingTexture.get(), outputTexture.get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    ASSERT_TRUE(SUCCEEDED(context->Map(stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped)));

    constexpr uint16_t kExpectedY = 294u << 6;
    constexpr uint16_t kExpectedCb = 387u << 6;
    constexpr uint16_t kExpectedCr = 960u << 6;
    const auto* bytes = static_cast<const uint8_t*>(mapped.pData);
    EXPECT_EQ(*reinterpret_cast<const uint16_t*>(bytes), kExpectedY);
    EXPECT_EQ(*reinterpret_cast<const uint16_t*>(bytes + sizeof(uint16_t)), kExpectedY);
    EXPECT_EQ(*reinterpret_cast<const uint16_t*>(bytes + mapped.RowPitch), kExpectedY);
    EXPECT_EQ(*reinterpret_cast<const uint16_t*>(bytes + mapped.RowPitch + sizeof(uint16_t)), kExpectedY);
    const auto* chroma = reinterpret_cast<const uint16_t*>(bytes + mapped.RowPitch * kHeight);
    EXPECT_EQ(chroma[0], kExpectedCb);
    EXPECT_EQ(chroma[1], kExpectedCr);
    context->Unmap(stagingTexture.get(), 0);
}

TEST(VideoEncoderSourceTest, DirectHdrP010ShaderProducesNeutralChromaForWhite) {
    ce::ComGuard<ID3D11Device> baseDevice;
    ce::ComGuard<ID3D11DeviceContext> context;
    D3D_FEATURE_LEVEL featureLevel = {};
    const HRESULT deviceHr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                                D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0, D3D11_SDK_VERSION,
                                                baseDevice.addressof(), &featureLevel, context.addressof());
    ASSERT_TRUE(SUCCEEDED(deviceHr)) << std::hex << deviceHr;
    ASSERT_GE(featureLevel, D3D_FEATURE_LEVEL_11_0);

    ce::ComGuard<ID3D11Device3> device;
    ASSERT_TRUE(SUCCEEDED(baseDevice->QueryInterface(IID_PPV_ARGS(device.addressof()))));

    constexpr UINT kWidth = 2;
    constexpr UINT kHeight = 2;
    constexpr uint32_t kNeutralRgb10 = [] {
        constexpr uint32_t kCode = 625u;  // PQ(203) * 1023 ≈ 625
        return (3u << 30) | (kCode << 20) | (kCode << 10) | kCode;
    }();
    const uint32_t sourcePixels[kWidth * kHeight] = {kNeutralRgb10, kNeutralRgb10, kNeutralRgb10, kNeutralRgb10};
    D3D11_TEXTURE2D_DESC sourceDesc = {};
    sourceDesc.Width = kWidth;
    sourceDesc.Height = kHeight;
    sourceDesc.MipLevels = 1;
    sourceDesc.ArraySize = 1;
    sourceDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    sourceDesc.SampleDesc.Count = 1;
    sourceDesc.Usage = D3D11_USAGE_IMMUTABLE;
    sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sourceData = {};
    sourceData.pSysMem = sourcePixels;
    sourceData.SysMemPitch = kWidth * sizeof(uint32_t);
    ce::ComGuard<ID3D11Texture2D> sourceTexture;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&sourceDesc, &sourceData, sourceTexture.addressof())));

    D3D11_TEXTURE2D_DESC outputDesc = {};
    outputDesc.Width = kWidth;
    outputDesc.Height = kHeight;
    outputDesc.MipLevels = 1;
    outputDesc.ArraySize = 1;
    outputDesc.Format = DXGI_FORMAT_P010;
    outputDesc.SampleDesc.Count = 1;
    outputDesc.Usage = D3D11_USAGE_DEFAULT;
    outputDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ce::ComGuard<ID3D11Texture2D> outputTexture;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&outputDesc, nullptr, outputTexture.addressof())));

    auto createPlaneView = [&](DXGI_FORMAT format, UINT plane, ce::ComGuard<ID3D11RenderTargetView1>& view) {
        D3D11_RENDER_TARGET_VIEW_DESC1 desc = {};
        desc.Format = format;
        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        desc.Texture2D.MipSlice = 0;
        desc.Texture2D.PlaneSlice = plane;
        return device->CreateRenderTargetView1(outputTexture.get(), &desc, view.addressof());
    };
    ce::ComGuard<ID3D11RenderTargetView1> lumaView;
    ce::ComGuard<ID3D11RenderTargetView1> chromaView;
    ASSERT_TRUE(SUCCEEDED(createPlaneView(DXGI_FORMAT_R16_UNORM, 0, lumaView)));
    ASSERT_TRUE(SUCCEEDED(createPlaneView(DXGI_FORMAT_R16G16_UNORM, 1, chromaView)));

    ce::ComGuard<ID3D11ShaderResourceView> sourceView;
    ASSERT_TRUE(SUCCEEDED(device->CreateShaderResourceView(sourceTexture.get(), nullptr, sourceView.addressof())));

    ce::ComGuard<ID3DBlob> vertexBlob;
    ce::ComGuard<ID3DBlob> lumaBlob;
    ce::ComGuard<ID3DBlob> chromaBlob;
    auto compile = [&](const char* entry, const char* target, ce::ComGuard<ID3DBlob>& blob) {
        ce::ComGuard<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(ce::video_color::kRgbColorConversionShaderSource,
                                      sizeof(ce::video_color::kRgbColorConversionShaderSource) - 1, nullptr, nullptr,
                                      nullptr, entry, target, 0, 0, blob.addressof(), errors.addressof());
        const std::string diagnostic =
            errors ? std::string(static_cast<const char*>(errors->GetBufferPointer()), errors->GetBufferSize()) : "";
        EXPECT_TRUE(SUCCEEDED(hr)) << entry << "/" << target << ": " << diagnostic;
        return hr;
    };
    ASSERT_TRUE(SUCCEEDED(compile("VS_Main", "vs_4_0", vertexBlob)));
    ASSERT_TRUE(SUCCEEDED(compile("PS_P010Y", "ps_4_0", lumaBlob)));
    ASSERT_TRUE(SUCCEEDED(compile("PS_P010UV", "ps_4_0", chromaBlob)));

    ce::ComGuard<ID3D11VertexShader> vertexShader;
    ce::ComGuard<ID3D11PixelShader> lumaShader;
    ce::ComGuard<ID3D11PixelShader> chromaShader;
    ASSERT_TRUE(SUCCEEDED(device->CreateVertexShader(vertexBlob->GetBufferPointer(), vertexBlob->GetBufferSize(),
                                                     nullptr, vertexShader.addressof())));
    ASSERT_TRUE(SUCCEEDED(device->CreatePixelShader(lumaBlob->GetBufferPointer(), lumaBlob->GetBufferSize(), nullptr,
                                                    lumaShader.addressof())));
    ASSERT_TRUE(SUCCEEDED(device->CreatePixelShader(chromaBlob->GetBufferPointer(), chromaBlob->GetBufferSize(), nullptr,
                                                    chromaShader.addressof())));

    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ce::ComGuard<ID3D11SamplerState> sampler;
    ASSERT_TRUE(SUCCEEDED(device->CreateSamplerState(&samplerDesc, sampler.addressof())));

    struct CopyConstants {
        uint32_t colorTransform;
        uint32_t padding;
        float outputInvWidth;
        float outputInvHeight;
        float lumaSharpenStrength;
        float sdrWhiteNits;
        float padding2[2];
    };
    const CopyConstants constants = {0, 0, 1.0f / kWidth, 1.0f / kHeight, 0.0f, 0.0f, {0.0f, 0.0f}};
    D3D11_BUFFER_DESC constantsDesc = {};
    constantsDesc.ByteWidth = sizeof(constants);
    constantsDesc.Usage = D3D11_USAGE_IMMUTABLE;
    constantsDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
    D3D11_SUBRESOURCE_DATA constantsData = {};
    constantsData.pSysMem = &constants;
    ce::ComGuard<ID3D11Buffer> constantsBuffer;
    ASSERT_TRUE(SUCCEEDED(device->CreateBuffer(&constantsDesc, &constantsData, constantsBuffer.addressof())));

    ID3D11ShaderResourceView* sourceViewRaw = sourceView.get();
    ID3D11SamplerState* samplerRaw = sampler.get();
    ID3D11Buffer* constantsRaw = constantsBuffer.get();
    context->VSSetShader(vertexShader.get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, &sourceViewRaw);
    context->PSSetSamplers(0, 1, &samplerRaw);
    context->PSSetConstantBuffers(0, 1, &constantsRaw);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);

    D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(kWidth), static_cast<float>(kHeight), 0.0f, 1.0f};
    ID3D11RenderTargetView* lumaViewRaw = lumaView.get();
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &lumaViewRaw, nullptr);
    context->PSSetShader(lumaShader.get(), nullptr, 0);
    context->Draw(3, 0);

    viewport.Width = static_cast<float>(kWidth / 2);
    viewport.Height = static_cast<float>(kHeight / 2);
    ID3D11RenderTargetView* chromaViewRaw = chromaView.get();
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &chromaViewRaw, nullptr);
    context->PSSetShader(chromaShader.get(), nullptr, 0);
    context->Draw(3, 0);
    ID3D11RenderTargetView* nullRtv = nullptr;
    ID3D11ShaderResourceView* nullSrv = nullptr;
    context->OMSetRenderTargets(1, &nullRtv, nullptr);
    context->PSSetShaderResources(0, 1, &nullSrv);

    D3D11_TEXTURE2D_DESC stagingDesc = outputDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ce::ComGuard<ID3D11Texture2D> stagingTexture;
    ASSERT_TRUE(SUCCEEDED(device->CreateTexture2D(&stagingDesc, nullptr, stagingTexture.addressof())));
    context->CopyResource(stagingTexture.get(), outputTexture.get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    ASSERT_TRUE(SUCCEEDED(context->Map(stagingTexture.get(), 0, D3D11_MAP_READ, 0, &mapped)));

    constexpr uint16_t kNeutralCb = 512u << 6;
    constexpr uint16_t kNeutralCr = 512u << 6;
    const auto* chromaOutput = reinterpret_cast<const uint16_t*>(static_cast<const uint8_t*>(mapped.pData) + mapped.RowPitch * kHeight);
    EXPECT_EQ(chromaOutput[0], kNeutralCb);
    EXPECT_EQ(chromaOutput[1], kNeutralCr);
    context->Unmap(stagingTexture.get(), 0);
}
