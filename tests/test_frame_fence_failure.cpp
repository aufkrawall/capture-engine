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

TEST(FrameFenceFailureTest, RemovedFenceReturnsBeforeGpuWaitAndWaitErrorsPropagate) {
    const std::string source = ce::test_source::ReadLogicalSource(
        std::filesystem::current_path() / "mediaengine" / "video" / "video_encoder_encode.cpp");
    const size_t begin = source.find("bool VideoEncoder::WaitForFrameFence(");
    const size_t end = source.find("bool VideoEncoder::ConvertFrameToYuv", begin);
    ASSERT_NE(begin, std::string::npos);
    ASSERT_NE(end, std::string::npos);
    const std::string body = source.substr(begin, end - begin);
    const size_t removed = body.find("readiness == ce::frame_fence::Readiness::Removed");
    const size_t defer = body.find("lastFrameDeferred.store(true", removed);
    const size_t reject = body.find("return false;", defer);
    const size_t wait = body.find("hr = d3d11Context->Wait(");
    ASSERT_NE(removed, std::string::npos);
    ASSERT_NE(defer, std::string::npos);
    ASSERT_NE(reject, std::string::npos);
    ASSERT_NE(wait, std::string::npos);
    EXPECT_LT(removed, defer);
    EXPECT_LT(defer, reject);
    EXPECT_LT(reject, wait);
    EXPECT_NE(body.find("hr = d3d11Fence->SetEventOnCompletion"), std::string::npos);
    EXPECT_NE(body.find("hr = HRESULT_FROM_WIN32(GetLastError())"), std::string::npos);
    EXPECT_NE(body.find("waitRes == WAIT_FAILED"), std::string::npos);
    EXPECT_NE(body.find("if (SUCCEEDED(hr)) {", reject), std::string::npos);
    EXPECT_NE(body.find("if (FAILED(hr))", wait), std::string::npos);
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
