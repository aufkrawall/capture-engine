#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <mmsystem.h>

#include <cstdint>
#include <cstring>
#include <vector>

#include "common/capture/screen_grab_privacy.h"
#include "captureengine/media/wgc_shared_planar.h"
#include "common/platform/raii_helpers.h"
#include "mediaengine/engine/mediaengine.h"

// Retained-YUV reservoir plumbing on the real adapter: the media engine's
// ingress conversion export, the split-capture-device shared slot and the
// NV12/P010 privacy black frame.

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
    desc.MiscFlags = 0;  // a shared slot's flags are invalid on a staging copy
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

namespace {

// A split capture device (DXGI duplication) converts into a slot the encoder's
// device created and shared (wgc_shared_planar.h); the encoder reads it after
// the keyed-mutex handoff at key 1, as WGCCapture publishes it.
void ExpectSharedSlotConversion(DXGI_FORMAT slotFormat, DXGI_FORMAT sourceFormat, uint32_t sourcePixel, int y,
                                int c) {
    Gpu encoder;
    Gpu capture;
    ASSERT_TRUE(encoder.Create());
    ASSERT_TRUE(capture.Create());
    const bool probed = ce::capture_policy::DeviceSharesPlanarFormat(encoder.device.get(), slotFormat) &&
                        ce::capture_policy::DeviceSharesPlanarFormat(capture.device.get(), slotFormat);

    D3D11_TEXTURE2D_DESC desc = {};
    desc.Width = 16;
    desc.Height = 8;
    desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
    desc.Format = slotFormat;
    desc.Usage = D3D11_USAGE_DEFAULT;
    desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
    desc.MiscFlags = D3D11_RESOURCE_MISC_SHARED_KEYEDMUTEX | D3D11_RESOURCE_MISC_SHARED_NTHANDLE;
    ComGuard<ID3D11Texture2D> slot;
    ComGuard<IDXGIResource1> resource;
    HANDLE handle = nullptr;
    bool shared = SUCCEEDED(encoder.device->CreateTexture2D(&desc, nullptr, slot.addressof())) &&
                  SUCCEEDED(slot->QueryInterface(IID_PPV_ARGS(resource.addressof()))) &&
                  SUCCEEDED(resource->CreateSharedHandle(nullptr, DXGI_SHARED_RESOURCE_READ |
                                                                      DXGI_SHARED_RESOURCE_WRITE,
                                                         nullptr, &handle));
    ComGuard<ID3D11Device1> capture1;
    ComGuard<ID3D11Texture2D> alias;
    shared = shared && SUCCEEDED(capture.device->QueryInterface(IID_PPV_ARGS(capture1.addressof()))) &&
             SUCCEEDED(capture1->OpenSharedResource1(handle, IID_PPV_ARGS(alias.addressof())));
    if (handle)
        CloseHandle(handle);
    // The probe must not be stricter than the driver, or the route never engages.
    EXPECT_EQ(probed, shared) << "format=" << slotFormat;
    if (!shared)
        GTEST_SKIP() << "adapter cannot share format " << slotFormat;

    const bool eightBit = slotFormat == DXGI_FORMAT_NV12;
    ComGuard<ID3D11RenderTargetView1> luma;
    ComGuard<ID3D11RenderTargetView1> chroma;
    ASSERT_TRUE(PlaneRtv(capture.device.get(), alias.get(), eightBit ? DXGI_FORMAT_R8_UNORM : DXGI_FORMAT_R16_UNORM,
                         0, &luma));
    ASSERT_TRUE(PlaneRtv(capture.device.get(), alias.get(),
                         eightBit ? DXGI_FORMAT_R8G8_UNORM : DXGI_FORMAT_R16G16_UNORM, 1, &chroma));
    const std::vector<uint32_t> pixels(16 * 8, sourcePixel);
    ComGuard<ID3D11Texture2D> source = MakeTexture(capture.device.get(), 16, 8, sourceFormat,
                                                   D3D11_BIND_SHADER_RESOURCE, pixels.data(), 16 * 4);
    ComGuard<ID3D11ShaderResourceView> view;
    ASSERT_TRUE(SUCCEEDED(capture.device->CreateShaderResourceView(source.get(), nullptr, view.addressof())));

    ComGuard<IDXGIKeyedMutex> writer;
    ComGuard<IDXGIKeyedMutex> reader;
    ASSERT_TRUE(SUCCEEDED(alias->QueryInterface(IID_PPV_ARGS(writer.addressof()))));
    ASSERT_TRUE(SUCCEEDED(slot->QueryInterface(IID_PPV_ARGS(reader.addressof()))));
    ASSERT_EQ(writer->AcquireSync(0, 1000), S_OK);
    MediaEngineRetainedFrameConversion conversion;
    conversion.sourceView = view.get();
    conversion.width = 16;
    conversion.height = 8;
    conversion.lumaTarget = luma.get();
    conversion.chromaTarget = chroma.get();
    conversion.route.yuvFormat = slotFormat;
    conversion.route.rgbLevels = eightBit ? 255 : 1023;
    const bool converted = MediaEngine_ConvertToRetainedFrame(&conversion);
    capture.context->Flush();
    ASSERT_EQ(writer->ReleaseSync(1), S_OK);
    ASSERT_TRUE(converted);

    ASSERT_EQ(reader->AcquireSync(1, 1000), S_OK);
    int yCode = -1;
    int cb = -1;
    int cr = -1;
    const bool read = ReadFirstCodes(encoder, slot.get(), &yCode, &cb, &cr);
    EXPECT_EQ(reader->ReleaseSync(0), S_OK);
    ASSERT_TRUE(read);
    EXPECT_EQ(yCode, y) << "format=" << slotFormat;
    EXPECT_EQ(cb, c);
    EXPECT_EQ(cr, c);
    MediaEngine_ReleaseRetainedFrameConverter();
}

}  // namespace

TEST(RetainedYuvApiTest, SplitCaptureDeviceConvertsIntoASharedP010Slot) {
    // R10 white: BT.709 limited P010 Y=940, Cb=Cr=512.
    const uint32_t white = 1023u | (1023u << 10) | (1023u << 20) | (3u << 30);
    ExpectSharedSlotConversion(DXGI_FORMAT_P010, DXGI_FORMAT_R10G10B10A2_UNORM, white, 940, 512);
}

TEST(RetainedYuvApiTest, SplitCaptureDeviceConvertsIntoASharedNv12Slot) {
    // BGRA8 white: BT.709 limited NV12 Y=235, Cb=Cr=128.
    ExpectSharedSlotConversion(DXGI_FORMAT_NV12, DXGI_FORMAT_B8G8R8A8_UNORM, 0xFFFFFFFFu, 235, 128);
}
