#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "source_fragment_reader.h"

// Copying the encoder input into the repeat cache after the encoder submit
// reads a surface NVENC has taken; on NVIDIA that copy waited for the encode
// and held CE's later GPU work behind it (session 20261016_013801). Every
// submit path stages the copy before the submit and commits it by swap.

namespace {

std::string ReadVideo(const char* file) {
    return ce::test_source::ReadLogicalSource(std::filesystem::current_path() / "mediaengine" / "video" / file);
}

}  // namespace

TEST(RepeatCacheStageTest, EverySubmitPathStagesTheCacheCopyBeforeTheEncoderSubmit) {
    const std::string encode = ReadVideo("video_encoder_encode.cpp");
    const std::string framegrab = ReadVideo("video_encoder_framegrab.cpp");
    ASSERT_FALSE(encode.empty());
    ASSERT_FALSE(framegrab.empty());

    const size_t freshStage =
        encode.find("StageRepeatFrameTexture(EncoderFrameTexture(d3d11Frame))");
    const size_t freshSubmit = encode.find("SubmitFrameForEncode(d3d11Frame", freshStage);
    const size_t freshCache = encode.find("CacheRepeatFrameTexture(", freshSubmit);
    ASSERT_NE(freshStage, std::string::npos);
    ASSERT_NE(freshSubmit, std::string::npos);
    EXPECT_NE(freshCache, std::string::npos);
    EXPECT_NE(encode.rfind("RepeatFrameStageScope repeatFrameStage{this};", freshStage), std::string::npos);

    const size_t grabStage = framegrab.find("StageRepeatFrameTexture(");
    const size_t grabSend =
        framegrab.find("SendFrameToEncoder(encoderInputFrame, ce::media::gpu_stage::Path::ScreenGrab");
    const size_t repeatStage = framegrab.find("StageRepeatFrameTexture(", grabSend);
    const size_t repeatSend =
        framegrab.find("SendFrameToEncoder(encoderInputFrame, ce::media::gpu_stage::Path::Repeat");
    ASSERT_NE(grabStage, std::string::npos);
    ASSERT_NE(grabSend, std::string::npos);
    ASSERT_NE(repeatStage, std::string::npos);
    ASSERT_NE(repeatSend, std::string::npos);
    EXPECT_LT(grabStage, grabSend);
    EXPECT_LT(repeatStage, repeatSend);
}

TEST(RepeatCacheStageTest, CommitOfAStagedFrameSwapsInsteadOfCopying) {
    const std::string textures = ReadVideo("video_encoder_textures.cpp");
    ASSERT_FALSE(textures.empty());
    const size_t cache = textures.find("bool VideoEncoder::CacheRepeatFrameTexture(");
    ASSERT_NE(cache, std::string::npos);
    const size_t swap = textures.find("std::swap(repeatFrameTexture, stagedRepeatFrameTexture);", cache);
    const size_t copy = textures.find("CopyResource(repeatFrameTexture, sourceTexture);", cache);
    const size_t dedupe = textures.find("sourceTexture == repeatFrameCachedFromStage", cache);
    ASSERT_NE(swap, std::string::npos);
    ASSERT_NE(copy, std::string::npos);
    ASSERT_NE(dedupe, std::string::npos);
    // Same-frame calls and the staged commit return before the fallback copy.
    EXPECT_LT(dedupe, copy);
    EXPECT_LT(swap, copy);
    // The stage is frame-scoped, so a reused pool texture pointer never matches.
    const size_t end = textures.find("void VideoEncoder::EndRepeatFrameStage()");
    ASSERT_NE(end, std::string::npos);
    EXPECT_NE(textures.find("stagedRepeatFrameSource = nullptr;", end), std::string::npos);
    EXPECT_NE(textures.find("repeatFrameCachedFromStage = nullptr;", end), std::string::npos);
}
