#include <gtest/gtest.h>

#include <d3d11.h>

#include <cstdint>
#include <memory>
#include <string>

#include "common/ipc/av_sync_latency_channel.h"
#include "mediaengine/audio/audio_latency_probe.h"
#include "mediaengine/engine/mediaengine_instance.h"
#include "mediaengine/video/encoder_auto_probe.h"
#include "mediaengine/video/gpu_memory_breakdown.h"
#include "mediaengine/video/handle_failure_cache.h"
#include "mediaengine/video/video_encoder.h"
#include "video_encoder_test_access.h"

namespace {

HANDLE FakeHandle(uintptr_t value) {
    return reinterpret_cast<HANDLE>(value);
}

// The reference count before this call; the probe reference is taken and dropped again.
ULONG CurrentReferenceCount(ID3D11Device* device) {
    const ULONG withProbe = device->AddRef();
    device->Release();
    return withProbe - 1;
}

TEST(MediaEngineInstanceTest, SourceReadSequencersAreIndependent) {
    MediaEngineInstance first;
    MediaEngineInstance second;
    const uint64_t sequence = first.injectSourceReads.Issue();
    first.injectSourceReads.CompleteThrough(sequence);

    EXPECT_TRUE(first.injectSourceReads.IsComplete(sequence));
    EXPECT_EQ(second.injectSourceReads.Issued(), 0u);
    EXPECT_FALSE(second.injectSourceReads.IsComplete(sequence));
}

TEST(MediaEngineInstanceTest, CaptureGaugesBelongToTheirPipeline) {
    MediaEngineInstance first;
    MediaEngineInstance second;
    first.captureGauges.reservoirBytes.store(1024u);
    first.captureGauges.sharedAcrossDevices.store(true);

    ce::gpu_memory::Breakdown fromFirst;
    ce::gpu_memory::LoadCaptureReport(first.captureGauges, &fromFirst);
    EXPECT_EQ(fromFirst.captureReservoir, 1024u);
    EXPECT_TRUE(fromFirst.captureSharedAcrossDevices);

    ce::gpu_memory::Breakdown fromSecond;
    ce::gpu_memory::LoadCaptureReport(second.captureGauges, &fromSecond);
    EXPECT_EQ(fromSecond.captureReservoir, 0u);
    EXPECT_FALSE(fromSecond.captureSharedAcrossDevices);
}

// The session latency channel reaches the render probe as an argument (MediaEngine_MeasureRenderEndpointLatency),
// so only a compatible block is ever read by it.
TEST(MediaEngineInstanceTest, SessionLatencyChannelIsAcceptedOnlyWhenCompatible) {
    auto block = std::make_unique<ce::av_sync::LatencyChannelBlock>();
    ce::av_sync::InitLatencyChannel(*block);
    EXPECT_EQ(ce::audio::AcceptRenderLatencyChannel(block.get()), block.get());

    block->magic = ce::av_sync::kLatencyChannelMagic ^ 0xFFu;
    EXPECT_EQ(ce::audio::AcceptRenderLatencyChannel(block.get()), nullptr);
}

TEST(MediaEngineInstanceTest, AutoEncoderResolutionsAreRememberedPerPipeline) {
    ce::media::AutoEncoderLastResolutions first;
    ce::media::AutoEncoderLastResolutions second;
    first.Remember("H264_AUTO", "h264_remembered_for_test");

    std::string encoder = "untouched";
    ASSERT_TRUE(first.Recall("h264_auto", &encoder));
    EXPECT_EQ(encoder, "h264_remembered_for_test");
    EXPECT_FALSE(second.Recall("h264_auto", &encoder));
    EXPECT_EQ(encoder, "h264_remembered_for_test");

    // A provisional name follows the pipeline that resolved it.
    const std::string remembered = ce::media::ProvisionalAutoEncoder("h264_auto", first);
    EXPECT_EQ(remembered, "h264_remembered_for_test");
    const std::string fresh = ce::media::ProvisionalAutoEncoder("h264_auto", second);
    EXPECT_NE(fresh, "h264_remembered_for_test");
    EXPECT_EQ(fresh.rfind("h264_", 0), 0u) << fresh;
}

TEST(MediaEngineInstanceTest, VideoEncodersKeepTheirFailureCachesAndStatisticsApart) {
    MediaEngineInstance firstInstance;
    MediaEngineInstance secondInstance;
    VideoEncoder first(firstInstance);
    VideoEncoder second(secondInstance);

    // Three failures make the handle permanently skipped (HandleFailureCache::ShouldSkipFence).
    const HANDLE handle = FakeHandle(0x1234);
    for (int i = 0; i < 3; ++i)
        VideoEncoderTestAccess::HandleFailures(first).RecordFenceFailure(handle);
    EXPECT_TRUE(VideoEncoderTestAccess::HandleFailures(first).ShouldSkipFence(handle));
    EXPECT_FALSE(VideoEncoderTestAccess::HandleFailures(second).ShouldSkipFence(handle));

    VideoEncoderTestAccess::FrameAnalysis(first).framesEncoded = 7;
    VideoEncoderTestAccess::FrameAnalysis(first).slowFrameCount = 2;
    EXPECT_EQ(VideoEncoderTestAccess::FrameAnalysis(second).framesEncoded, 0);
    EXPECT_EQ(VideoEncoderTestAccess::FrameAnalysis(second).slowFrameCount, 0);
    EXPECT_EQ(VideoEncoderTestAccess::FrameAnalysis(second).lastFramePts, -1);
}

TEST(MediaEngineInstanceTest, ReleaseSharedDeviceDropsTheConverterAndTheDeviceReferences) {
    // The converter holds its own device reference (and its shaders hold more); releasing the shared device
    // must drop the converter's and the instance's references. The exact count is not asserted: D3D11
    // child objects and the immediate context hold references of their own.
    MediaEngineInstance instance;
    if (FAILED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                 D3D11_SDK_VERSION, &instance.sharedDevice, nullptr, &instance.sharedContext))) {
        GTEST_SKIP() << "no WARP D3D11 device";
    }
    ID3D11Device* device = instance.sharedDevice;
    device->AddRef();  // this test's reference, so the device stays observable after the release
    ASSERT_TRUE(instance.retainedConverter.EnsureKernels(device));
    const ULONG withConverter = CurrentReferenceCount(device);

    instance.ReleaseSharedDevice();
    EXPECT_EQ(instance.sharedDevice, nullptr);
    EXPECT_EQ(instance.sharedContext, nullptr);
    EXPECT_LE(CurrentReferenceCount(device) + 2, withConverter);
    device->Release();
}

}  // namespace
