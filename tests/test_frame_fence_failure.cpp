#include <gtest/gtest.h>

#include <filesystem>
#include <limits>
#include <string>

#include "mediaengine/video/frame_fence_policy.h"
#include "source_fragment_reader.h"

TEST(FrameFenceFailureTest, RemovedFenceIsNeverSuccessfulCompletion) {
    using ce::frame_fence::Classify;
    using ce::frame_fence::Readiness;
    constexpr uint64_t removed = std::numeric_limits<uint64_t>::max();
    EXPECT_EQ(Classify(removed, 0), Readiness::Removed);
    EXPECT_EQ(Classify(removed, 45), Readiness::Removed);
    EXPECT_EQ(Classify(removed, removed), Readiness::Removed);
    EXPECT_EQ(Classify(0, 1), Readiness::Pending);
    EXPECT_EQ(Classify(44, 45), Readiness::Pending);
    EXPECT_EQ(Classify(45, 45), Readiness::Ready);
    EXPECT_EQ(Classify(46, 45), Readiness::Ready);
    EXPECT_EQ(Classify(0, 0), Readiness::Ready);
    // A replacement generation starts its own fence timeline normally.
    EXPECT_EQ(Classify(0, 45), Readiness::Pending);
    EXPECT_EQ(Classify(1, 1), Readiness::Ready);
}

TEST(FrameFenceFailureTest, RemovedFenceRejectsAndCompletionErrorsPropagate) {
    const std::string source = ce::test_source::ReadLogicalSource(
        std::filesystem::current_path() / "mediaengine" / "video" / "video_encoder_encode.cpp");
    const size_t begin = source.find("bool VideoEncoder::WaitForFrameFence(");
    const size_t end = source.find("bool VideoEncoder::ConvertFrameToYuv", begin);
    ASSERT_NE(begin, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    const std::string body = source.substr(begin, end - begin);
    const size_t removed = body.find("readiness == ce::frame_fence::Readiness::Removed");
    const size_t defer = body.find("lastFrameDeferred.store(false", removed);
    const size_t reject = body.find("return false;", defer);
    const size_t failed = body.find("if (FAILED(hr))", reject);
    ASSERT_NE(removed, std::string::npos);
    ASSERT_NE(defer, std::string::npos);
    ASSERT_NE(reject, std::string::npos);
    ASSERT_NE(failed, std::string::npos);
    EXPECT_LT(removed, defer);
    EXPECT_LT(defer, reject);
    EXPECT_NE(body.find("removedFrameTransport.Mark", removed), std::string::npos);
    EXPECT_NE(body.find("cachedD3D11Fence = nullptr", removed), std::string::npos);
    EXPECT_NE(body.find("hr = d3d11Fence->SetEventOnCompletion"), std::string::npos);
    EXPECT_NE(body.find("hr = HRESULT_FROM_WIN32(GetLastError())"), std::string::npos);
    EXPECT_NE(body.find("waitRes == WAIT_FAILED"), std::string::npos);
}

// The frame proceeds only once the CPU saw the producer's value complete
// (ready on entry, or the zero-timeout event fired); a pending fence defers
// the frame. A GPU Wait on that already-satisfied value only added a scheduler
// sync point per fresh frame (session 20261015_223320).
TEST(FrameFenceFailureTest, CpuVerifiedFenceQueuesNoGpuWait) {
    const std::string source = ce::test_source::ReadLogicalSource(
        std::filesystem::current_path() / "mediaengine" / "video" / "video_encoder_encode.cpp");
    const size_t begin = source.find("bool VideoEncoder::WaitForFrameFence(");
    const size_t end = source.find("bool VideoEncoder::ConvertFrameToYuv", begin);
    ASSERT_NE(begin, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    const std::string body = source.substr(begin, end - begin);
    const size_t pending = body.find("readiness == ce::frame_fence::Readiness::Pending");
    const size_t poll = body.find("WaitForSingleObject(fenceEvent, 0)", pending);
    const size_t timeout = body.find("waitRes == WAIT_TIMEOUT", poll);
    const size_t deferred = body.find("lastFrameDeferred.store(true", timeout);
    ASSERT_NE(pending, std::string::npos);
    ASSERT_NE(poll, std::string::npos);
    ASSERT_NE(timeout, std::string::npos);
    ASSERT_NE(deferred, std::string::npos);
    EXPECT_NE(body.find("return false;", deferred), std::string::npos);
    EXPECT_EQ(body.find("->Wait("), std::string::npos);
}

TEST(FrameFenceFailureTest, RemovedTransportStaysFailedUntilAnActualPublicationChangesIdentity) {
    ce::frame_fence::RemovedTransport transport;
    EXPECT_FALSE(transport.Matches(100, 20, 3));
    transport.Mark(100, 20, 3);
    for (unsigned retry = 0; retry < 100; ++retry) EXPECT_TRUE(transport.Matches(100, 20, 3));
    EXPECT_FALSE(transport.Matches(101, 20, 3));
    EXPECT_FALSE(transport.Matches(100, 21, 3));
    EXPECT_FALSE(transport.Matches(100, 20, 4));
}

TEST(FrameFenceFailureTest, FailedRepeatAllocationLogsDeviceReasonAndSuppressedCount) {
    const std::string source = ce::test_source::ReadLogicalSource(
        std::filesystem::current_path() / "mediaengine" / "video" / "video_encoder_framegrab.cpp");
    ASSERT_FALSE(source.empty());
    EXPECT_NE(source.find("ce::log_meter::ChangeGate allocationFailureLog(1000)"), std::string::npos);
    EXPECT_NE(source.find("d3d11Device->GetDeviceRemovedReason()"), std::string::npos);
    EXPECT_NE(source.find("ce::log_meter::FieldKey(frameRet, deviceReason, cachedTransportGeneration)"),
              std::string::npos);
    EXPECT_NE(source.find("ce::log_meter::SuppressedNote(log.suppressed).c_str()"), std::string::npos);
}
