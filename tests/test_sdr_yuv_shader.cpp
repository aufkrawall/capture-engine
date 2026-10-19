#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <mmsystem.h>

#include <filesystem>
#include <string>
#include <vector>

#include "common/platform/raii_helpers.h"
#include "mediaengine/video/video_color_conversion_shader.h"
#include "source_fragment_reader.h"

// CE's SDR RGB -> NV12/P010 shaders (PS_YuvY / PS_YuvUV) replace the D3D11
// VideoProcessor, whose Blt queued behind NVENC on NVIDIA (sessions
// 20261016_003600 / 010712 / 011829). These render real NV12/P010 surfaces
// through plane RTVs, as the encoder does, and check the exact codes.

namespace {

struct YuvCodes {
    std::vector<uint16_t> luma;    // row-major, width x height
    std::vector<uint16_t> chroma;  // interleaved Cb,Cr, (width/2) x (height/2)
};

constexpr uint32_t kFlag8Bit = 1u;
constexpr uint32_t kFlagBt2020 = 2u;
constexpr uint32_t kFlagFullRange = 4u;

// Renders `pixels` (8-bit RGBA or BGRA, row-major) and returns raw codes:
// 8-bit values for NV12, 10-bit codes (value >> 6) for P010.
bool RenderYuv(const std::vector<uint32_t>& pixels, UINT width, UINT height, DXGI_FORMAT sourceFormat,
               uint32_t flags, YuvCodes* out) {
    ce::ComGuard<ID3D11Device> baseDevice;
    ce::ComGuard<ID3D11DeviceContext> context;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                 nullptr, 0, D3D11_SDK_VERSION, baseDevice.addressof(), nullptr,
                                 context.addressof())))
        return false;
    ce::ComGuard<ID3D11Device3> device;
    if (FAILED(baseDevice->QueryInterface(IID_PPV_ARGS(device.addressof()))))
        return false;

    D3D11_TEXTURE2D_DESC sourceDesc = {};
    sourceDesc.Width = width;
    sourceDesc.Height = height;
    sourceDesc.MipLevels = sourceDesc.ArraySize = sourceDesc.SampleDesc.Count = 1;
    sourceDesc.Format = sourceFormat;
    sourceDesc.Usage = D3D11_USAGE_IMMUTABLE;
    sourceDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sourceData = {pixels.data(), static_cast<UINT>(width * sizeof(uint32_t)), 0};
    ce::ComGuard<ID3D11Texture2D> source;
    if (FAILED(device->CreateTexture2D(&sourceDesc, &sourceData, source.addressof())))
        return false;

    const bool eightBit = (flags & kFlag8Bit) != 0;
    D3D11_TEXTURE2D_DESC outputDesc = sourceDesc;
    outputDesc.Format = eightBit ? DXGI_FORMAT_NV12 : DXGI_FORMAT_P010;
    outputDesc.Usage = D3D11_USAGE_DEFAULT;
    outputDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ce::ComGuard<ID3D11Texture2D> output;
    if (FAILED(device->CreateTexture2D(&outputDesc, nullptr, output.addressof())))
        return false;
    auto planeView = [&](DXGI_FORMAT format, UINT plane, ce::ComGuard<ID3D11RenderTargetView1>& view) {
        D3D11_RENDER_TARGET_VIEW_DESC1 desc = {};
        desc.Format = format;
        desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        desc.Texture2D.PlaneSlice = plane;
        return device->CreateRenderTargetView1(output.get(), &desc, view.addressof());
    };
    ce::ComGuard<ID3D11RenderTargetView1> lumaView;
    ce::ComGuard<ID3D11RenderTargetView1> chromaView;
    if (FAILED(planeView(eightBit ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R16_UNORM, 0, lumaView)) ||
        FAILED(planeView(eightBit ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_UNORM, 1, chromaView)))
        return false;
    ce::ComGuard<ID3D11ShaderResourceView> sourceView;
    if (FAILED(device->CreateShaderResourceView(source.get(), nullptr, sourceView.addressof())))
        return false;

    auto compile = [&](const char* entry, const char* target, ce::ComGuard<ID3DBlob>& blob) {
        ce::ComGuard<ID3DBlob> errors;
        const HRESULT hr = D3DCompile(ce::video_color::kRgbColorConversionShaderSource,
                                      sizeof(ce::video_color::kRgbColorConversionShaderSource) - 1, nullptr, nullptr,
                                      nullptr, entry, target, 0, 0, blob.addressof(), errors.addressof());
        EXPECT_TRUE(SUCCEEDED(hr)) << entry << ": "
                                   << (errors ? static_cast<const char*>(errors->GetBufferPointer()) : "");
        return hr;
    };
    ce::ComGuard<ID3DBlob> vsBlob;
    ce::ComGuard<ID3DBlob> yBlob;
    ce::ComGuard<ID3DBlob> uvBlob;
    if (FAILED(compile("VS_Main", "vs_4_0", vsBlob)) || FAILED(compile("PS_YuvY", "ps_4_0", yBlob)) ||
        FAILED(compile("PS_YuvUV", "ps_4_0", uvBlob)))
        return false;
    ce::ComGuard<ID3D11VertexShader> vs;
    ce::ComGuard<ID3D11PixelShader> yPs;
    ce::ComGuard<ID3D11PixelShader> uvPs;
    if (FAILED(device->CreateVertexShader(vsBlob->GetBufferPointer(), vsBlob->GetBufferSize(), nullptr,
                                          vs.addressof())) ||
        FAILED(device->CreatePixelShader(yBlob->GetBufferPointer(), yBlob->GetBufferSize(), nullptr,
                                         yPs.addressof())) ||
        FAILED(device->CreatePixelShader(uvBlob->GetBufferPointer(), uvBlob->GetBufferSize(), nullptr,
                                         uvPs.addressof())))
        return false;

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D11_SAMPLER_DESC samplerDesc = {};
    samplerDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;  // the encoder's hdrP010Sampler
    samplerDesc.AddressU = samplerDesc.AddressV = samplerDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
    ce::ComGuard<ID3D11SamplerState> sampler;
    if (FAILED(device->CreateSamplerState(&samplerDesc, sampler.addressof())))
        return false;
    struct Constants {
        uint32_t colorTransform;
        uint32_t yuvFlags;
        float invWidth;
        float invHeight;
        float padding[4];
    };
    const Constants constants = {0, flags, 1.0f / static_cast<float>(width), 1.0f / static_cast<float>(height), {}};
    D3D11_BUFFER_DESC cbDesc = {static_cast<UINT>(sizeof(constants)), D3D11_USAGE_IMMUTABLE,
                                D3D11_BIND_CONSTANT_BUFFER, 0, 0, 0};
    D3D11_SUBRESOURCE_DATA cbData = {&constants, 0, 0};
    ce::ComGuard<ID3D11Buffer> cb;
    if (FAILED(device->CreateBuffer(&cbDesc, &cbData, cb.addressof())))
        return false;

    ID3D11ShaderResourceView* srv = sourceView.get();
    ID3D11SamplerState* samplerRaw = sampler.get();
    ID3D11Buffer* cbRaw = cb.get();
    context->VSSetShader(vs.get(), nullptr, 0);
    context->PSSetShaderResources(0, 1, &srv);
    context->PSSetSamplers(0, 1, &samplerRaw);
    context->PSSetConstantBuffers(0, 1, &cbRaw);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    D3D11_VIEWPORT viewport = {0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, 1.0f};
    ID3D11RenderTargetView* rtv = lumaView.get();
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &rtv, nullptr);
    context->PSSetShader(yPs.get(), nullptr, 0);
    context->Draw(3, 0);
    const auto chromaWidth = width / 2;  // 4:2:0 chroma plane, rounded down like the texture
    const auto chromaHeight = height / 2;
    viewport.Width = static_cast<float>(chromaWidth);
    viewport.Height = static_cast<float>(chromaHeight);
    rtv = chromaView.get();
    context->RSSetViewports(1, &viewport);
    context->OMSetRenderTargets(1, &rtv, nullptr);
    context->PSSetShader(uvPs.get(), nullptr, 0);
    context->Draw(3, 0);
    ID3D11RenderTargetView* nullRtv = nullptr;
    context->OMSetRenderTargets(1, &nullRtv, nullptr);

    D3D11_TEXTURE2D_DESC stagingDesc = outputDesc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ce::ComGuard<ID3D11Texture2D> staging;
    if (FAILED(device->CreateTexture2D(&stagingDesc, nullptr, staging.addressof())))
        return false;
    context->CopyResource(staging.get(), output.get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    const auto* bytes = static_cast<const uint8_t*>(mapped.pData);
    auto code = [&](size_t offset) -> uint16_t {
        if (eightBit)
            return bytes[offset];
        return static_cast<uint16_t>(*reinterpret_cast<const uint16_t*>(bytes + offset) >> 6);
    };
    const size_t sampleBytes = eightBit ? 1 : 2;
    out->luma.clear();
    out->chroma.clear();
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x)
            out->luma.push_back(code(y * mapped.RowPitch + x * sampleBytes));
    const size_t chromaBase = static_cast<size_t>(mapped.RowPitch) * height;
    for (UINT y = 0; y < height / 2; ++y)
        for (UINT x = 0; x < width; ++x)  // width/2 pairs of Cb,Cr
            out->chroma.push_back(code(chromaBase + y * mapped.RowPitch + x * sampleBytes));
    context->Unmap(staging.get(), 0);
    return true;
}

// 0xAABBGGRR in memory order R,G,B,A for R8G8B8A8; B8G8R8A8 swaps R and B bytes.
constexpr uint32_t Rgba(uint8_t r, uint8_t g, uint8_t b) {
    return 0xff000000u | (uint32_t{b} << 16) | (uint32_t{g} << 8) | r;
}
constexpr uint32_t Bgra(uint8_t r, uint8_t g, uint8_t b) {
    return 0xff000000u | (uint32_t{r} << 16) | (uint32_t{g} << 8) | b;
}

}  // namespace

TEST(SdrYuvShaderTest, Bt709LimitedP010CodesForWhiteBlackAndRed) {
    YuvCodes codes;
    const std::vector<uint32_t> white(4, Rgba(255, 255, 255));
    ASSERT_TRUE(RenderYuv(white, 2, 2, DXGI_FORMAT_R8G8B8A8_UNORM, 0, &codes));
    EXPECT_EQ(codes.luma, std::vector<uint16_t>(4, 940));
    EXPECT_EQ(codes.chroma, (std::vector<uint16_t>{512, 512}));

    const std::vector<uint32_t> black(4, Rgba(0, 0, 0));
    ASSERT_TRUE(RenderYuv(black, 2, 2, DXGI_FORMAT_R8G8B8A8_UNORM, 0, &codes));
    EXPECT_EQ(codes.luma, std::vector<uint16_t>(4, 64));
    EXPECT_EQ(codes.chroma, (std::vector<uint16_t>{512, 512}));

    // Y = 64 + 876 * 0.2126, Cb = 512 - 896 * 0.2126 / 1.8556, Cr = 512 + 896 * 0.5.
    const std::vector<uint32_t> red(4, Rgba(255, 0, 0));
    ASSERT_TRUE(RenderYuv(red, 2, 2, DXGI_FORMAT_R8G8B8A8_UNORM, 0, &codes));
    EXPECT_EQ(codes.luma, std::vector<uint16_t>(4, 250));
    EXPECT_EQ(codes.chroma, (std::vector<uint16_t>{409, 960}));
}

TEST(SdrYuvShaderTest, RgbaAndBgraSourcesProduceTheSameCodesWithoutASwapPass) {
    YuvCodes rgba;
    YuvCodes bgra;
    const std::vector<uint32_t> rgbaPixels = {Rgba(200, 30, 90), Rgba(10, 220, 40), Rgba(70, 80, 250),
                                              Rgba(128, 128, 128)};
    const std::vector<uint32_t> bgraPixels = {Bgra(200, 30, 90), Bgra(10, 220, 40), Bgra(70, 80, 250),
                                              Bgra(128, 128, 128)};
    ASSERT_TRUE(RenderYuv(rgbaPixels, 2, 2, DXGI_FORMAT_R8G8B8A8_UNORM, kFlagFullRange, &rgba));
    ASSERT_TRUE(RenderYuv(bgraPixels, 2, 2, DXGI_FORMAT_B8G8R8A8_UNORM, kFlagFullRange, &bgra));
    EXPECT_EQ(rgba.luma, bgra.luma);
    EXPECT_EQ(rgba.chroma, bgra.chroma);
}

TEST(SdrYuvShaderTest, FullRangeNv12UsesEveryCode) {
    YuvCodes codes;
    const std::vector<uint32_t> pixels = {Rgba(255, 255, 255), Rgba(0, 0, 0), Rgba(255, 0, 0), Rgba(128, 128, 128)};
    ASSERT_TRUE(RenderYuv(pixels, 2, 2, DXGI_FORMAT_R8G8B8A8_UNORM, kFlag8Bit | kFlagFullRange, &codes));
    // Y = 255 * (0.2126 R + 0.7152 G + 0.0722 B).
    EXPECT_EQ(codes.luma, (std::vector<uint16_t>{255, 0, 54, 128}));
}

TEST(SdrYuvShaderTest, Bt2020MatrixIsSelectable) {
    YuvCodes codes;
    const std::vector<uint32_t> red(4, Rgba(255, 0, 0));
    ASSERT_TRUE(RenderYuv(red, 2, 2, DXGI_FORMAT_R8G8B8A8_UNORM, kFlagBt2020, &codes));
    // Y = 64 + 876 * 0.2627, Cr = 512 + 896 * 0.5.
    EXPECT_EQ(codes.luma, std::vector<uint16_t>(4, 294));
    EXPECT_EQ(codes.chroma[1], 960u);
}

TEST(SdrYuvShaderTest, ChromaIsLeftSitedOnTheEvenLumaColumn) {
    // One red column at x=0 of a 4x2 frame. Left siting centres chroma column 0
    // on luma column 0 with a 1-2-1 low-pass (the clamped x=-1 is red too):
    // R' = 0.75 there, and chroma column 1 (luma columns 1..3) sees no red. A
    // centred 4:2:0 filter would instead give R' = 0.5 for column 0.
    const uint32_t red = Rgba(255, 0, 0);
    const uint32_t black = Rgba(0, 0, 0);
    const std::vector<uint32_t> pixels = {red, black, black, black, red, black, black, black};
    YuvCodes codes;
    ASSERT_TRUE(RenderYuv(pixels, 4, 2, DXGI_FORMAT_R8G8B8A8_UNORM, kFlag8Bit | kFlagFullRange, &codes));
    ASSERT_EQ(codes.chroma.size(), 4u);
    // R'=0.75: Y = 0.15945, Cb = 128 - 255 * 0.15945 / 1.8556, Cr = 128 + 255 * 0.375.
    EXPECT_EQ(codes.chroma[0], 106u);
    EXPECT_EQ(codes.chroma[1], 224u);
    EXPECT_EQ(codes.chroma[2], 128u);
    EXPECT_EQ(codes.chroma[3], 128u);
}

TEST(SdrYuvShaderTest, EncoderUsesTheShaderForSdrOutputAndKeepsNoSwapPassForIt) {
    const auto video = std::filesystem::current_path() / "mediaengine" / "video";
    const std::string convert = ce::test_source::ReadLogicalSource(video / "video_encoder_convert_bgra.cpp");
    const std::string yuv = ce::test_source::ReadLogicalSource(video / "video_encoder_convert_yuv.cpp");
    ASSERT_FALSE(convert.empty());
    ASSERT_FALSE(yuv.empty());
    const size_t decide = convert.find("CanConvertSdrYuvWithShader(preparedCursorSource, outputTexture");
    const size_t swap = convert.find("srcDesc.Format == DXGI_FORMAT_R8G8B8A8_UNORM && !useSdrShader", decide);
    const size_t shader = convert.find("ConvertSdrRgbToYuv(vpInputTexture, outputTexture, outputArraySlice");
    const size_t blt = convert.find("VideoProcessorBlt(");
    ASSERT_NE(decide, std::string::npos);
    ASSERT_NE(swap, std::string::npos);
    ASSERT_NE(shader, std::string::npos);
    ASSERT_NE(blt, std::string::npos);
    EXPECT_LT(shader, blt);
    EXPECT_EQ(yuv.find("->VideoProcessorBlt("), std::string::npos);
    EXPECT_EQ(yuv.find("Flush("), std::string::npos);
    EXPECT_NE(yuv.find("sdrYuvLumaPS"), std::string::npos);
    EXPECT_NE(yuv.find("sdrYuvChromaPS"), std::string::npos);
}

TEST(SdrYuvShaderTest, VideoProcessorObjectsAreCreatedOnlyOnTheFallbackPath) {
    // The shader path needs no VideoProcessor: InitVideoProcessor only sets the
    // conversion geometry, and the enumerator, processor and BGRA staging
    // texture (~33 MB at 4K) appear only once a frame reaches the Blt.
    const auto video = std::filesystem::current_path() / "mediaengine" / "video";
    const std::string convert = ce::test_source::ReadLogicalSource(video / "video_encoder_convert_bgra.cpp");
    const std::string conversion = ce::test_source::ReadLogicalSource(video / "video_encoder_conversion.cpp");
    ASSERT_FALSE(convert.empty());
    ASSERT_FALSE(conversion.empty());
    const size_t init = conversion.find("bool VideoEncoder::InitVideoProcessor()");
    const size_t objects = conversion.find("bool VideoEncoder::EnsureVideoProcessorObjects()");
    ASSERT_NE(init, std::string::npos);
    ASSERT_NE(objects, std::string::npos);
    ASSERT_LT(init, objects);
    const std::string initBody = conversion.substr(init, objects - init);
    EXPECT_EQ(initBody.find("CreateVideoProcessorEnumerator"), std::string::npos);
    EXPECT_EQ(initBody.find("ID3D11VideoDevice"), std::string::npos);
    EXPECT_EQ(initBody.find("bgraStagingTexture"), std::string::npos);
    EXPECT_NE(conversion.find("CreateVideoProcessorEnumerator", objects), std::string::npos);

    const size_t shader = convert.find("ConvertSdrRgbToYuv(vpInputTexture, outputTexture, outputArraySlice");
    const size_t hdr = convert.find("ConvertHdrRgb10ToP010(vpInputTexture, outputTexture, outputArraySlice");
    const size_t ensure = convert.find("EnsureVideoProcessorObjects()");
    const size_t outputView = convert.find("CreateVideoProcessorOutputView(");
    const size_t blt = convert.find("VideoProcessorBlt(");
    ASSERT_NE(shader, std::string::npos);
    ASSERT_NE(hdr, std::string::npos);
    ASSERT_NE(ensure, std::string::npos);
    ASSERT_NE(outputView, std::string::npos);
    EXPECT_LT(shader, ensure);
    EXPECT_LT(hdr, ensure);
    EXPECT_LT(ensure, outputView);
    EXPECT_LT(outputView, blt);
}
