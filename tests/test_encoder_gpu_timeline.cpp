#include <gtest/gtest.h>
#include <d3d11.h>
#include <wrl/client.h>

#include <filesystem>
#include <string>

#include "mediaengine/video/encoder_gpu_stage_probe.h"
#include "mediaengine/video/encoder_gpu_timeline.h"
#include "source_fragment_reader.h"

namespace stage = ce::media::gpu_stage;
using Microsoft::WRL::ComPtr;

namespace {

struct WarpTimeline {
    ComPtr<ID3D11Device> device;
    ComPtr<ID3D11DeviceContext> context;
    ComPtr<ID3D11Texture2D> target;
    ComPtr<ID3D11Texture2D> staging;

    bool Create() {
        if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                     &device, nullptr, &context)))
            return false;
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = desc.Height = 64;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        if (FAILED(device->CreateTexture2D(&desc, nullptr, &target)))
            return false;
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        return SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging));
    }

    // Blocks until the GPU executed everything queued so far, without sleeping.
    void Drain() {
        context->CopyResource(staging.Get(), target.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        ASSERT_EQ(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped), S_OK);
        context->Unmap(staging.Get(), 0);
    }
};

}  // namespace

TEST(EncoderGpuTimelineTest, StepsPairGpuGapsWithTheirSubmissionGaps) {
    // 1 GHz GPU clock, 10 MHz QPC.
    const uint64_t gpu[stage::kTimelinePointCount] = {1000000, 1061000, 1061000, 8061000, 8100000};
    const int64_t cpu[stage::kTimelinePointCount] = {100, 2600, 2700, 84700, 85000};
    const stage::TimelineSample sample = stage::MakeTimelineSample(gpu, 1000000000ull, false, cpu, 10000000);
    EXPECT_FALSE(sample.disjoint);
    EXPECT_DOUBLE_EQ(sample.gpuUs[0], 61.0);
    EXPECT_DOUBLE_EQ(sample.gpuUs[1], 0.0);
    EXPECT_DOUBLE_EQ(sample.gpuUs[2], 7000.0);
    EXPECT_DOUBLE_EQ(sample.gpuUs[3], 39.0);
    EXPECT_EQ(sample.cpuUs[0], 250u);
    EXPECT_EQ(sample.cpuUs[1], 10u);
    EXPECT_EQ(sample.cpuUs[2], 8200u);
    EXPECT_EQ(sample.cpuUs[3], 30u);

    // A timestamp running backwards counts as zero instead of wrapping; a
    // disjoint bracket keeps the CPU side only.
    const uint64_t backwards[stage::kTimelinePointCount] = {10, 5, 20, 30, 40};
    EXPECT_DOUBLE_EQ(stage::MakeTimelineSample(backwards, 1000000000ull, false, cpu, 10000000).gpuUs[0], 0.0);
    const stage::TimelineSample disjoint = stage::MakeTimelineSample(gpu, 1000000000ull, true, cpu, 10000000);
    EXPECT_TRUE(disjoint.disjoint);
    EXPECT_EQ(disjoint.cpuUs[2], 8200u);
}

TEST(EncoderGpuTimelineTest, WindowReportsEveryStepAsGpuAndCpuAverageAndMax) {
    const uint64_t gpu[stage::kTimelinePointCount] = {0, 60000, 60000, 7060000, 7100000};
    const int64_t cpu[stage::kTimelinePointCount] = {0, 2000, 2000, 82000, 82400};
    stage::TimelineWindow window;
    window.Add(stage::MakeTimelineSample(gpu, 1000000000ull, false, cpu, 10000000), 1000);
    window.Add(stage::MakeTimelineSample(gpu, 1000000000ull, true, cpu, 10000000), 2000);
    EXPECT_FALSE(window.Due(2000));
    EXPECT_TRUE(window.Due(1000 + stage::kWindowMaxAgeMs));
    const std::string line = window.TakeLine(stage::Path::Fresh);
    EXPECT_NE(line.find("[VideoEncoder GPU Timeline] path=fresh samples=2 disjoint=1"), std::string::npos) << line;
    EXPECT_NE(line.find("convert=60/60|200/200"), std::string::npos) << line;
    EXPECT_NE(line.find("sourceRead=0/0|0/0"), std::string::npos) << line;
    EXPECT_NE(line.find("submit=7000/7000|8000/8000"), std::string::npos) << line;
    EXPECT_NE(line.find("next=40/40|40/40"), std::string::npos) << line;
    EXPECT_EQ(window.Count(), 0u);
}

TEST(EncoderGpuTimelineTest, StampsOneFrameAndTheNextFrameOfAnyPathOnTheGpu) {
    WarpTimeline warp;
    ASSERT_TRUE(warp.Create());
    stage::Timeline timeline;
    stage::TimelineSample sample;
    stage::Path path = stage::Path::Repeat;
    // Repeats are never sampled, only stamped as the next frame.
    timeline.OnStageBegin(stage::Path::Repeat, warp.device.Get(), warp.context.Get(), 5000, 10);
    timeline.Mark(stage::Path::Repeat, stage::TimelinePoint::Converted, warp.context.Get(), 20);
    EXPECT_FALSE(timeline.Collect(warp.context.Get(), 1000000, &sample, &path));

    timeline.OnStageBegin(stage::Path::Fresh, warp.device.Get(), warp.context.Get(), 5000, 100);
    // Out-of-order marks and other paths' marks are ignored.
    timeline.Mark(stage::Path::Fresh, stage::TimelinePoint::Done, warp.context.Get(), 150);
    timeline.Mark(stage::Path::ScreenGrab, stage::TimelinePoint::Converted, warp.context.Get(), 160);
    timeline.Mark(stage::Path::Fresh, stage::TimelinePoint::Converted, warp.context.Get(), 200);
    timeline.Mark(stage::Path::Fresh, stage::TimelinePoint::SourceRead, warp.context.Get(), 250);
    timeline.Mark(stage::Path::Fresh, stage::TimelinePoint::Done, warp.context.Get(), 9000);
    EXPECT_FALSE(timeline.Collect(warp.context.Get(), 1000000, &sample, &path));
    timeline.OnStageBegin(stage::Path::Repeat, warp.device.Get(), warp.context.Get(), 5008, 9400);
    warp.Drain();
    ASSERT_TRUE(timeline.Collect(warp.context.Get(), 1000000, &sample, &path));
    EXPECT_EQ(path, stage::Path::Fresh);
    EXPECT_EQ(sample.cpuUs[0], 100u);
    EXPECT_EQ(sample.cpuUs[1], 50u);
    EXPECT_EQ(sample.cpuUs[2], 8750u);
    EXPECT_EQ(sample.cpuUs[3], 400u);
    for (double gpuUs : sample.gpuUs)
        EXPECT_GE(gpuUs, 0.0);
    // At most one sample per interval.
    timeline.OnStageBegin(stage::Path::Fresh, warp.device.Get(), warp.context.Get(), 5500, 9500);
    timeline.Mark(stage::Path::Fresh, stage::TimelinePoint::Converted, warp.context.Get(), 9600);
    timeline.Mark(stage::Path::Fresh, stage::TimelinePoint::SourceRead, warp.context.Get(), 9700);
    timeline.Mark(stage::Path::Fresh, stage::TimelinePoint::Done, warp.context.Get(), 9800);
    timeline.OnStageBegin(stage::Path::Fresh, warp.device.Get(), warp.context.Get(), 5600, 9900);
    warp.Drain();
    EXPECT_FALSE(timeline.Collect(warp.context.Get(), 1000000, &sample, &path));
}

TEST(EncoderGpuTimelineTest, DropsTheSampleOfAFrameThatFailedBeforeDone) {
    WarpTimeline warp;
    ASSERT_TRUE(warp.Create());
    stage::Timeline timeline;
    stage::TimelineSample sample;
    stage::Path path = stage::Path::Repeat;
    timeline.OnStageBegin(stage::Path::ScreenGrab, warp.device.Get(), warp.context.Get(), 1000, 10);
    timeline.Mark(stage::Path::ScreenGrab, stage::TimelinePoint::Converted, warp.context.Get(), 20);
    // The next frame begins without Done: the sample is dropped, and a new one
    // starts only once the interval has elapsed.
    timeline.OnStageBegin(stage::Path::ScreenGrab, warp.device.Get(), warp.context.Get(), 1008, 30);
    warp.Drain();
    EXPECT_FALSE(timeline.Collect(warp.context.Get(), 1000000, &sample, &path));
    timeline.OnStageBegin(stage::Path::ScreenGrab, warp.device.Get(), warp.context.Get(),
                          1000 + stage::kSampleIntervalMs, 40);
    timeline.Mark(stage::Path::ScreenGrab, stage::TimelinePoint::Converted, warp.context.Get(), 50);
    timeline.Mark(stage::Path::ScreenGrab, stage::TimelinePoint::SourceRead, warp.context.Get(), 50);
    timeline.Mark(stage::Path::ScreenGrab, stage::TimelinePoint::Done, warp.context.Get(), 60);
    timeline.OnStageBegin(stage::Path::Repeat, warp.device.Get(), warp.context.Get(), 2100, 70);
    warp.Drain();
    ASSERT_TRUE(timeline.Collect(warp.context.Get(), 1000000, &sample, &path));
    EXPECT_EQ(path, stage::Path::ScreenGrab);
    EXPECT_EQ(sample.cpuUs[1], 0u);
}

TEST(EncoderGpuTimelineTest, EncoderMarksSourceReadAndDoneAroundTheSubmit) {
    const auto root = std::filesystem::current_path() / "mediaengine" / "video";
    const std::string encode = ce::test_source::ReadLogicalSource(root / "video_encoder_encode.cpp");
    const std::string framegrab = ce::test_source::ReadLogicalSource(root / "video_encoder_framegrab.cpp");
    const std::string timing = ce::test_source::ReadLogicalSource(root / "video_encoder_gpu_timing.cpp");
    ASSERT_FALSE(encode.empty());
    ASSERT_FALSE(framegrab.empty());
    ASSERT_FALSE(timing.empty());

    const size_t complete = encode.find("sourceReadCompletion.Complete();");
    const size_t sourceRead = encode.find("MarkSourceRead(ce::media::gpu_stage::Path::Fresh", complete);
    const size_t submit = encode.find("SubmitFrameForEncode(d3d11Frame", complete);
    const size_t cache = encode.find("CacheRepeatFrameTexture(", submit);
    const size_t done = encode.find("MarkFrameDone(ce::media::gpu_stage::Path::Fresh", cache);
    ASSERT_NE(complete, std::string::npos);
    ASSERT_NE(sourceRead, std::string::npos);
    ASSERT_NE(submit, std::string::npos);
    ASSERT_NE(cache, std::string::npos);
    ASSERT_NE(done, std::string::npos);
    EXPECT_LT(sourceRead, submit);

    const size_t grabSend =
        framegrab.find("SendFrameToEncoder(encoderInputFrame, ce::media::gpu_stage::Path::ScreenGrab");
    const size_t grabCache = framegrab.find("CacheRepeatFrameTexture(", grabSend);
    const size_t grabDone = framegrab.find("MarkFrameDone(ce::media::gpu_stage::Path::ScreenGrab", grabCache);
    ASSERT_NE(grabSend, std::string::npos);
    ASSERT_NE(grabCache, std::string::npos);
    EXPECT_NE(grabDone, std::string::npos);

    EXPECT_NE(timing.find("gpuStageMonitor.TakeTimelineLine(path, d3d11Context, &timelineLine, flush)"),
              std::string::npos);
}
