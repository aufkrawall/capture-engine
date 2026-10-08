#include <gtest/gtest.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

#include "common/graphics/gamma_policy.h"
#include "hook/sharpen/sharpen_d3d11.h"

namespace {
using Microsoft::WRL::ComPtr;

class GammaShaderTest : public testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0,
                                   D3D11_SDK_VERSION, &device_, nullptr, &context_), S_OK);
    }

    std::vector<uint8_t> Run(DXGI_FORMAT format, DXGI_FORMAT viewFormat, uint32_t width,
                             uint32_t pixelBytes, const void* input, ce::sharpen::Request request) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = width;
        desc.Height = 32;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = viewFormat == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ? DXGI_FORMAT_R8G8B8A8_TYPELESS : format;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        D3D11_SUBRESOURCE_DATA data = {};
        data.pSysMem = input;
        data.SysMemPitch = width * pixelBytes;
        ComPtr<ID3D11Texture2D> target;
        EXPECT_EQ(device_->CreateTexture2D(&desc, &data, &target), S_OK);
        if (!target)
            return {};
        D3D11_RENDER_TARGET_VIEW_DESC viewDesc = {};
        viewDesc.Format = viewFormat;
        viewDesc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
        ComPtr<ID3D11RenderTargetView> view;
        EXPECT_EQ(device_->CreateRenderTargetView(target.Get(), &viewDesc, &view), S_OK);
        ce::sharpen::D3D11Pass pass;
        const bool expectedRun = ce::sharpen::Requested(request);
        EXPECT_EQ(pass.Render(device_.Get(), context_.Get(), view.Get(), request,
                              ce::sharpen::Route::NormalBackbuffer, ce::sharpen::TargetEncoding::Unorm), expectedRun);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        EXPECT_EQ(device_->CreateTexture2D(&desc, nullptr, &staging), S_OK);
        if (!staging)
            return {};
        context_->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        if (context_->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped) != S_OK) {
            ADD_FAILURE() << "Gamma output readback failed";
            return {};
        }
        std::vector<uint8_t> output(static_cast<size_t>(width) * 32 * pixelBytes);
        for (uint32_t y = 0; y < 32; ++y) {
            std::memcpy(output.data() + static_cast<size_t>(y) * width * pixelBytes,
                        static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                        static_cast<size_t>(width) * pixelBytes);
        }
        context_->Unmap(staging.Get(), 0);
        return output;
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
};

TEST_F(GammaShaderTest, FloatOutputMatchesReferenceForEveryCurvePair) {
    std::vector<std::array<float, 4>> input(256 * 32);
    for (size_t index = 0; index < input.size(); ++index) {
        const float value = static_cast<float>(index % 256) / 255.0f;
        input[index] = {value, value * 0.75f, value * 0.25f, 0.375f};
    }
    for (float source : {0.0f, 2.2f, 2.4f}) {
        for (float destination : {0.0f, 2.2f, 2.4f}) {
            ce::sharpen::Request request;
            request.gamma = {source, destination};
            const auto output = Run(DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                    256, 16, input.data(), request);
            ASSERT_EQ(output.size(), input.size() * sizeof(input[0]));
            for (size_t index = 0; index < input.size(); ++index) {
                std::array<float, 4> pixel;
                std::memcpy(pixel.data(), output.data() + index * sizeof(pixel), sizeof(pixel));
                for (size_t channel = 0; channel < 3; ++channel)
                    ASSERT_NEAR(pixel[channel], ce::gamma::Convert(input[index][channel], request.gamma), 3e-6)
                        << "source=" << source << " destination=" << destination << " pixel=" << index;
                ASSERT_EQ(pixel[3], input[index][3]);
            }
        }
    }
}

TEST_F(GammaShaderTest, EightBitDitherIsBoundedNeutralAndSpatiallyStableWithSrgbViews) {
    std::vector<uint32_t> input(256 * 32);
    for (size_t index = 0; index < input.size(); ++index) {
        const uint32_t code = static_cast<uint32_t>(index % 256);
        input[index] = code | (code << 8) | (code << 16) | (93u << 24);
    }
    ce::sharpen::Request request;
    request.gamma.destination = 0.0f;
    for (DXGI_FORMAT view : {DXGI_FORMAT_R8G8B8A8_UNORM, DXGI_FORMAT_R8G8B8A8_UNORM_SRGB}) {
        const auto output = Run(DXGI_FORMAT_R8G8B8A8_UNORM, view, 256, 4, input.data(), request);
        ASSERT_EQ(output.size(), input.size() * 4);
        EXPECT_EQ(output, Run(DXGI_FORMAT_R8G8B8A8_UNORM, view, 256, 4, input.data(), request));
        double sumError = 0.0;
        for (size_t index = 0; index < input.size(); ++index) {
            const size_t offset = index * 4;
            const double expected = ce::gamma::Convert(static_cast<double>(index % 256) / 255.0, request.gamma) * 255.0;
            ASSERT_LE(std::abs(output[offset] - expected), 1.01);
            ASSERT_EQ(output[offset], output[offset + 1]);
            ASSERT_EQ(output[offset], output[offset + 2]);
            ASSERT_EQ(output[offset + 3], 93);
            if (index % 256 == 0 || index % 256 == 255)
                ASSERT_EQ(output[offset], index % 256);
            sumError += output[offset] - expected;
        }
        EXPECT_LT(std::abs(sumError / static_cast<double>(input.size())), 0.025);
    }
}

TEST_F(GammaShaderTest, TenBitConversionPreservesAlphaAndCodeAccuracy) {
    std::vector<uint32_t> input(1024 * 32);
    for (size_t index = 0; index < input.size(); ++index) {
        const uint32_t code = static_cast<uint32_t>(index % 1024);
        input[index] = code | (code << 10) | (code << 20) | (2u << 30);
    }
    ce::sharpen::Request request;
    request.gamma = {2.4f, 0.0f};
    const auto output = Run(DXGI_FORMAT_R10G10B10A2_UNORM, DXGI_FORMAT_R10G10B10A2_UNORM,
                            1024, 4, input.data(), request);
    ASSERT_EQ(output.size(), input.size() * 4);
    for (size_t index = 0; index < input.size(); ++index) {
        uint32_t pixel = 0;
        std::memcpy(&pixel, output.data() + index * 4, 4);
        const uint32_t red = pixel & 1023u;
        const double expected = ce::gamma::Convert(static_cast<double>(index % 1024) / 1023.0, request.gamma) * 1023.0;
        ASSERT_LE(std::abs(red - expected), 1.01);
        ASSERT_EQ(red, (pixel >> 10) & 1023u);
        ASSERT_EQ(red, (pixel >> 20) & 1023u);
        ASSERT_EQ(pixel >> 30, 2u);
    }
}

TEST_F(GammaShaderTest, CombinedSharpeningAppliesGammaAfterTheIntensityMix) {
    std::vector<std::array<float, 4>> input(256 * 32);
    for (size_t index = 0; index < input.size(); ++index) {
        const float value = index % 3 == 0 ? 0.3f : 0.1f;
        input[index] = {value, value * 0.75f, value * 0.25f, 0.625f};
    }
    for (ce::sharpen::Mode mode : {ce::sharpen::Mode::Cas, ce::sharpen::Mode::Rcas}) {
        for (float amount : {0.0f, 0.5f, 1.0f}) {
            ce::sharpen::Request request;
            request.mode = mode;
            request.intensity = amount;
            const auto sharpened = Run(DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                       256, 16, input.data(), request);
            ASSERT_EQ(sharpened.size(), input.size() * sizeof(input[0]));
            std::array<float, 4> baseline;
            std::memcpy(baseline.data(), sharpened.data() + 1024 * sizeof(baseline), sizeof(baseline));
            request.gamma.destination = 0.0f;
            const auto output = Run(DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
                                    256, 16, input.data(), request);
            ASSERT_EQ(output.size(), input.size() * sizeof(input[0]));
            std::array<float, 4> pixel;
            std::memcpy(pixel.data(), output.data() + 1024 * sizeof(pixel), sizeof(pixel));
            for (size_t channel = 0; channel < 3; ++channel)
                EXPECT_NEAR(pixel[channel], ce::gamma::Convert(baseline[channel], request.gamma), 3e-6);
            EXPECT_EQ(pixel[3], 0.625f);
        }
    }
}
}  // namespace
