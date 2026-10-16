#include <gtest/gtest.h>

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11_4.h>
#include <mmsystem.h>

#include <string>
#include <vector>

#include "common/platform/raii_helpers.h"
#include "mediaengine/video/encoder_device_bridge.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/dict.h>
#include <libavutil/hwcontext.h>
#include <libavutil/hwcontext_d3d11va.h>
}

// End to end through FFmpeg's NVENC wrapper, as VideoEncoder uses it: the pool
// lives on the bridge's encoder device with shared NT handles, CE's work device
// writes each frame through its alias, and the frame is submitted only after
// the work-device fence completed. Skips without an NVENC-capable GPU.

namespace {

struct AvGuards {
    AVBufferRef* device = nullptr;
    AVBufferRef* frames = nullptr;
    AVCodecContext* codec = nullptr;
    AVPacket* packet = nullptr;
    ~AvGuards() {
        av_packet_free(&packet);
        avcodec_free_context(&codec);
        av_buffer_unref(&frames);
        av_buffer_unref(&device);
    }
};

int DrainPackets(AVCodecContext* codec, AVPacket* packet) {
    int count = 0;
    while (avcodec_receive_packet(codec, packet) == 0) {
        ++count;
        av_packet_unref(packet);
    }
    return count;
}

// Encodes 12 frames written on the work device into pool textures on the
// bridge's encoder device; skips when the encoder cannot open here.
void EncodeBridgeWrittenFrames(const char* encoderName, const char* options) {
    const AVCodec* encoder = avcodec_find_encoder_by_name(encoderName);
    if (!encoder)
        GTEST_SKIP() << encoderName << " not built";
    ce::ComGuard<ID3D11Device> workDevice;
    ce::ComGuard<ID3D11DeviceContext> workContext;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr,
                                 0, D3D11_SDK_VERSION, workDevice.addressof(), nullptr, workContext.addressof())))
        GTEST_SKIP() << "no hardware D3D11 device";
    ce::media::EncoderDeviceBridge bridge;
    std::string error;
    ASSERT_TRUE(bridge.Create(workDevice.get(), &error)) << error;

    AvGuards av;
    av.device = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
    ASSERT_NE(av.device, nullptr);
    auto* deviceHw = reinterpret_cast<AVD3D11VADeviceContext*>(
        reinterpret_cast<AVHWDeviceContext*>(av.device->data)->hwctx);
    deviceHw->device = bridge.EncoderDevice();
    deviceHw->device->AddRef();
    ASSERT_GE(av_hwdevice_ctx_init(av.device), 0);

    constexpr int kWidth = 320;
    constexpr int kHeight = 192;
    av.frames = av_hwframe_ctx_alloc(av.device);
    ASSERT_NE(av.frames, nullptr);
    auto* frames = reinterpret_cast<AVHWFramesContext*>(av.frames->data);
    auto* framesHw = reinterpret_cast<AVD3D11VAFramesContext*>(frames->hwctx);
    frames->format = AV_PIX_FMT_D3D11;
    frames->sw_format = AV_PIX_FMT_NV12;
    frames->width = kWidth;
    frames->height = kHeight;
    frames->initial_pool_size = 0;
    framesHw->BindFlags |= D3D11_BIND_RENDER_TARGET;
    framesHw->MiscFlags |= ce::media::kEncoderBridgePoolMiscFlags;
    ASSERT_GE(av_hwframe_ctx_init(av.frames), 0);

    av.codec = avcodec_alloc_context3(encoder);
    ASSERT_NE(av.codec, nullptr);
    av.codec->width = kWidth;
    av.codec->height = kHeight;
    av.codec->time_base = {1, 60};
    av.codec->framerate = {60, 1};
    av.codec->pix_fmt = AV_PIX_FMT_D3D11;
    av.codec->max_b_frames = 0;
    av.codec->hw_device_ctx = av_buffer_ref(av.device);
    av.codec->hw_frames_ctx = av_buffer_ref(av.frames);
    AVDictionary* openOptions = nullptr;
    if (options)
        av_dict_parse_string(&openOptions, options, "=", ":", 0);
    const int opened = avcodec_open2(av.codec, encoder, &openOptions);
    av_dict_free(&openOptions);
    if (opened < 0)
        GTEST_SKIP() << encoderName << " unavailable on this GPU";
    av.packet = av_packet_alloc();
    ASSERT_NE(av.packet, nullptr);

    D3D11_TEXTURE2D_DESC sourceDesc = {};
    sourceDesc.Width = kWidth;
    sourceDesc.Height = kHeight;
    sourceDesc.MipLevels = sourceDesc.ArraySize = sourceDesc.SampleDesc.Count = 1;
    sourceDesc.Format = DXGI_FORMAT_NV12;
    sourceDesc.Usage = D3D11_USAGE_DEFAULT;
    std::vector<uint8_t> pixels(static_cast<size_t>(kWidth) * kHeight * 3 / 2, 0x80);
    D3D11_SUBRESOURCE_DATA data = {pixels.data(), kWidth, 0};
    ce::ComGuard<ID3D11Texture2D> source;
    ASSERT_TRUE(SUCCEEDED(workDevice->CreateTexture2D(&sourceDesc, &data, source.addressof())));

    constexpr int kFrames = 12;
    int packets = 0;
    for (int index = 0; index < kFrames; ++index) {
        AVFrame* frame = av_frame_alloc();
        ASSERT_NE(frame, nullptr);
        ASSERT_GE(av_hwframe_get_buffer(av.frames, frame, 0), 0);
        ID3D11Texture2D* alias = bridge.WorkTexture(reinterpret_cast<ID3D11Texture2D*>(frame->data[0]));
        ASSERT_NE(alias, nullptr);
        workContext->CopyResource(alias, source.get());
        const uint64_t fence = bridge.SignalWorkDone(workContext.get());
        ASSERT_NE(fence, 0u);
        ASSERT_EQ(bridge.Wait(fence, 5000), ce::media::EncoderDeviceBridge::WaitResult::Complete);
        frame->pts = index;
        EXPECT_EQ(avcodec_send_frame(av.codec, frame), 0) << "frame " << index;
        av_frame_free(&frame);
        packets += DrainPackets(av.codec, av.packet);
    }
    ASSERT_EQ(avcodec_send_frame(av.codec, nullptr), 0);
    packets += DrainPackets(av.codec, av.packet);
    EXPECT_EQ(packets, kFrames);
    // Pool textures are recycled after NVENC released them, so the alias cache
    // stays at the pool size instead of growing per frame.
    EXPECT_LT(bridge.CachedTextureCount(), static_cast<size_t>(kFrames));
}

TEST(EncoderDeviceBridgeNvencTest, NvencEncodesPoolFramesWrittenOnTheWorkDevice) {
    EncodeBridgeWrittenFrames("h264_nvenc", nullptr);
}

TEST(EncoderDeviceBridgeNvencTest, MediaFoundationHardwareMftEncodesPoolFramesWrittenOnTheWorkDevice) {
    // The DXGI device manager takes the pool's (encoder) device.
    EncodeBridgeWrittenFrames("h264_mf", "hw_encoding=1");
}

}  // namespace
