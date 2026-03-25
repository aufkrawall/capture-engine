#include <gtest/gtest.h>

#include "../mediaengine/audio_resampler.h"

TEST(AudioResamplerTest, MaxCompensationPercentCanBeRaisedForCoverageLoss) {
    AudioResampler resampler;
    AudioResampler::InputFormat inFmt{};
    inFmt.channels = 2;
    inFmt.sampleRate = 48000;
    inFmt.bitsPerSample = 32;
    inFmt.validBitsPerSample = 32;
    inFmt.isFloat = true;
    inFmt.blockAlign = 8;

    AudioResampler::OutputFormat outFmt{};
    outFmt.channels = 2;
    outFmt.sampleRate = 48000;
    outFmt.sampleFmt = AV_SAMPLE_FMT_FLTP;

    ASSERT_TRUE(resampler.Init(inFmt, outFmt));
    EXPECT_EQ(resampler.GetMaxCompensationDelta(), 4800);

    resampler.SetMaxCompensationPercent(1.25);
    EXPECT_EQ(resampler.GetMaxCompensationDelta(), 6000);

    resampler.SetMaxCompensationPercent(1.0);
    EXPECT_EQ(resampler.GetMaxCompensationDelta(), 4800);
}
