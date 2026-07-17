#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>

namespace {

std::string ReadVideoEncoderSource() {
    const std::filesystem::path source = std::filesystem::current_path() / "mediaengine" / "video_encoder.cpp";
    std::ifstream file(source, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

std::string ReadCursorRendererSource() {
    const std::filesystem::path source = std::filesystem::current_path() / "mediaengine" / "cursor_renderer.cpp";
    std::ifstream file(source, std::ios::binary);
    std::ostringstream contents;
    contents << file.rdbuf();
    return contents.str();
}

}  // namespace

TEST(VideoEncoderSourceTest, CursorShaderCompilesAndCompilerModuleOutlivesReturnedBlobs) {
    const std::string source = ReadCursorRendererSource();
    ASSERT_FALSE(source.empty());

    EXPECT_EQ(source.find("float3 linear ="), std::string::npos);
    EXPECT_NE(source.find("float3 linearRgb ="), std::string::npos);

    const size_t moduleGuard = source.find("ce::ModuleGuard d3dCompiler");
    const size_t vertexBlob = source.find("ce::ComGuard<ID3DBlob> vsBlob", moduleGuard);
    ASSERT_NE(moduleGuard, std::string::npos);
    ASSERT_NE(vertexBlob, std::string::npos);
    EXPECT_LT(moduleGuard, vertexBlob);

    const size_t createResources = source.find("bool CursorRenderer::CreateRenderingResources()");
    ASSERT_NE(createResources, std::string::npos);
    EXPECT_EQ(source.find("FreeLibrary(d3dCompiler)", createResources), std::string::npos);
}

TEST(VideoEncoderSourceTest, RealTimeGpuPathsNeverUseSleepRetries) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    EXPECT_EQ(source.find("Sleep(2)"), std::string::npos);
    EXPECT_NE(source.find("if (FAILED(hr) && hasSharedAlt)"), std::string::npos);
    EXPECT_NE(source.find("dupTexAlt.addressof()"), std::string::npos);
    EXPECT_EQ(source.find("Sleep(1)"), std::string::npos);
    EXPECT_NE(source.find("lastFrameDeferred.store(true"), std::string::npos);
    EXPECT_NE(source.find("Frame-ring publication uses release/acquire ordering"), std::string::npos);
}

TEST(VideoEncoderSourceTest, PostMuxProbeNeverOutlivesMediaEngineCode) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    EXPECT_EQ(source.find("std::thread probeThread"), std::string::npos);
    EXPECT_EQ(source.find("probeFuture.wait_for"), std::string::npos);
    EXPECT_NE(source.find("probeCtx->interrupt_callback.callback = PostMuxProbeInterrupt"), std::string::npos);
    EXPECT_NE(source.find("kPostMuxProbeMaxPackets"), std::string::npos);
    EXPECT_NE(source.find("Run on the already-owned writer/finalizer thread"), std::string::npos);
}

TEST(VideoEncoderSourceTest, EveryCfrSubmissionAdvancesOneContiguousPacketTick) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    const size_t function = source.find("int64_t ComputeTargetVideoPts");
    const size_t nextFunction = source.find("bool IsConfiguredNvencLookaheadActive", function);
    ASSERT_NE(function, std::string::npos);
    ASSERT_NE(nextFunction, std::string::npos);
    const std::string body = source.substr(function, nextFunction - function);
    EXPECT_NE(body.find("if (useExplicitCfrTimeline)"), std::string::npos);
    EXPECT_EQ(body.find("ComputeCfrFrameIndexForElapsedUs"), std::string::npos);
    const std::string contiguousCall = "ComputeNextCfrFrameIndex(lastAssignedVideoPts)";
    const size_t firstCall = body.find(contiguousCall);
    const size_t secondCall = body.find(contiguousCall, firstCall + 1);
    ASSERT_NE(firstCall, std::string::npos);
    ASSERT_NE(secondCall, std::string::npos);
    EXPECT_EQ(body.find(contiguousCall, secondCall + 1), std::string::npos);
}

TEST(VideoEncoderSourceTest, CaptureSourceTransitionDropsCachedVisualContent) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    const size_t reset = source.find("void VideoEncoder::ResetRepeatFrameCache()");
    ASSERT_NE(reset, std::string::npos);
    const size_t end = source.find("bool VideoEncoder::WasLastFrameDeferred", reset);
    ASSERT_NE(end, std::string::npos);
    const std::string body = source.substr(reset, end - reset);
    EXPECT_NE(body.find("repeatFrameTexture->Release()"), std::string::npos);
    EXPECT_NE(body.find("InvalidateRepeatSourceFrameTexture()"), std::string::npos);
    EXPECT_EQ(source.find("cachedRepeatPacket_"), std::string::npos);
    EXPECT_EQ(source.find("av_packet_ref(repeatPkt"), std::string::npos);
}

TEST(VideoEncoderSourceTest, Av1NvencSafetyOptionsOverrideCustomOptionsBeforeCodecOpen) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    const size_t customApply = source.find("for (const auto& option : optionPlan.customOptions)",
                                           source.find("if (isMF)"));
    const size_t requiredApply = source.find("for (const auto& option : optionPlan.requiredOptions)", customApply);
    const size_t codecOpen = source.find("avcodec_open2(codecCtx, codec, &opts)", requiredApply);
    ASSERT_NE(customApply, std::string::npos);
    ASSERT_NE(requiredApply, std::string::npos);
    ASSERT_NE(codecOpen, std::string::npos);
    EXPECT_LT(customApply, requiredApply);
    EXPECT_LT(requiredApply, codecOpen);
    EXPECT_EQ(source.find("repeat_pps"), std::string::npos);
}

TEST(VideoEncoderSourceTest, VideoProcessorOutputsAreOwnedByFfmpegHardwareFrames) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("d11FramesHw->BindFlags |= D3D11_BIND_RENDER_TARGET"), std::string::npos);
    EXPECT_NE(source.find("av_hwframe_get_buffer(d3d11FramesCtx, outputFrame, 0)"), std::string::npos);
    EXPECT_NE(source.find("VideoProcessorBlt(videoProcessor, outputView, 0, 1, &stream)"), std::string::npos);
    EXPECT_NE(source.find("outputViewCache.push_back({outputTexture, outputArraySlice, outputView})"),
              std::string::npos);
    EXPECT_EQ(source.find("nv12StagingTextures"), std::string::npos);
    EXPECT_EQ(source.find("av_buffer_create(reinterpret_cast<uint8_t*>(nv12Tex)"), std::string::npos);
}

TEST(VideoEncoderSourceTest, MetadataDurationDiagnosticsRunAfterTrailerAndIgnoreUnavailableFields) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    const size_t syncFinalize = source.find("[VideoEncoder] Sync Stop: Finalizing file...");
    const size_t syncTrailer = source.find("av_write_trailer(fmtCtx)", syncFinalize);
    const size_t syncSummary = source.find("LogFinalDurationSummary(fmtCtx", syncTrailer);
    ASSERT_NE(syncFinalize, std::string::npos);
    ASSERT_NE(syncTrailer, std::string::npos);
    ASSERT_NE(syncSummary, std::string::npos);
    EXPECT_LT(syncTrailer, syncSummary);

    const size_t asyncFinalize = source.find("[VideoEncoder] Async Finalize: Writing Trailer...");
    const size_t asyncTrailer = source.find("av_write_trailer(fmtCtx)", asyncFinalize);
    const size_t asyncSummary = source.find("LogFinalDurationSummary(fmtCtx", asyncTrailer);
    ASSERT_NE(asyncFinalize, std::string::npos);
    ASSERT_NE(asyncTrailer, std::string::npos);
    ASSERT_NE(asyncSummary, std::string::npos);
    EXPECT_LT(asyncTrailer, asyncSummary);

    EXPECT_NE(source.find("const bool metadataComplete"), std::string::npos);
    EXPECT_NE(source.find("Final in-memory AVStream durations are incomplete"), std::string::npos);
}

TEST(VideoEncoderSourceTest, VideoProcessorUsesOneDeterministicPrecompositedRgbStream) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("VideoProcessorSetStreamFrameFormat(videoProcessor, 0, "
                          "D3D11_VIDEO_FRAME_FORMAT_PROGRESSIVE)"),
              std::string::npos);
    EXPECT_NE(source.find("VideoProcessorSetStreamAutoProcessingMode(videoProcessor, 0, FALSE)"), std::string::npos);
    EXPECT_NE(source.find("[VideoProcessor] Deterministic single-stream processing"), std::string::npos);
    EXPECT_EQ(source.find("VideoProcessorSetStreamFrameFormat(videoProcessor, 1"), std::string::npos);
    EXPECT_EQ(source.find("VideoProcessorSetStreamAutoProcessingMode(videoProcessor, 1"), std::string::npos);
    EXPECT_EQ(source.find("VideoProcessorSetStreamAlpha"), std::string::npos);
    EXPECT_EQ(source.find("streams[1]"), std::string::npos);
}

TEST(VideoEncoderSourceTest, CursorPrecompositionBacksUpOnlyTouchedRgbRegionOnNormalCaptureSurfaces) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    EXPECT_NE(source.find("PrepareVideoProcessorCursorInput(bgraTexture, overlayCursor"), std::string::npos);
    EXPECT_NE(source.find("(sourceDesc.BindFlags & D3D11_BIND_RENDER_TARGET) != 0"), std::string::npos);
    EXPECT_NE(source.find("CopySubresourceRegion(cursorRestoreTexture, 0, 0, 0, 0, source"), std::string::npos);
    EXPECT_NE(source.find("CopySubresourceRegion(target, 0, destinationX, destinationY, 0, backup"), std::string::npos);
    EXPECT_NE(source.find("Point RGB precomposition before VP active"), std::string::npos);
    EXPECT_NE(source.find("VideoProcessorBlt(videoProcessor, outputView, 0, 1, &stream)"), std::string::npos);
}

TEST(VideoEncoderSourceTest, CursorPrecompositionFailureNeverFailsVideoConversion) {
    const std::string source = ReadVideoEncoderSource();
    ASSERT_FALSE(source.empty());

    const size_t prepare = source.find("bool VideoEncoder::PrepareVideoProcessorCursorInput");
    ASSERT_NE(prepare, std::string::npos);
    const size_t preparedSource = source.find("*preparedSource = source;", prepare);
    ASSERT_NE(preparedSource, std::string::npos);
    const size_t convert = source.find("bool VideoEncoder::ConvertBGRAtoNV12", preparedSource);
    ASSERT_NE(convert, std::string::npos);
    const std::string cursorBody = source.substr(preparedSource, convert - preparedSource);

    EXPECT_EQ(cursorBody.find("return false;"), std::string::npos);
    EXPECT_NE(cursorBody.find("video conversion continues without this"), std::string::npos);
}
