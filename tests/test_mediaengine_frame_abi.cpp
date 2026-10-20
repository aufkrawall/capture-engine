#include <gtest/gtest.h>

#include <windows.h>

// These functions are statically linked into the tests and exported by the same
// product translation unit that exports them from mediaengine.dll.
#define MEDIAENGINE_EXPORTS
#include "mediaengine/engine/mediaengine.h"

#include "media_test_pipeline.h"

// DR-12: mediaengine.dll exports are package-private and version-locked, so predecessors with no caller
// left are deleted instead of preserved. Frame submission and repeat are the *WithResultV1 entry points;
// the positional and bool-returning forms, the unused repeat/deferral queries, the DLL's own log line and
// the D3D11 call shims are no exports any more.
TEST(MediaEngineFrameAbiTest, DroppedLegacyExportsStayGone) {
    const HMODULE module = GetModuleHandleW(nullptr);
    for (const char* name : {"MediaEngine_ProcessFrame", "MediaEngine_ProcessFrameD3D11", "MediaEngine_SubmitFrame",
                             "MediaEngine_SubmitFrameD3D11", "MediaEngine_RepeatLastFrame",
                             "MediaEngine_RepeatLastFrameWithTimeline", "MediaEngine_WasLastFrameDeferred",
                             "DLL_Log", "CallOpenSharedFence", "CallOpenSharedResource", "CallOpenSharedResource1",
                             "MediaEngine_Shutdown", "MediaEngine_SetRenderLatencyChannel"}) {
        SCOPED_TRACE(name);
        EXPECT_EQ(GetProcAddress(module, name), nullptr);
    }
    for (const char* name : {"MediaEngine_SubmitFrameWithResultV1", "MediaEngine_SubmitFrameD3D11WithResultV1",
                             "MediaEngine_RepeatLastFrameWithResultV1", "MediaEngine_CanRepeatLastFrame",
                             "MediaEngine_GetBuildIdentity", "MediaEngine_RunProcessLoopbackWorker",
                             "MediaEngine_Create", "MediaEngine_Destroy", "MediaEngine_MeasureRenderEndpointLatency"}) {
        SCOPED_TRACE(name);
        EXPECT_NE(GetProcAddress(module, name), nullptr);
    }
}

// The loader compares this identity with its own before it resolves or calls anything else.
TEST(MediaEngineFrameAbiTest, BuildIdentityExportAnswersThisBuildAndRefusesAnotherLayout) {
    namespace bid = ce::build_identity;
    const HMODULE module = GetModuleHandleW(nullptr);
    const auto identity = reinterpret_cast<bid::ExportFn>(GetProcAddress(module, bid::kMediaEngineExportName));
    ASSERT_NE(identity, nullptr);
    EXPECT_EQ(identity, &MediaEngine_GetBuildIdentity);

    bid::Record record = bid::Request();
    ASSERT_TRUE(identity(&record));
    EXPECT_EQ(bid::Compare(bid::Current(), record), bid::Verdict::Match);

    bid::Record other = bid::Request();
    other.size -= 1;
    other.buildNumber = 99;
    EXPECT_FALSE(identity(&other));
    EXPECT_EQ(other.buildNumber, 99u) << "a refused request is never written to";
    EXPECT_FALSE(identity(nullptr));
}

TEST(MediaEngineFrameAbiTest, VersionedResultExportsValidateStorageAndReportInactiveRejection) {
    using Result = ce::media::FrameSubmissionResultV1;
    using Inject = bool (*)(MediaEngineHandle, const VideoFrameSubmissionDesc*, Result*);
    using D3D11 = bool (*)(MediaEngineHandle, const D3D11FrameSubmissionDesc*, Result*);
    using Repeat = bool (*)(MediaEngineHandle, int64_t, int64_t, const ce::cursor::CaptureState*, Result*);
    MediaTestPipeline pipeline;
    const MediaEngineHandle engine = pipeline.handle();
    const HMODULE module = GetModuleHandleW(nullptr);
    const auto inject = reinterpret_cast<Inject>(GetProcAddress(module, "MediaEngine_SubmitFrameWithResultV1"));
    const auto d3d11 = reinterpret_cast<D3D11>(GetProcAddress(module, "MediaEngine_SubmitFrameD3D11WithResultV1"));
    const auto repeat = reinterpret_cast<Repeat>(GetProcAddress(module, "MediaEngine_RepeatLastFrameWithResultV1"));
    ASSERT_NE(inject, nullptr);
    ASSERT_NE(d3d11, nullptr);
    ASSERT_NE(repeat, nullptr);
    VideoFrameSubmissionDesc injectDesc{};
    D3D11FrameSubmissionDesc screenDesc{};
    Result result;
    EXPECT_FALSE(inject(engine, nullptr, &result));
    EXPECT_FALSE(d3d11(engine, nullptr, &result));
    EXPECT_FALSE(inject(engine, &injectDesc, nullptr));
    EXPECT_FALSE(d3d11(engine, &screenDesc, nullptr));
    EXPECT_FALSE(repeat(engine, 0, -1, nullptr, nullptr));
    for (uint32_t size : {0u, 39u, 41u}) {
        result.size = size;
        result.videoTimelineUs = 123;
        EXPECT_FALSE(inject(engine, &injectDesc, &result));
        EXPECT_FALSE(d3d11(engine, &screenDesc, &result));
        EXPECT_FALSE(repeat(engine, 0, -1, nullptr, &result));
        EXPECT_EQ(result.size, size);
        EXPECT_EQ(result.videoTimelineUs, 123);
    }
    result.size = sizeof(result);
    EXPECT_TRUE(inject(engine, &injectDesc, &result));
    EXPECT_FALSE(result.Accepted());
    EXPECT_EQ(result.output, ce::media::SubmissionOutput::None);
    EXPECT_EQ(result.firstOutputCommitted, 0u);
    EXPECT_EQ(result.videoTimelineUs, -1);
    EXPECT_TRUE(d3d11(engine, &screenDesc, &result));
    EXPECT_FALSE(result.Accepted());
    EXPECT_TRUE(repeat(engine, 0, -1, nullptr, &result));
    EXPECT_FALSE(result.Accepted());
    EXPECT_EQ(result.source, ce::media::SourceDisposition::NoCandidate);
}
