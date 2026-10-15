#include <gtest/gtest.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <filesystem>
#include <string>

#include "mediaengine/video/encoder_gpu_stage_probe.h"
#include "source_fragment_reader.h"

namespace stage = ce::media::gpu_stage;
using Microsoft::WRL::ComPtr;

namespace {

stage::Sample MakeSample(double spanUs, uint64_t readyUs, uint64_t sendUs, stage::Readiness readiness) {
    stage::Sample sample;
    sample.gpuSpanUs = spanUs;
    sample.submitToReadyUs = readyUs;
    sample.sendUs = sendUs;
    sample.readiness = readiness;
    return sample;
}

struct WarpStage {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> source;
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11Texture2D> staging;

    bool Create() {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                     &device, nullptr, &context)))
            return false;
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = 256;
        desc.Height = 256;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &source)) ||
            FAILED(device->CreateTexture2D(&desc, nullptr, &target)))
            return false;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging));
    }

    // The encoder's stage stand-in: one full-texture copy.
    void Work() { context->CopyResource(target.Get(), source.Get()); }

    // Blocks until the GPU has executed everything queued so far (the
    // immediate context runs in order), without sleeping or polling.
    void Drain() {
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        ASSERT_EQ(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), S_OK);
        context->Unmap(staging.Get(), 0);
    }
};

// One complete sample whose stage finished before the submit started.
void RunCompletedSample(stage::Probe& probe, WarpStage& warp, uint64_t nowMs, stage::Sample* out) {
    ASSERT_TRUE(probe.Begin(warp.device.Get(), warp.context.Get(), nowMs));
    warp.Work();
    probe.End(warp.context.Get(), 1000);
    warp.Drain();
    probe.BeforeSend(warp.context.Get(), 1500);
    probe.AfterSend(warp.context.Get(), 2000, 700);
    ASSERT_TRUE(probe.Collect(warp.context.Get(), 2100, 1000000, out));
    EXPECT_TRUE(probe.Idle());
}

}  // namespace

TEST(EncoderGpuStageWindowTest, AggregatesSamplesIntoOneLineAndResets) {
    stage::Window window;
    EXPECT_FALSE(window.Due(0));
    window.Add(MakeSample(100.0, 9000, 9400, stage::Readiness::DuringSend), 1000);
    window.Add(MakeSample(200.0, 7000, 8000, stage::Readiness::BeforeSend), 2000);
    EXPECT_EQ(window.Count(), 2u);
    EXPECT_FALSE(window.Due(5000));
    EXPECT_TRUE(window.Due(1000 + stage::kWindowMaxAgeMs));

    const std::string line = window.TakeLine(stage::Path::Fresh);
    EXPECT_NE(line.find("[VideoEncoder GPU Stage] path=fresh samples=2"), std::string::npos) << line;
    EXPECT_NE(line.find("gpuSpan=150/200us"), std::string::npos) << line;
    EXPECT_NE(line.find("submitToReady=8000/9000us"), std::string::npos) << line;
    EXPECT_NE(line.find("send=8700/9400us"), std::string::npos) << line;
    EXPECT_NE(line.find("ready(before/during/after send)=1/1/0"), std::string::npos) << line;
    EXPECT_NE(line.find("disjoint=0"), std::string::npos) << line;
    EXPECT_EQ(window.Count(), 0u);
    EXPECT_FALSE(window.Due(1000 + stage::kWindowMaxAgeMs));
}

TEST(EncoderGpuStageWindowTest, FullWindowIsDueAndDisjointSamplesStayOutOfTheSpan) {
    stage::Window window;
    for (uint32_t index = 0; index + 1 < stage::kWindowSamples; ++index)
        window.Add(MakeSample(50.0, 100, 100, stage::Readiness::AfterSend), 1000);
    EXPECT_FALSE(window.Due(1000));
    window.Add(MakeSample(-1.0, 100, 100, stage::Readiness::AfterSend), 1000);
    EXPECT_TRUE(window.Due(1000));
    const std::string line = window.TakeLine(stage::Path::Repeat);
    EXPECT_NE(line.find("path=repeat"), std::string::npos) << line;
    EXPECT_NE(line.find("gpuSpan=50/50us"), std::string::npos) << line;
    EXPECT_NE(line.find("disjoint=1"), std::string::npos) << line;
}

TEST(EncoderGpuStageProbeTest, MeasuresAStageThatCompletedBeforeTheSubmit) {
    WarpStage warp;
    ASSERT_TRUE(warp.Create());
    stage::Probe probe;
    stage::Sample sample;
    RunCompletedSample(probe, warp, 5000, &sample);
    EXPECT_EQ(sample.readiness, stage::Readiness::BeforeSend);
    EXPECT_EQ(sample.submitToReadyUs, 500u);  // End at 1000, observed ready at 1500, 1 MHz clock
    EXPECT_EQ(sample.sendUs, 700u);
    EXPECT_GE(sample.gpuSpanUs, 0.0);
}

TEST(EncoderGpuStageProbeTest, SamplesAtMostOncePerInterval) {
    WarpStage warp;
    ASSERT_TRUE(warp.Create());
    stage::Probe probe;
    stage::Sample sample;
    RunCompletedSample(probe, warp, 5000, &sample);
    EXPECT_FALSE(probe.Begin(warp.device.Get(), warp.context.Get(), 5000 + stage::kSampleIntervalMs - 1));
    RunCompletedSample(probe, warp, 5000 + stage::kSampleIntervalMs, &sample);
}

TEST(EncoderGpuStageProbeTest, AbandonsSamplesOfFramesThatNeverReachedTheSubmit) {
    WarpStage warp;
    ASSERT_TRUE(warp.Create());
    stage::Probe probe;
    // Failed between Begin and End (e.g. the conversion failed).
    ASSERT_TRUE(probe.Begin(warp.device.Get(), warp.context.Get(), 1000));
    EXPECT_FALSE(probe.Begin(warp.device.Get(), warp.context.Get(), 1500));
    EXPECT_TRUE(probe.Idle());
    // Failed between End and the submit.
    ASSERT_TRUE(probe.Begin(warp.device.Get(), warp.context.Get(), 2000));
    probe.End(warp.context.Get(), 10);
    stage::Sample sample;
    EXPECT_FALSE(probe.Collect(warp.context.Get(), 20, 1000000, &sample));
    RunCompletedSample(probe, warp, 3000, &sample);
    EXPECT_EQ(sample.readiness, stage::Readiness::BeforeSend);
}

TEST(EncoderGpuStageProbeTest, RecreatesItsQueriesWhenTheEncoderDeviceChanges) {
    WarpStage first;
    WarpStage second;
    ASSERT_TRUE(first.Create());
    ASSERT_TRUE(second.Create());
    stage::Probe probe;
    stage::Sample sample;
    RunCompletedSample(probe, first, 1000, &sample);
    // Queries of the first device must not be issued on the second device's context.
    RunCompletedSample(probe, second, 2000, &sample);
    EXPECT_EQ(sample.readiness, stage::Readiness::BeforeSend);
}

TEST(EncoderGpuStageProbeTest, EncoderBracketsBothPathsAroundTheirGpuWorkAndSubmit) {
    const auto root = std::filesystem::current_path() / "mediaengine" / "video";
    const std::string encode = ce::test_source::ReadLogicalSource(root / "video_encoder_encode.cpp");
    const std::string repeat = ce::test_source::ReadLogicalSource(root / "video_encoder_framegrab.cpp");
    ASSERT_FALSE(encode.empty());
    ASSERT_FALSE(repeat.empty());

    const size_t freshBegin = encode.find("BeginStage(ce::media::gpu_stage::Path::Fresh");
    const size_t convert = encode.find("ConvertFrameToYuv(bgraTextureGuard.get()");
    const size_t freshEnd = encode.find("EndStage(ce::media::gpu_stage::Path::Fresh");
    const size_t freshBefore = encode.find("BeforeSend(ce::media::gpu_stage::Path::Fresh");
    const size_t freshSend = encode.find("avcodec_send_frame(codecCtx, sendTarget)");
    const size_t freshAfter = encode.find("AfterSend(ce::media::gpu_stage::Path::Fresh");
    ASSERT_NE(freshBegin, std::string::npos);
    EXPECT_LT(freshBegin, convert);
    EXPECT_LT(convert, freshEnd);
    EXPECT_LT(freshBefore, freshSend);
    EXPECT_LT(freshSend, freshAfter);
    EXPECT_NE(encode.find("LogGpuStageTiming(ce::media::gpu_stage::Path::Fresh)"), std::string::npos);

    const size_t repeatBegin = repeat.find("BeginStage(ce::media::gpu_stage::Path::Repeat");
    const size_t repeatCopy = repeat.find("CopyResource(reinterpret_cast<ID3D11Texture2D*>(d3d11Frame->data[0]), "
                                          "repeatFrameTexture)");
    const size_t repeatEnd = repeat.find("EndStage(ce::media::gpu_stage::Path::Repeat");
    const size_t repeatBefore = repeat.find("BeforeSend(ce::media::gpu_stage::Path::Repeat");
    const size_t repeatAfter = repeat.find("AfterSend(ce::media::gpu_stage::Path::Repeat");
    ASSERT_NE(repeatBegin, std::string::npos);
    EXPECT_LT(repeatBegin, repeatCopy);
    EXPECT_LT(repeatCopy, repeatEnd);
    EXPECT_LT(repeatEnd, repeatBefore);
    EXPECT_NE(repeat.find("avcodec_send_frame(codecCtx, frame)", repeatBefore), std::string::npos);
    EXPECT_LT(repeat.find("avcodec_send_frame(codecCtx, frame)", repeatBefore), repeatAfter);
    EXPECT_NE(repeat.find("LogGpuStageTiming(ce::media::gpu_stage::Path::Repeat)"), std::string::npos);
}
