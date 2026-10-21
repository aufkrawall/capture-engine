// SPDX-License-Identifier: MIT
// Copyright (c) 2026 aufkrawall
// A frame taken from the encoder's hardware pool must leave no frames-context reference behind
// (mediaengine/video/encoder_pool_frame.h). The encode paths set frame->hw_frames_ctx before av_hwframe_get_buffer,
// which overwrites it: the lost reference kept the pool and its D3D11 device alive after every in-process recording.
#include "mediaengine/video/encoder_pool_frame.h"

#include <gtest/gtest.h>

#include <d3d11.h>

#include <string>

extern "C" {
#include <libavutil/hwcontext_d3d11va.h>
}

namespace {

struct D3D11FramesPool {
    ID3D11Device* device = nullptr;
    AVBufferRef* deviceRef = nullptr;
    AVBufferRef* framesRef = nullptr;

    std::string failure;

    bool Fail(const char* step, int code) {
        failure = std::string(step) + " (" + std::to_string(code) + ")";
        return false;
    }
    bool Create() {
        // FFmpeg's D3D11 device wants the video interfaces, as the encoder's own device has them.
        constexpr UINT kFlags = D3D11_CREATE_DEVICE_BGRA_SUPPORT | D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
        HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, kFlags, nullptr, 0, D3D11_SDK_VERSION,
                                       &device, nullptr, nullptr);
        if (FAILED(hr))
            hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, kFlags, nullptr, 0, D3D11_SDK_VERSION,
                                   &device, nullptr, nullptr);
        if (FAILED(hr))
            return Fail("D3D11CreateDevice(video support)", static_cast<int>(hr));
        deviceRef = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_D3D11VA);
        if (!deviceRef)
            return Fail("av_hwdevice_ctx_alloc", 0);
        auto* d3d11 = static_cast<AVD3D11VADeviceContext*>(reinterpret_cast<AVHWDeviceContext*>(deviceRef->data)->hwctx);
        d3d11->device = device;
        device->AddRef();  // FFmpeg owns one reference
        if (const int ret = av_hwdevice_ctx_init(deviceRef); ret < 0)
            return Fail("av_hwdevice_ctx_init", ret);
        framesRef = av_hwframe_ctx_alloc(deviceRef);
        if (!framesRef)
            return Fail("av_hwframe_ctx_alloc", 0);
        auto* frames = reinterpret_cast<AVHWFramesContext*>(framesRef->data);
        frames->format = AV_PIX_FMT_D3D11;
        frames->sw_format = AV_PIX_FMT_BGRA;
        frames->width = 64;
        frames->height = 64;
        frames->initial_pool_size = 2;
        static_cast<AVD3D11VAFramesContext*>(frames->hwctx)->BindFlags = D3D11_BIND_SHADER_RESOURCE;
        if (const int ret = av_hwframe_ctx_init(framesRef); ret < 0)
            return Fail("av_hwframe_ctx_init", ret);
        return true;
    }
    // The device's own count once every FFmpeg reference is gone: 0 means it was destroyed.
    ULONG Destroy() {
        av_buffer_unref(&framesRef);
        av_buffer_unref(&deviceRef);
        const ULONG left = device ? device->Release() : 0;
        device = nullptr;
        return left;
    }
    ~D3D11FramesPool() {
        if (device)
            Destroy();
    }
};

}  // namespace

TEST(EncoderPoolFrameTest, APoolFrameLeavesNoFramesContextReferenceBehind) {
    D3D11FramesPool pool;
    ASSERT_TRUE(pool.Create()) << pool.failure;
    ASSERT_EQ(av_buffer_get_ref_count(pool.framesRef), 1);

    AVFrame* frame = av_frame_alloc();
    ASSERT_NE(frame, nullptr);
    frame->hw_frames_ctx = av_buffer_ref(pool.framesRef);  // what the encode paths used to set beforehand
    ASSERT_GE(ce::media::GetEncoderPoolFrame(pool.framesRef, frame), 0);
    ASSERT_NE(frame->data[0], nullptr);
    EXPECT_EQ(av_buffer_get_ref_count(pool.framesRef), 2) << "ours plus the one the frame holds";
    av_frame_free(&frame);
    EXPECT_EQ(av_buffer_get_ref_count(pool.framesRef), 1) << "the frame left no reference behind";

    EXPECT_EQ(pool.Destroy(), 0u) << "with the last reference gone the D3D11 device is destroyed";
}

// The FFmpeg behaviour the helper exists for: av_hwframe_get_buffer replaces frame->hw_frames_ctx without releasing it.
TEST(EncoderPoolFrameTest, FfmpegOverwritesAPresetFramesContextReference) {
    D3D11FramesPool pool;
    ASSERT_TRUE(pool.Create()) << pool.failure;
    AVFrame* frame = av_frame_alloc();
    ASSERT_NE(frame, nullptr);
    AVBufferRef* preset = av_buffer_ref(pool.framesRef);
    frame->hw_frames_ctx = preset;
    ASSERT_GE(av_hwframe_get_buffer(pool.framesRef, frame, 0), 0);
    EXPECT_NE(frame->hw_frames_ctx, preset) << "the preset reference was replaced, not released";
    av_frame_free(&frame);
    EXPECT_EQ(av_buffer_get_ref_count(pool.framesRef), 2) << "the lost reference still counts";
    av_buffer_unref(&preset);  // only this test still knows it
    EXPECT_EQ(pool.Destroy(), 0u);
}
