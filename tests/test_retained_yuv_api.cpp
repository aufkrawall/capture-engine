#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <mmsystem.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "common/capture/screen_grab_privacy.h"
#include "common/platform/raii_helpers.h"
#include "mediaengine/engine/mediaengine.h"

// Retained-YUV reservoir plumbing on the real adapter: the media engine's
// ingress conversion export and the NV12/P010 privacy black frame.

namespace {

using ce::ComGuard;

struct Gpu {
    ComGuard<ID3D11Device> device;
    ComGuard<ID3D11DeviceContext> context;
    bool Create() {
        return SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                                           nullptr, 0, D3D11_SDK_VERSION, device.addressof(), nullptr,
                                           context.addressof()));
    }
};

ComGuard<ID3D11Texture2D> MakeTexture(ID3D11Device* device, UINT w, UINT h, DXGI_FORMAT format, UINT bind,
                                      const void* data = nullptr, UINT pitch = 0) {
    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = w;
    desc.Height = h;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = format;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = bind;
    D3D11_SUBRESOURCE_DATA init = {data, pitch, 0};
    ComGuard<ID3D11Texture2D> texture;
    EXPECT_TRUE(SUCCEEDED(device->CreateTexture2D(&desc, data ? &init : nullptr, texture.addressof())));
    return texture;
}

// First luma code and first Cb/Cr codes of an NV12/P010 texture.
bool ReadFirstCodes(Gpu& gpu, ID3D11Texture2D* texture, int* y, int* cb, int* cr) {
    D3D11_TEXTURE2D_DESC desc = {};
    texture->GetDesc(&desc);
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ComGuard<ID3D11Texture2D> staging;
    if (FAILED(gpu.device->CreateTexture2D(&desc, nullptr, staging.addressof())))
        return false;
    gpu.context->CopyResource(staging.get(), texture);
    D3D11_MAPPED_SUBRESOURCE mapped = {};
    if (FAILED(gpu.context->Map(staging.get(), 0, D3D11_MAP_READ, 0, &mapped)))
        return false;
    const auto* bytes = static_cast<const uint8_t*>(mapped.pData);
    const size_t chroma = static_cast<size_t>(mapped.RowPitch) * desc.Height;
    if (desc.Format == DXGI_FORMAT_NV12) {
        *y = bytes[0];
        *cb = bytes[chroma];
        *cr = bytes[chroma + 1];
    } else {
        uint16_t values[3] = {};
        std::memcpy(&values[0], bytes, 2);
        std::memcpy(&values[1], bytes + chroma, 2);
        std::memcpy(&values[2], bytes + chroma + 2, 2);
        *y = values[0] >> 6;
        *cb = values[1] >> 6;
        *cr = values[2] >> 6;
    }
    gpu.context->Unmap(staging.get(), 0);
    return true;
}

bool PlaneRtv(ID3D11Device* device, ID3D11Texture2D* texture, DXGI_FORMAT format, UINT plane,
              ComGuard<ID3D11RenderTargetView1>* out) {
    ComGuard<ID3D11Device3> device3;
    if (FAILED(device->QueryInterface(IID_PPV_ARGS(device3.addressof()))))
        return false;
    D3D11_RENDER_TARGET_VIEW_DESC1 desc = {};
    desc.Format = format;
    desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    desc.Texture2D.PlaneSlice = plane;
    return SUCCEEDED(device3->CreateRenderTargetView1(texture, &desc, out->addressof()));
}

}  // namespace

TEST(RetainedYuvApiTest, PrivacyBlackFrameUsesVideoBlackCodesOfTheRange) {
    Gpu gpu;
    ASSERT_TRUE(gpu.Create());
    struct Case {
        DXGI_FORMAT format;
        bool fullRange;
        int y;
        int c;
    };
    const Case cases[] = {{DXGI_FORMAT_NV12, false, 16, 128},
                          {DXGI_FORMAT_NV12, true, 0, 128},
                          {DXGI_FORMAT_P010, false, 64, 512},
                          {DXGI_FORMAT_P010, true, 0, 512}};
    for (const Case& c : cases) {
        ComGuard<ID3D11Texture2D> reference =
            MakeTexture(gpu.device.get(), 64, 32, c.format, D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
        ce::screen_grab_privacy::BlackFrameTextureCache cache;
        cache.SetYuvFullRange(c.fullRange);
        ASSERT_TRUE(cache.Prepare(reference.get())) << c.format;
        EXPECT_EQ(cache.Format(), c.format);
        int y = -1;
        int cb = -1;
        int cr = -1;
        ASSERT_TRUE(ReadFirstCodes(gpu, cache.Get(), &y, &cb, &cr));
        EXPECT_EQ(y, c.y) << c.format << " full=" << c.fullRange;
        EXPECT_EQ(cb, c.c);
        EXPECT_EQ(cr, c.c);
    }
}

TEST(RetainedYuvApiTest, RouteQueryWithoutEngineKeepsRgb) {
    MediaEngineRetainedFrameQuery query;
    query.sourceFormat = DXGI_FORMAT_R10G10B10A2_UNORM;
    query.width = 3840;
    query.height = 2160;
    query.sameDevice = true;
    MediaEngineRetainedFrameRoute route;
    ASSERT_TRUE(MediaEngine_QueryRetainedFrameRoute(&query, &route));
    EXPECT_EQ(route.yuvFormat, 0u);
    EXPECT_STRNE(route.reason, "");
    MediaEngineRetainedFrameQuery stale = query;
    stale.size = 4;  // ABI guard
    EXPECT_FALSE(MediaEngine_QueryRetainedFrameRoute(&stale, &route));
}

TEST(RetainedYuvApiTest, IngressExportConvertsAnSdrFrameIntoP010Planes) {
    Gpu gpu;
    ASSERT_TRUE(gpu.Create());
    // R10 white: BT.709 limited P010 Y=940, Cb=Cr=512 (SdrYuvShaderTest codes).
    const uint32_t white = 1023u | (1023u << 10) | (1023u << 20) | (3u << 30);
    const std::vector<uint32_t> pixels(16 * 8, white);
    ComGuard<ID3D11Texture2D> source = MakeTexture(gpu.device.get(), 16, 8, DXGI_FORMAT_R10G10B10A2_UNORM,
                                                   D3D11_BIND_SHADER_RESOURCE, pixels.data(), 16 * 4);
    ComGuard<ID3D11ShaderResourceView> view;
    ASSERT_TRUE(SUCCEEDED(gpu.device->CreateShaderResourceView(source.get(), nullptr, view.addressof())));
    ComGuard<ID3D11Texture2D> slot = MakeTexture(gpu.device.get(), 16, 8, DXGI_FORMAT_P010,
                                                 D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE);
    ComGuard<ID3D11RenderTargetView1> luma;
    ComGuard<ID3D11RenderTargetView1> chroma;
    ASSERT_TRUE(PlaneRtv(gpu.device.get(), slot.get(), DXGI_FORMAT_R16_UNORM, 0, &luma));
    ASSERT_TRUE(PlaneRtv(gpu.device.get(), slot.get(), DXGI_FORMAT_R16G16_UNORM, 1, &chroma));

    MediaEngineRetainedFrameConversion conversion;
    conversion.sourceView = view.get();
    conversion.width = 16;
    conversion.height = 8;
    conversion.lumaTarget = luma.get();
    conversion.chromaTarget = chroma.get();
    conversion.route.yuvFormat = DXGI_FORMAT_P010;
    conversion.route.rgbLevels = 1023;
    ASSERT_TRUE(MediaEngine_ConvertToRetainedFrame(&conversion));
    int y = -1;
    int cb = -1;
    int cr = -1;
    ASSERT_TRUE(ReadFirstCodes(gpu, slot.get(), &y, &cb, &cr));
    EXPECT_EQ(y, 940);
    EXPECT_EQ(cb, 512);
    EXPECT_EQ(cr, 512);

    MediaEngineRetainedFrameConversion rgbRoute = conversion;
    rgbRoute.route.yuvFormat = 0;  // RGB route: nothing to convert
    EXPECT_FALSE(MediaEngine_ConvertToRetainedFrame(&rgbRoute));
    MediaEngine_ReleaseRetainedFrameConverter();
}
