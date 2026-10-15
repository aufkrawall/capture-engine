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
    // The encoder polls right after its own submit.
    const size_t submit = send.find("avcodec_send_frame(codecCtx, frame)");
    const size_t poll = send.find("PollSourceReadCompletion();", submit);
    ASSERT_NE(submit, std::string::npos);
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
