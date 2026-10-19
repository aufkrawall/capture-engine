#include <gtest/gtest.h>
#include <d3d11_4.h>
#include <wrl/client.h>

#include <deque>
#include <filesystem>
#include <string>

#include "captureengine/media/inject_source_read.h"
#include "mediaengine/video/source_read_markers.h"
#include "source_fragment_reader.h"

namespace {

using Microsoft::WRL::ComPtr;
using ce::media::SourceReadMarker;
using ce::media::SourceReadSequencer;

void NoLock() {}

class SourceReadMarkersTest : public testing::Test {
protected:
    void SetUp() override {
        ASSERT_EQ(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0, nullptr, 0, D3D11_SDK_VERSION,
                                    &device_, nullptr, &context_),
                  S_OK);
        D3D11_TEXTURE2D_DESC desc = {};
        desc.Width = desc.Height = 64;
        desc.MipLevels = desc.ArraySize = desc.SampleDesc.Count = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET;
        ASSERT_EQ(device_->CreateTexture2D(&desc, nullptr, &target_), S_OK);
        desc.Usage = D3D11_USAGE_STAGING;
        desc.BindFlags = 0;
        desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        ASSERT_EQ(device_->CreateTexture2D(&desc, nullptr, &staging_), S_OK);
        wake_ = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        ASSERT_NE(wake_, nullptr);
    }
    void TearDown() override {
        for (SourceReadMarker& marker : markers_)
            ce::media::ReleaseSourceReadMarker(marker);
        if (wake_)
            CloseHandle(wake_);
    }

    // The encoder's SignalSourceReadCompletion: End a query, Flush, queue it.
    uint64_t AddMarker() {
        ComPtr<ID3D11Query> query;
        const D3D11_QUERY_DESC desc{D3D11_QUERY_EVENT, 0};
        EXPECT_EQ(device_->CreateQuery(&desc, &query), S_OK);
        context_->End(query.Get());
        context_->Flush();
        SourceReadMarker marker;
        marker.sequence = sequencer_.Issue();
        marker.query = query.Detach();
        marker.context = context_.Get();
        marker.context->AddRef();
        markers_.push_back(marker);
        return marker.sequence;
    }

    // Blocks until the GPU executed everything queued so far, without sleeping.
    void DrainGpu() {
        context_->CopyResource(staging_.Get(), target_.Get());
        D3D11_MAPPED_SUBRESOURCE mapped = {};
        ASSERT_EQ(context_->Map(staging_.Get(), 0, D3D11_MAP_READ, 0, &mapped), S_OK);
        context_->Unmap(staging_.Get(), 0);
    }

    ComPtr<ID3D11Device> device_;
    ComPtr<ID3D11DeviceContext> context_;
    ComPtr<ID3D11Texture2D> target_;
    ComPtr<ID3D11Texture2D> staging_;
    SourceReadSequencer sequencer_;
    std::deque<SourceReadMarker> markers_;
    HANDLE wake_ = nullptr;
};

}  // namespace

TEST(SourceReadSequencerTest, CompletionIsMonotonicAndWakesOnlyWhenItAdvances) {
    SourceReadSequencer sequencer;
    HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ASSERT_NE(wake, nullptr);
    sequencer.SetWakeEvent(wake);
    EXPECT_EQ(sequencer.Issue(), 1u);
    EXPECT_EQ(sequencer.Issue(), 2u);
    EXPECT_FALSE(sequencer.IsComplete(0));
    EXPECT_FALSE(sequencer.IsComplete(1));

    sequencer.CompleteThrough(1);
    EXPECT_TRUE(sequencer.IsComplete(1));
    EXPECT_FALSE(sequencer.IsComplete(2));
    EXPECT_EQ(WaitForSingleObject(wake, 0), static_cast<DWORD>(WAIT_OBJECT_0));
    // Going backwards neither regresses completion nor wakes anyone.
    sequencer.CompleteThrough(0);
    sequencer.CompleteThrough(1);
    EXPECT_TRUE(sequencer.IsComplete(1));
    EXPECT_EQ(WaitForSingleObject(wake, 0), static_cast<DWORD>(WAIT_TIMEOUT));
    sequencer.CompleteThrough(2);
    EXPECT_TRUE(sequencer.IsComplete(2));
    EXPECT_FALSE(sequencer.IsComplete(0));  // "no read" never counts as a completed read
    EXPECT_EQ(WaitForSingleObject(wake, 0), static_cast<DWORD>(WAIT_OBJECT_0));
    CloseHandle(wake);
}

TEST_F(SourceReadMarkersTest, PollCompletesExecutedMarkersInOrderAndWakesTheReleaser) {
    sequencer_.SetWakeEvent(wake_);
    AddMarker();
    const uint64_t last = AddMarker();
    DrainGpu();
    EXPECT_EQ(ce::media::PollSourceReadMarkers(markers_, sequencer_), 2u);
    EXPECT_TRUE(markers_.empty());
    EXPECT_TRUE(sequencer_.IsComplete(last));
    EXPECT_EQ(WaitForSingleObject(wake_, 0), static_cast<DWORD>(WAIT_OBJECT_0));
    // Nothing left: polling again neither completes nor wakes.
    EXPECT_EQ(ce::media::PollSourceReadMarkers(markers_, sequencer_), 0u);
    EXPECT_EQ(WaitForSingleObject(wake_, 0), static_cast<DWORD>(WAIT_TIMEOUT));
}

TEST_F(SourceReadMarkersTest, TeardownWaitsForOutstandingMarkersWithoutAnEncoderPoll) {
    const uint64_t first = AddMarker();
    const uint64_t last = AddMarker();
    EXPECT_TRUE(ce::media::FinishSourceReadMarkers(markers_, sequencer_, 5000, NoLock, NoLock));
    EXPECT_TRUE(markers_.empty());
    EXPECT_TRUE(sequencer_.IsComplete(first));
    EXPECT_TRUE(sequencer_.IsComplete(last));
    // Nothing outstanding is finished at once.
    EXPECT_TRUE(ce::media::FinishSourceReadMarkers(markers_, sequencer_, 0, NoLock, NoLock));
}

TEST(InjectSourceReadTokenTest, CompletionAndWakeGoThroughTheEncoderSequenceOnly) {
    static uint64_t s_completed = 0;
    static HANDLE s_wake = nullptr;
    auto isComplete = [](uint64_t sequence) { return sequence <= s_completed; };
    auto setWake = [](HANDLE event) { s_wake = event; };
    s_completed = 4;
    s_wake = nullptr;

    InjectSourceReadToken token(5, isComplete, setWake);
    EXPECT_EQ(token.Value(), 5u);
    EXPECT_FALSE(token.Completed());
    HANDLE event = reinterpret_cast<HANDLE>(0x1234);
    EXPECT_TRUE(token.ArmEvent(event));
    EXPECT_EQ(s_wake, event);
    s_completed = 5;
    EXPECT_TRUE(token.Completed());

    // A token without a sequence or without the encoder exports never completes.
    EXPECT_FALSE(InjectSourceReadToken(0, isComplete, setWake).Completed());
    EXPECT_FALSE(InjectSourceReadToken(3, nullptr, nullptr).Completed());
    EXPECT_FALSE(InjectSourceReadToken(3, nullptr, nullptr).ArmEvent(event));
}

namespace {

std::string FunctionBody(const std::string& source, const char* signature) {
    const size_t begin = source.find(signature);
    if (begin == std::string::npos)
        return {};
    const size_t open = source.find('{', begin);
    int depth = 0;
    for (size_t at = open; at < source.size(); ++at) {
        if (source[at] == '{')
            ++depth;
        else if (source[at] == '}' && --depth == 0)
            return source.substr(open, at - open + 1);
    }
    return {};
}

}  // namespace

// Every extra GPU submission CE forced ahead of the encoder submit waited about
// one encode for NVENC on NVIDIA (session 20261016_010712), so nothing on the
// per-frame path flushes; the encoder submit carries CE's work and markers.
TEST(InjectSourceReadTokenTest, PerFrameEncoderPathForcesNoGpuSubmission) {
    const auto video = std::filesystem::current_path() / "mediaengine" / "video";
    const std::string sourceRead = ce::test_source::ReadLogicalSource(video / "video_encoder_source_read.cpp");
    const std::string probe = ce::test_source::ReadLogicalSource(video / "encoder_gpu_stage_probe.cpp");
    const std::string timeline = ce::test_source::ReadLogicalSource(video / "encoder_gpu_timeline.cpp");
    const std::string signal = FunctionBody(sourceRead, "void VideoEncoder::SignalSourceReadCompletion()");
    const std::string probeEnd = FunctionBody(probe, "void Probe::End(");
    const std::string stageBegin = FunctionBody(timeline, "void Timeline::OnStageBegin(");
    ASSERT_FALSE(signal.empty());
    ASSERT_FALSE(probeEnd.empty());
    ASSERT_FALSE(stageBegin.empty());
    EXPECT_NE(signal.find("d3d11Context->End(sourceReadQuery);"), std::string::npos);
    EXPECT_EQ(signal.find("Flush("), std::string::npos);
    EXPECT_EQ(signal.find("->Signal("), std::string::npos);
    EXPECT_EQ(probeEnd.find("Flush("), std::string::npos);
    EXPECT_EQ(stageBegin.find("Flush("), std::string::npos);
}

TEST(InjectSourceReadTokenTest, OnlyTheEncoderThreadTouchesTheImmediateContext) {
    const auto root = std::filesystem::current_path();
    const std::string token = ce::test_source::ReadFile(root / "captureengine" / "media" / "inject_source_read.h");
    const std::string send =
        ce::test_source::ReadLogicalSource(root / "mediaengine" / "video" / "video_encoder_send.cpp");
    const std::string stop =
        ce::test_source::ReadLogicalSource(root / "captureengine" / "media" / "media_main_start.cpp");
    ASSERT_FALSE(token.empty());
    ASSERT_FALSE(send.empty());
    ASSERT_FALSE(stop.empty());
    // The releasing threads see completion only through the sequence.
    EXPECT_EQ(token.find("GetData"), std::string::npos);
    EXPECT_EQ(token.find("Flush1"), std::string::npos);
    EXPECT_EQ(token.find("ID3D11"), std::string::npos);
    // The encoder polls right after its own submit (sent now or gated).
    const size_t entry = send.find("bool VideoEncoder::SendFrameToEncoder(");
    const size_t dispatch = send.find("SendFrameNow(frame, path", entry);
    const size_t poll = send.find("PollSourceReadCompletion();", dispatch);
    ASSERT_NE(entry, std::string::npos);
    ASSERT_NE(dispatch, std::string::npos);
    EXPECT_NE(send.find("avcodec_send_frame(codecCtx, frame)"), std::string::npos);
    EXPECT_NE(poll, std::string::npos);
    // Teardown finishes the encoder's markers after the encoder thread joined
    // and before the release queue drains.
    const size_t join = stop.find("JoinThreadWithTimeout(media_main_g_EncoderThread");
    const size_t finish = stop.find("MediaEngine_FinishInjectSourceReads(5000)", join);
    const size_t drain = stop.find("media_main_g_InjectSourceReadReleases.Drain(5000)", finish);
    ASSERT_NE(join, std::string::npos);
    ASSERT_NE(finish, std::string::npos);
    EXPECT_NE(drain, std::string::npos);
    EXPECT_NE(stop.find("encoderJoined && MediaEngine_FinishInjectSourceReads"), std::string::npos);
}

// An AV1 show-existing repeat submits nothing, so the poll after encoder submits
// never runs while the source stalls. Talos 20261019_182822 (FSR FG enable): the
// stalled generation's last slot stayed leased for 0.96 s, the producer could not
// replace its generation for the new swapchain and the recording froze while the
// game presented. The repeat path polls on the same thread before it returns.
TEST(InjectSourceReadTokenTest, ShowExistingRepeatsPollTheEncodersSourceReads) {
    const std::string grab = ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "mediaengine" /
                                                                "video" / "video_encoder_framegrab.cpp");
    ASSERT_FALSE(grab.empty());
    const size_t entry = grab.find("bool VideoEncoder::RepeatLastFrame(");
    ASSERT_NE(entry, std::string::npos);
    const size_t repeat = grab.find("av1Repeats.TryRepeat(", entry);
    ASSERT_NE(repeat, std::string::npos);
    const size_t shown = grab.find("return true;", repeat);
    const size_t reencode = grab.find("allocateD3D11RepeatFrame", repeat);
    ASSERT_NE(shown, std::string::npos);
    ASSERT_NE(reencode, std::string::npos);
    ASSERT_LT(shown, reencode) << "the show-existing branch returns before the re-encode";
    const size_t poll = grab.find("PollSourceReadCompletion();", repeat);
    ASSERT_NE(poll, std::string::npos);
    EXPECT_LT(poll, shown);
}
