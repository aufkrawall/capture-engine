#include <gtest/gtest.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <array>
#include <cstdio>

#include "hook/sharpen/sharpen_d3d11.h"

// Opt-in hardware measurement; correctness runs use deterministic WARP tests.
TEST(GammaGpuTiming, DISABLED_HardwareCostAtCommonResolutions) {
    using Microsoft::WRL::ComPtr;
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ASSERT_EQ(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                               &device, nullptr, &context), S_OK);
    constexpr size_t kSamples = 8;
    for (const auto& extent : {std::array<UINT, 2>{1920, 1080}, {2560, 1440}, {3840, 2160}}) {
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = extent[0];
        desc.Height = extent[1];
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ComPtr<ID3D11Texture2D> target;
        ASSERT_EQ(device->CreateTexture2D(&desc, nullptr, &target), S_OK);
        ComPtr<ID3D11RenderTargetView> view;
        ASSERT_EQ(device->CreateRenderTargetView(target.Get(), nullptr, &view), S_OK);
        desc.Width = desc.Height = 1;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ComPtr<ID3D11Texture2D> staging;
        ASSERT_EQ(device->CreateTexture2D(&desc, nullptr, &staging), S_OK);
        for (int scenario = 0; scenario < 3; ++scenario) {
            ce::sharpen::Request request;
            request.mode = scenario == 0 ? ce::sharpen::Mode::Off : ce::sharpen::Mode::Cas;
            request.gamma.destination = scenario == 1 ? -1.0f : 0.0f;
            ce::sharpen::D3D11Pass pass;
            const float gray[] = {0.5f, 0.5f, 0.5f, 1.0f};
            context->ClearRenderTargetView(view.Get(), gray);
            ASSERT_TRUE(pass.Render(device.Get(), context.Get(), view.Get(), request,
                                   ce::sharpen::Route::NormalBackbuffer, ce::sharpen::TargetEncoding::Unorm));
            D3D11_QUERY_DESC queryDesc = {};
            queryDesc.Query = D3D11_QUERY_TIMESTAMP_DISJOINT;
            ComPtr<ID3D11Query> disjoint;
            ASSERT_EQ(device->CreateQuery(&queryDesc, &disjoint), S_OK);
            std::array<ComPtr<ID3D11Query>, kSamples * 2> timestamps;
            queryDesc.Query = D3D11_QUERY_TIMESTAMP;
            for (auto& query : timestamps)
                ASSERT_EQ(device->CreateQuery(&queryDesc, &query), S_OK);
            context->Begin(disjoint.Get());
            for (size_t sample = 0; sample < kSamples; ++sample) {
                context->ClearRenderTargetView(view.Get(), gray);
                context->End(timestamps[sample * 2].Get());
                ASSERT_TRUE(pass.Render(device.Get(), context.Get(), view.Get(), request,
                                       ce::sharpen::Route::NormalBackbuffer, ce::sharpen::TargetEncoding::Unorm));
                context->End(timestamps[sample * 2 + 1].Get());
            }
            context->End(disjoint.Get());
            const D3D11_BOX pixel = {0, 0, 0, 1, 1, 1};
            context->CopySubresourceRegion(staging.Get(), 0, 0, 0, 0, target.Get(), 0, &pixel);
            D3D11_MAPPED_SUBRESOURCE mapped = {};
            ASSERT_EQ(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), S_OK);
            context->Unmap(staging.Get(), 0);
            D3D11_QUERY_DATA_TIMESTAMP_DISJOINT frequency = {};
            ASSERT_EQ(context->GetData(disjoint.Get(), &frequency, sizeof(frequency), 0), S_OK);
            ASSERT_FALSE(frequency.Disjoint);
            UINT64 total = 0;
            for (size_t sample = 0; sample < kSamples; ++sample) {
                UINT64 begin = 0;
                UINT64 end = 0;
                ASSERT_EQ(context->GetData(timestamps[sample * 2].Get(), &begin, sizeof(begin), 0), S_OK);
                ASSERT_EQ(context->GetData(timestamps[sample * 2 + 1].Get(), &end, sizeof(end), 0), S_OK);
                total += end - begin;
            }
            const double milliseconds = static_cast<double>(total) * 1000.0 /
                                        (static_cast<double>(frequency.Frequency) * kSamples);
            std::printf("Gamma GPU: %ux%u %s %.4f ms/displayed-frame\n", extent[0], extent[1],
                        scenario == 0 ? "gamma" : scenario == 1 ? "cas" : "cas+gamma", milliseconds);
        }
    }
    context->ClearState();
    context->Flush();
}
