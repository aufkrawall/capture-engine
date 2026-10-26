#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <d3dcompiler.h>
#include <mmsystem.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <vector>

#include "common/platform/raii_helpers.h"
#include "mediaengine/video/frame_scaler.h"
#include "mediaengine/video/rgb_yuv_draw.h"
#include "mediaengine/video/runtime_shader_compiler.h"
#include "mediaengine/video/sampleable_source_copy.h"
#include "mediaengine/video/video_color_conversion_shader.h"

// The shader scaler (frame_scaler.h) on the real adapter: values stay exact
// where nothing should change, edges sharpen without ringing, detail above the
// output's Nyquist frequency does not alias, and every output layout (RGB,
// 4:4:4, 4:2:2, 4:2:0; 8/10-bit; SDR and HDR) receives the scaled image.

namespace {

using ce::ComGuard;
using ce::video_color::ChromaLayout;

uint32_t planBuilds = 0;

ce::video_scale::ScalePlan CountPlanBuild(uint32_t inWidth, uint32_t inHeight, uint32_t outWidth,
                                          uint32_t outHeight, const ce::video_scale::KernelSpec& kernel,
                                          uint32_t sourceBits, uint32_t outputBits) {
    ++planBuilds;
    return ce::video_scale::PlanScale(inWidth, inHeight, outWidth, outHeight, kernel, sourceBits, outputBits);
}

struct Gpu {
    ComGuard<ID3D11Device> device;
    ComGuard<ID3D11DeviceContext> context;
    bool Create() {
        return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                           nullptr, 0, D3D11_SDK_VERSION, device.addressof(), nullptr,
                                           context.addressof()));
    }
};

// One gray level per pixel, 0..1, row-major.
using Image = std::vector<float>;

ComGuard<ID3D11Texture2D> MakeSource(Gpu& gpu, const Image& gray, UINT width, UINT height, DXGI_FORMAT format,
                                     float red = -1.0f, float blue = -1.0f) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = width;
    desc.Height = height;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.Usage = D3D11_USAGE_IMMUTABLE;
    desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    std::vector<uint32_t> texels(static_cast<size_t>(width) * height);
    for (size_t i = 0; i < texels.size(); ++i) {
        const float g = gray[i];
        const float r = red >= 0.0f ? red : g;
        const float b = blue >= 0.0f ? blue : g;
        if (format == DXGI_FORMAT_R10G10B10A2_UNORM) {
            auto code = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 1023.0f)); };
            texels[i] = code(r) | (code(g) << 10) | (code(b) << 20) | (3u << 30);
        } else {  // B8G8R8A8: B in the low byte
            auto code = [](float v) { return static_cast<uint32_t>(std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f)); };
            texels[i] = code(b) | (code(g) << 8) | (code(r) << 16) | (255u << 24);
        }
    }
    D3D11_SUBRESOURCE_DATA data = {texels.data(), width * 4u, 0};
    ComGuard<ID3D11Texture2D> texture;
    EXPECT_TRUE(SUCCEEDED(gpu.device->CreateTexture2D(&desc, &data, texture.addressof())));
    return texture;
}

ComGuard<ID3D11ShaderResourceView> ViewOf(Gpu& gpu, ID3D11Texture2D* texture) {
    ComGuard<ID3D11ShaderResourceView> view;
    EXPECT_TRUE(SUCCEEDED(gpu.device->CreateShaderResourceView(texture, nullptr, view.addressof())));
    return view;
}

// Channel `channel` of every texel as 0..1 (RGB10A2, RGBA16, BGRA8, R8/R8G8, R16/R16G16).
std::vector<float> ReadChannel(Gpu& gpu, ID3D11Resource* resource, UINT channel) {
    ComGuard<ID3D11Texture2D> texture;
    resource->QueryInterface(IID_PPV_ARGS(texture.addressof()));
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    D3D11_TEXTURE2D_DESC stagingDesc = desc;
    stagingDesc.Usage = D3D11_USAGE_STAGING;
    stagingDesc.BindFlags = 0;
    stagingDesc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComGuard<ID3D11Texture2D> staging;
    EXPECT_TRUE(SUCCEEDED(gpu.device->CreateTexture2D(&stagingDesc, nullptr, staging.addressof())));
    gpu.context->CopyResource(staging.get(), texture.get());
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    EXPECT_TRUE(SUCCEEDED(gpu.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)));
    std::vector<float> values;
    for (UINT y = 0; y < desc.Height; ++y) {
        const auto* row = static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch;
        for (UINT x = 0; x < desc.Width; ++x) {
            switch (desc.Format) {
                case DXGI_FORMAT_R10G10B10A2_UNORM: {
                    uint32_t texel = 0;
                    std::memcpy(&texel, row + x * 4u, 4);
                    values.push_back(static_cast<float>((texel >> (10u * channel)) & 1023u) / 1023.0f);
                    break;
                }
                case DXGI_FORMAT_R16G16B16A16_UNORM:
                case DXGI_FORMAT_R16_UNORM:
                case DXGI_FORMAT_R16G16_UNORM: {
                    const UINT components = desc.Format == DXGI_FORMAT_R16_UNORM     ? 1u
                                            : desc.Format == DXGI_FORMAT_R16G16_UNORM ? 2u
                                                                                      : 4u;
                    uint16_t value = 0;
                    std::memcpy(&value, row + (x * components + channel) * 2u, 2);
                    values.push_back(static_cast<float>(value) / 65535.0f);
                    break;
                }
                case DXGI_FORMAT_B8G8R8A8_UNORM:  // channel 0 = R
                    values.push_back(static_cast<float>(row[x * 4u + (channel == 0 ? 2u : channel == 2 ? 0u : 1u)]) /
                                     255.0f);
                    break;
                default: {  // R8 / R8G8
                    const UINT components = desc.Format == DXGI_FORMAT_R8_UNORM ? 1u : 2u;
                    values.push_back(static_cast<float>(row[x * components + channel]) / 255.0f);
                    break;
                }
            }
        }
    }
    gpu.context->Unmap(staging.get(), 0);
    return values;
}

struct Scaled {
    std::vector<float> green;
    UINT width = 0;
    UINT height = 0;
};

Scaled ScaleGray(Gpu& gpu, const Image& gray, UINT width, UINT height, UINT outWidth, UINT outHeight,
                 uint32_t lobes, float gain, DXGI_FORMAT format = DXGI_FORMAT_R10G10B10A2_UNORM) {
    ComGuard<ID3D11Texture2D> source = MakeSource(gpu, gray, width, height, format);
    ComGuard<ID3D11ShaderResourceView> view = ViewOf(gpu, source.get());
    ce::video_scale::FrameScaler scaler;
    ce::video_scale::FrameScaler::Request request;
    request.source = view.get();
    request.sourceWidth = width;
    request.sourceHeight = height;
    request.outputWidth = outWidth;
    request.outputHeight = outHeight;
    request.kernel.lobes = lobes;
    request.kernel.sideLobeGain = gain;
    request.sourceBits = 10;
    request.outputBits = 10;
    ID3D11ShaderResourceView* output = nullptr;
    EXPECT_TRUE(scaler.Scale(gpu.device.get(), gpu.context.get(), request, &output));
    Scaled scaled;
    if (!output)
        return scaled;
    ComGuard<ID3D11Resource> resource;
    output->GetResource(resource.addressof());
    scaled.green = ReadChannel(gpu, resource.get(), 1);
    scaled.width = outWidth;
    scaled.height = outHeight;
    return scaled;
}

Image Fill(UINT width, UINT height, const std::function<float(UINT, UINT)>& value) {
    Image image(static_cast<size_t>(width) * height);
    for (UINT y = 0; y < height; ++y)
        for (UINT x = 0; x < width; ++x)
            image[static_cast<size_t>(y) * width + x] = value(x, y);
    return image;
}

TEST(FrameScalerGpuTest, ReusesCpuWeightTablesUntilThePlanChanges) {
    Gpu gpu;
    ASSERT_TRUE(gpu.Create());
    auto source = MakeSource(gpu, Image(32u * 16u, 0.5f), 32, 16, DXGI_FORMAT_B8G8R8A8_UNORM);
    auto view = ViewOf(gpu, source.get());
    ce::video_scale::FrameScaler scaler(&CountPlanBuild);
    ce::video_scale::FrameScaler::Request req;
    req.source = view.get();
    req.sourceWidth = 32;
    req.sourceHeight = 16;
    req.outputWidth = 16;
    req.outputHeight = 8;
    planBuilds = 0;
    ID3D11ShaderResourceView* scaled = nullptr;
    for (int i = 0; i < 4; ++i)
        ASSERT_TRUE(scaler.Scale(gpu.device.get(), gpu.context.get(), req, &scaled));
    EXPECT_EQ(planBuilds, 1u);
    req.outputWidth = 12;
    ASSERT_TRUE(scaler.Scale(gpu.device.get(), gpu.context.get(), req, &scaled));
    EXPECT_EQ(planBuilds, 2u);
    req.kernel = ce::video_scale::KernelSpecFromConfig("best", 50);
    ASSERT_TRUE(scaler.Scale(gpu.device.get(), gpu.context.get(), req, &scaled));
    EXPECT_EQ(planBuilds, 3u);
    req.sourceBits = req.outputBits = 10;
    ASSERT_TRUE(scaler.Scale(gpu.device.get(), gpu.context.get(), req, &scaled));
    EXPECT_EQ(planBuilds, 4u);
    req.outputBits = 8;
    ASSERT_TRUE(scaler.Scale(gpu.device.get(), gpu.context.get(), req, &scaled));
    EXPECT_EQ(planBuilds, 5u);
    req.sourceBits = 8;  // same effective intermediate precision
    ASSERT_TRUE(scaler.Scale(gpu.device.get(), gpu.context.get(), req, &scaled));
    EXPECT_EQ(planBuilds, 5u);
    scaler.Reset();
    ASSERT_TRUE(scaler.Scale(gpu.device.get(), gpu.context.get(), req, &scaled));
    EXPECT_EQ(planBuilds, 6u);
}

TEST(FrameScalerGpuTest, AFlatImageStaysExactAtEveryRatio) {
    Gpu gpu;
    ASSERT_TRUE(gpu.Create());
    const Image flat = Fill(97, 61, [](UINT, UINT) { return 600.0f / 1023.0f; });
    const UINT sizes[][2] = {{40, 26}, {250, 160}, {97, 30}, {33, 61}};
    for (const auto& size : sizes) {
        for (uint32_t lobes : {2u, 3u}) {
            const Scaled scaled = ScaleGray(gpu, flat, 97, 61, size[0], size[1], lobes, 1.0f);
            ASSERT_EQ(scaled.green.size(), static_cast<size_t>(size[0]) * size[1]);
            for (float v : scaled.green)
                ASSERT_NEAR(v * 1023.0f, 600.0f, 0.02f) << size[0] << "x" << size[1] << " lobes=" << lobes;
        }
    }
}

// Edge between two mid levels, so UNORM storage cannot hide an overshoot.
TEST(FrameScalerGpuTest, EdgesSharpenWithoutRinging) {
    Gpu gpu;
    ASSERT_TRUE(gpu.Create());
    const float lo = 0.25f;
    const float hi = 0.75f;
    const Image edge = Fill(48, 8, [&](UINT x, UINT) { return x < 24 ? lo : hi; });
    for (const UINT outWidth : {192u, 83u, 20u}) {
        int transitional[2] = {0, 0};
        for (int sharp = 0; sharp < 2; ++sharp) {
            const Scaled scaled = ScaleGray(gpu, edge, 48, 8, outWidth, 8, 3, sharp ? 1.0f : 0.0f);
            ASSERT_FALSE(scaled.green.empty());
            for (float v : scaled.green) {
                ASSERT_GE(v, lo - 1.0f / 1023.0f) << outWidth;
                ASSERT_LE(v, hi + 1.0f / 1023.0f) << outWidth;
                transitional[sharp] += (v > lo + 0.05f && v < hi - 0.05f) ? 1 : 0;
            }
        }
        // Side lobes steepen the edge: never more transitional pixels than without them.
        EXPECT_LE(transitional[1], transitional[0]) << outWidth;
        std::printf("[Scaler] edge 48->%u transitional rows-px: sharp=%d soft=%d\n", outWidth, transitional[1],
                    transitional[0]);
    }
}

TEST(FrameScalerGpuTest, DetailAboveTheOutputNyquistDoesNotAlias) {
    Gpu gpu;
    ASSERT_TRUE(gpu.Create());
    // A one-pixel checkerboard has no frequency an output of under half the
    // size can represent: a correct downscale is flat mid-gray, an aliasing
    // one shows moire.
    const Image checker = Fill(320, 180, [](UINT x, UINT y) { return ((x + y) & 1u) ? 0.75f : 0.25f; });
    const UINT sizes[][2] = {{160, 90}, {121, 68}, {64, 36}};
    for (const auto& size : sizes) {
        for (uint32_t lobes : {2u, 3u}) {
            const Scaled scaled = ScaleGray(gpu, checker, 320, 180, size[0], size[1], lobes, 1.0f);
            ASSERT_FALSE(scaled.green.empty());
            float worst = 0.0f;
            for (float v : scaled.green)
                worst = std::max(worst, std::fabs(v - 0.5f));
            std::printf("[Scaler] checker 320x180->%ux%u lanczos%u: max |v-0.5|=%.4f\n", size[0], size[1], lobes,
                        worst);
            EXPECT_LT(worst, 0.03f) << size[0] << "x" << size[1] << " lobes=" << lobes;
        }
    }
}

TEST(FrameScalerGpuTest, UpscaledRampStaysMonotonicAndLinear) {
    Gpu gpu;
    ASSERT_TRUE(gpu.Create());
    const Image ramp = Fill(64, 4, [](UINT x, UINT) { return static_cast<float>(x) / 63.0f; });
    const Scaled scaled = ScaleGray(gpu, ramp, 64, 4, 160, 4, 3, 1.0f);
    ASSERT_EQ(scaled.green.size(), 640u);
    for (UINT x = 1; x < 160; ++x)
        ASSERT_GE(scaled.green[x], scaled.green[x - 1] - 1.0f / 65535.0f) << x;
    for (UINT x = 8; x < 152; ++x) {
        const float sourceX = (static_cast<float>(x) + 0.5f) * 64.0f / 160.0f - 0.5f;
        EXPECT_NEAR(scaled.green[x], sourceX / 63.0f, 0.004f) << x;
    }
}

struct ConversionRig {
    ComGuard<ID3D11VertexShader> vs;
    ComGuard<ID3D11PixelShader> ps[5];
    ComGuard<ID3D11SamplerState> pointSampler;
    ComGuard<ID3D11SamplerState> linearSampler;
    ComGuard<ID3D11Buffer> constants;

    bool Init(ID3D11Device* device) {
        const ce::video_color::RuntimeShaderEntry entries[] = {
            {"VS_Main", "vs_4_0"},   {"PS_Main", "ps_4_0"}, {"PS_P010Y", "ps_4_0"},
            {"PS_P010UV", "ps_4_0"}, {"PS_YuvY", "ps_4_0"}, {"PS_YuvUV", "ps_4_0"}};
        const bool compiled = ce::video_color::PrepareShaderBytecode(
            ce::video_color::kRgbColorConversionShaderSource,
            std::strlen(ce::video_color::kRgbColorConversionShaderSource), entries, 6, "RGBConvert",
            [&](const ce::video_color::ShaderBytecodeView* shaders) {
                if (FAILED(device->CreateVertexShader(shaders[0].data, shaders[0].size, nullptr, vs.addressof())))
                    return false;
                for (int i = 0; i < 5; ++i) {
                    if (FAILED(device->CreatePixelShader(shaders[i + 1].data, shaders[i + 1].size, nullptr,
                                                         ps[i].addressof())))
                        return false;
                }
                return true;
            });
        if (!compiled)
            return false;
        // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
        D3D11_SAMPLER_DESC sampDesc = {};
        sampDesc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
        sampDesc.AddressU = sampDesc.AddressV = sampDesc.AddressW = D3D11_TEXTURE_ADDRESS_CLAMP;
        if (FAILED(device->CreateSamplerState(&sampDesc, pointSampler.addressof())))
            return false;
        sampDesc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        if (FAILED(device->CreateSamplerState(&sampDesc, linearSampler.addressof())))
            return false;
        D3D11_BUFFER_DESC cbDesc = {};
        cbDesc.ByteWidth = 32;
        cbDesc.Usage = D3D11_USAGE_DYNAMIC;
        cbDesc.BindFlags = D3D11_BIND_CONSTANT_BUFFER;
        cbDesc.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        return SUCCEEDED(device->CreateBuffer(&cbDesc, nullptr, constants.addressof()));
    }

    ce::video_color::RgbYuvDrawKernels Kernels() const {
        ce::video_color::RgbYuvDrawKernels k;
        k.vertexShader = vs.get();
        k.normalizeShader = ps[0].get();
        k.p010LumaShader = ps[1].get();
        k.p010ChromaShader = ps[2].get();
        k.yuvLumaShader = ps[3].get();
        k.yuvChromaShader = ps[4].get();
        k.pointSampler = pointSampler.get();
        k.linearSampler = linearSampler.get();
        k.constants = constants.get();
        return k;
    }
};

TEST(FrameScalerGpuTest, ScalesDirectlyIntoCallerRgbTargetEightAndTenBit) {
    Gpu gpu;
    ASSERT_TRUE(gpu.Create());
    const Image green = Fill(40, 20, [](UINT, UINT) { return 0.5f; });
    for (DXGI_FORMAT format : {DXGI_FORMAT_B8G8R8A8_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM}) {
        const uint32_t bits = format == DXGI_FORMAT_R10G10B10A2_UNORM ? 10u : 8u;
        ComGuard<ID3D11Texture2D> source = MakeSource(gpu, green, 40, 20, format, 0.25f, 0.75f);
        ComGuard<ID3D11ShaderResourceView> view = ViewOf(gpu, source.get());
        D3D11_TEXTURE2D_DESC outDesc = {};
        outDesc.Width = 24;
        outDesc.Height = 12;
        outDesc.MipLevels = outDesc.ArraySize = outDesc.SampleDesc.Count = 1;
        outDesc.Format = format;
        outDesc.Usage = D3D11_USAGE_DEFAULT;
        outDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComGuard<ID3D11Texture2D> targetTex;
        ComGuard<ID3D11RenderTargetView> targetRtv;
        ASSERT_TRUE(SUCCEEDED(gpu.device->CreateTexture2D(&outDesc, nullptr, targetTex.addressof())));
        ASSERT_TRUE(SUCCEEDED(gpu.device->CreateRenderTargetView(targetTex.get(), nullptr, targetRtv.addressof())));

        ce::video_scale::FrameScaler scaler;
        ce::video_scale::FrameScaler::Request req;
        req.source = view.get();
        req.sourceWidth = 40;
        req.sourceHeight = 20;
        req.outputWidth = 24;
        req.outputHeight = 12;
        req.kernel = ce::video_scale::KernelSpecFromConfig("best", 100);
        req.sourceBits = bits;
        req.outputBits = bits;
        req.target = targetRtv.get();
        ID3D11ShaderResourceView* unused = nullptr;
        ASSERT_TRUE(scaler.Scale(gpu.device.get(), gpu.context.get(), req, &unused));
        EXPECT_EQ(unused, nullptr);
        const auto r = ReadChannel(gpu, targetTex.get(), 0);
        const auto g = ReadChannel(gpu, targetTex.get(), 1);
        const auto b = ReadChannel(gpu, targetTex.get(), 2);
        ASSERT_EQ(r.size(), 24u * 12u);
        for (size_t i = 0; i < r.size(); ++i) {
            EXPECT_NEAR(r[i], 0.25f, 1.5f / 255.0f);
            EXPECT_NEAR(g[i], 0.50f, 1.5f / 255.0f);
            EXPECT_NEAR(b[i], 0.75f, 1.5f / 255.0f);
        }
    }
}

TEST(FrameScalerGpuTest, ScalesIntoEveryChromaLayoutEightAndTenBitSdrAndHdr) {
    Gpu gpu;
    ASSERT_TRUE(gpu.Create());
    ConversionRig rig;
    ASSERT_TRUE(rig.Init(gpu.device.get()));
    const Image green = Fill(32, 16, [](UINT, UINT) { return 0.5f; });
    ComGuard<ID3D11Texture2D> source = MakeSource(gpu, green, 32, 16, DXGI_FORMAT_R10G10B10A2_UNORM, 0.8f, 0.2f);
    ComGuard<ID3D11ShaderResourceView> sourceView = ViewOf(gpu, source.get());

    ce::video_scale::FrameScaler scaler;
    ce::video_scale::FrameScaler::Request req;
    req.source = sourceView.get();
    req.sourceWidth = 32;
    req.sourceHeight = 16;
    req.outputWidth = 16;
    req.outputHeight = 8;
    req.kernel = ce::video_scale::KernelSpecFromConfig("normal", 100);
    req.sourceBits = 10;
    req.outputBits = 10;
    ID3D11ShaderResourceView* scaledView = nullptr;
    ASSERT_TRUE(scaler.Scale(gpu.device.get(), gpu.context.get(), req, &scaledView));
    ASSERT_NE(scaledView, nullptr);

    for (ChromaLayout layout : {ChromaLayout::k420, ChromaLayout::k422, ChromaLayout::k444}) {
        const UINT cw = ce::video_color::ChromaPlaneWidth(layout, 16);
        const UINT ch = ce::video_color::ChromaPlaneHeight(layout, 8);
        for (int mode = 0; mode < 3; ++mode) {  // 0: 8-bit SDR, 1: 10-bit SDR, 2: 10-bit HDR
            const bool eightBit = mode == 0;
            const bool hdr = mode == 2;
            D3D11_TEXTURE2D_DESC lumaDesc = {};
            lumaDesc.Width = 16;
            lumaDesc.Height = 8;
            lumaDesc.MipLevels = lumaDesc.ArraySize = lumaDesc.SampleDesc.Count = 1;
            lumaDesc.Format = eightBit ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R16_UNORM;
            lumaDesc.Usage = D3D11_USAGE_DEFAULT;
            lumaDesc.BindFlags = D3D11_BIND_RENDER_TARGET;
            D3D11_TEXTURE2D_DESC chromaDesc = lumaDesc;
            chromaDesc.Width = cw;
            chromaDesc.Height = ch;
            chromaDesc.Format = eightBit ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_UNORM;
            ComGuard<ID3D11Texture2D> lumaTex;
            ComGuard<ID3D11Texture2D> chromaTex;
            ComGuard<ID3D11RenderTargetView> lumaRtv;
            ComGuard<ID3D11RenderTargetView> chromaRtv;
            ASSERT_TRUE(SUCCEEDED(gpu.device->CreateTexture2D(&lumaDesc, nullptr, lumaTex.addressof())));
            ASSERT_TRUE(SUCCEEDED(gpu.device->CreateTexture2D(&chromaDesc, nullptr, chromaTex.addressof())));
            ASSERT_TRUE(SUCCEEDED(gpu.device->CreateRenderTargetView(lumaTex.get(), nullptr, lumaRtv.addressof())));
            ASSERT_TRUE(SUCCEEDED(gpu.device->CreateRenderTargetView(chromaTex.get(), nullptr, chromaRtv.addressof())));

            const ce::video_color::YuvPlaneTargets targets = {lumaRtv.get(), chromaRtv.get(), 16, 8, layout};
            const bool drawn =
                hdr ? ce::video_color::DrawHdrRgb10ToP010(gpu.context.get(), rig.Kernels(), scaledView, targets, false)
                    : ce::video_color::DrawSdrRgbToYuv(gpu.context.get(), rig.Kernels(), scaledView, targets,
                                                       ce::video_color::SdrYuvFlags(eightBit, false, false));
            ASSERT_TRUE(drawn);
            const auto yPlane = ReadChannel(gpu, lumaTex.get(), 0);
            const auto cbPlane = ReadChannel(gpu, chromaTex.get(), 0);
            const auto crPlane = ReadChannel(gpu, chromaTex.get(), 1);
            ASSERT_EQ(yPlane.size(), 16u * 8u);
            ASSERT_EQ(cbPlane.size(), static_cast<size_t>(cw) * ch);
            ASSERT_EQ(crPlane.size(), static_cast<size_t>(cw) * ch);
            const float toCode = eightBit ? 255.0f : 65535.0f / 64.0f;
            for (float v : yPlane)
                EXPECT_NEAR(v * toCode, yPlane[0] * toCode, 1.0f);
            for (float v : cbPlane)
                EXPECT_NEAR(v * toCode, cbPlane[0] * toCode, 1.0f);
            for (float v : crPlane)
                EXPECT_NEAR(v * toCode, crPlane[0] * toCode, 1.0f);
            // R > G > B -> Cr above neutral, Cb below neutral.
            const float neutral = eightBit ? 128.0f : 512.0f;
            EXPECT_LT(cbPlane[0] * toCode, neutral - 10.0f);
            EXPECT_GT(crPlane[0] * toCode, neutral + 10.0f);
        }
    }
}

TEST(FrameScalerGpuTest, SampleableSourceCopyHandlesUnsampleableAndMultisampledInputs) {
    Gpu gpu;
    ASSERT_TRUE(gpu.Create());
    SampleableSourceCopy copy;
    ce::video_scale::FrameScaler scaler;
    for (UINT samples : {1u, 4u}) {
        UINT qualityLevels = 0;
        if (samples > 1 &&
            (FAILED(gpu.device->CheckMultisampleQualityLevels(DXGI_FORMAT_B8G8R8A8_UNORM, samples, &qualityLevels)) ||
             qualityLevels == 0))
            continue;
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = 32;
        desc.Height = 16;
        desc.MipLevels = desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = samples;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;  // no D3D11_BIND_SHADER_RESOURCE
        ComGuard<ID3D11Texture2D> rawSource;
        ComGuard<ID3D11RenderTargetView> rawRtv;
        ASSERT_TRUE(SUCCEEDED(gpu.device->CreateTexture2D(&desc, nullptr, rawSource.addressof())));
        ASSERT_TRUE(SUCCEEDED(gpu.device->CreateRenderTargetView(rawSource.get(), nullptr, rawRtv.addressof())));
        const float clearColor[4] = {0.2f, 0.6f, 0.4f, 1.0f};
        gpu.context->ClearRenderTargetView(rawRtv.get(), clearColor);

        ID3D11Texture2D* sampleable = copy.Prepare(gpu.device.get(), gpu.context.get(), rawSource.get());
        ASSERT_NE(sampleable, nullptr);
        ComGuard<ID3D11ShaderResourceView> view = ViewOf(gpu, sampleable);
        ce::video_scale::FrameScaler::Request req;
        req.source = view.get();
        req.sourceWidth = 32;
        req.sourceHeight = 16;
        req.outputWidth = 16;
        req.outputHeight = 8;
        req.kernel = ce::video_scale::KernelSpecFromConfig("normal", 100);
        ID3D11ShaderResourceView* scaled = nullptr;
        ASSERT_TRUE(scaler.Scale(gpu.device.get(), gpu.context.get(), req, &scaled));
        ComGuard<ID3D11Resource> res;
        scaled->GetResource(res.addressof());
        const auto g = ReadChannel(gpu, res.get(), 1);
        ASSERT_EQ(g.size(), 16u * 8u);
        EXPECT_NEAR(g[0], 0.6f, 1.5f / 255.0f) << "samples=" << samples;
    }
}

TEST(FrameScalerGpuTest, LinearFp16SdrNormalizesAndClipsBeforeScaling) {
    Gpu gpu;
    ASSERT_TRUE(gpu.Create());
    ConversionRig rig;
    ASSERT_TRUE(rig.Init(gpu.device.get()));
    D3D11_TEXTURE2D_DESC fpDesc = {};
    fpDesc.Width = 16;
    fpDesc.Height = 8;
    fpDesc.MipLevels = fpDesc.ArraySize = fpDesc.SampleDesc.Count = 1;
    fpDesc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    fpDesc.Usage = D3D11_USAGE_IMMUTABLE;
    fpDesc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    // Left half negative (-0.5 -> clips to 0), right half >1 (2.5 -> clips to 1), green at 0.214041 (~0.5 sRGB).
    std::vector<float> pixels(16u * 8u * 4u);
    for (size_t i = 0; i < 16u * 8u; ++i) {
        pixels[i * 4 + 0] = -0.5f;
        pixels[i * 4 + 1] = 0.214041f;
        pixels[i * 4 + 2] = 2.5f;
        pixels[i * 4 + 3] = 0.0f;
    }
    D3D11_SUBRESOURCE_DATA data = {pixels.data(), 16u * 16u, 0};
    ComGuard<ID3D11Texture2D> source;
    ASSERT_TRUE(SUCCEEDED(gpu.device->CreateTexture2D(&fpDesc, &data, source.addressof())));
    ComGuard<ID3D11ShaderResourceView> sourceView = ViewOf(gpu, source.get());

    D3D11_TEXTURE2D_DESC normDesc = fpDesc;
    normDesc.Format = DXGI_FORMAT_R10G10B10A2_UNORM;
    normDesc.Usage = D3D11_USAGE_DEFAULT;
    normDesc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    ComGuard<ID3D11Texture2D> normTex;
    ComGuard<ID3D11RenderTargetView> normRtv;
    ComGuard<ID3D11ShaderResourceView> normSrv;
    ASSERT_TRUE(SUCCEEDED(gpu.device->CreateTexture2D(&normDesc, nullptr, normTex.addressof())));
    ASSERT_TRUE(SUCCEEDED(gpu.device->CreateRenderTargetView(normTex.get(), nullptr, normRtv.addressof())));
    ASSERT_TRUE(SUCCEEDED(gpu.device->CreateShaderResourceView(normTex.get(), nullptr, normSrv.addressof())));
    ASSERT_TRUE(ce::video_color::DrawRgbNormalize(gpu.context.get(), rig.Kernels(), sourceView.get(), normRtv.get(), 16,
                                                  8, ce::video_format::RgbColorTransform::kLinearToSrgb, 80.0f));

    ce::video_scale::FrameScaler scaler;
    ce::video_scale::FrameScaler::Request req;
    req.source = normSrv.get();
    req.sourceWidth = 16;
    req.sourceHeight = 8;
    req.outputWidth = 8;
    req.outputHeight = 4;
    req.kernel = ce::video_scale::KernelSpecFromConfig("normal", 100);
    req.sourceBits = 10;
    req.outputBits = 10;
    ID3D11ShaderResourceView* scaled = nullptr;
    ASSERT_TRUE(scaler.Scale(gpu.device.get(), gpu.context.get(), req, &scaled));
    ComGuard<ID3D11Resource> res;
    scaled->GetResource(res.addressof());
    const auto r = ReadChannel(gpu, res.get(), 0);
    const auto g = ReadChannel(gpu, res.get(), 1);
    const auto b = ReadChannel(gpu, res.get(), 2);
    ASSERT_EQ(r.size(), 32u);
    EXPECT_NEAR(r[0], 0.0f, 1.0f / 1023.0f);
    EXPECT_NEAR(g[0], 0.5f, 2.0f / 1023.0f);
    EXPECT_NEAR(b[0], 1.0f, 1.0f / 1023.0f);
}

}  // namespace
