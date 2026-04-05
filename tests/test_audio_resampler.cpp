#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <vector>

#include "../mediaengine/audio_resampler.h"

namespace {

AudioResampler::InputFormat MakeStereoPacked24Input() {
    AudioResampler::InputFormat inFmt{};
    inFmt.channels = 2;
    inFmt.sampleRate = 48000;
    inFmt.bitsPerSample = 24;
    inFmt.validBitsPerSample = 24;
    inFmt.isFloat = false;
    inFmt.blockAlign = 6;
    return inFmt;
}

AudioResampler::OutputFormat MakeStereoFloatOutput(int sampleRate = 48000) {
    AudioResampler::OutputFormat outFmt{};
    outFmt.channels = 2;
    outFmt.sampleRate = sampleRate;
    outFmt.sampleFmt = AV_SAMPLE_FMT_FLTP;
    return outFmt;
}

}  // namespace

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

TEST(AudioResamplerTest, ProcessPacked24BitStereoProducesUsablePlanarFloatSamples) {
    AudioResampler resampler;
    ASSERT_TRUE(resampler.Init(MakeStereoPacked24Input(), MakeStereoFloatOutput()));

    // Two stereo frames of packed 24-bit little-endian PCM.
    const std::array<uint8_t, 12> input = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x40, 0x00, 0x00, 0xC0,
    };

    uint8_t** output = nullptr;
    int outputSamples = 0;
    ASSERT_TRUE(resampler.Process(input.data(), static_cast<int>(input.size()), &output, &outputSamples));
    ASSERT_NE(output, nullptr);
    EXPECT_EQ(outputSamples, 2);

    const float* left = reinterpret_cast<const float*>(output[0]);
    const float* right = reinterpret_cast<const float*>(output[1]);
    EXPECT_NEAR(left[0], 0.0f, 1e-6f);
    EXPECT_NEAR(right[0], 0.0f, 1e-6f);
    EXPECT_GT(left[1], 0.45f);
    EXPECT_LT(right[1], -0.45f);

    AudioResampler::FreeOutputBuffer(output);
}

TEST(AudioResamplerTest, ProcessCanUpmixMonoAndResampleToEncoderRate) {
    AudioResampler resampler;
    AudioResampler::InputFormat inFmt{};
    inFmt.channels = 1;
    inFmt.sampleRate = 24000;
    inFmt.bitsPerSample = 16;
    inFmt.validBitsPerSample = 16;
    inFmt.isFloat = false;
    inFmt.blockAlign = 2;

    AudioResampler::OutputFormat outFmt{};
    outFmt.channels = 2;
    outFmt.sampleRate = 48000;
    outFmt.sampleFmt = AV_SAMPLE_FMT_FLTP;

    ASSERT_TRUE(resampler.Init(inFmt, outFmt));

    std::vector<int16_t> monoSamples(512);
    for (size_t i = 0; i < monoSamples.size(); ++i) {
        monoSamples[i] = (i % 2 == 0) ? 16384 : -16384;
    }

    uint8_t** output = nullptr;
    int outputSamples = 0;
    ASSERT_TRUE(resampler.Process(reinterpret_cast<const uint8_t*>(monoSamples.data()),
                                  static_cast<int>(monoSamples.size() * sizeof(int16_t)), &output, &outputSamples));
    int totalOutputSamples = outputSamples;
    if (outputSamples > 0) {
        ASSERT_NE(output, nullptr);
        const float* left = reinterpret_cast<const float*>(output[0]);
        const float* right = reinterpret_cast<const float*>(output[1]);
        for (int i = 0; i < outputSamples; ++i) {
            EXPECT_NEAR(left[i], right[i], 1e-5f);
        }
    }
    AudioResampler::FreeOutputBuffer(output);

    uint8_t** flushed = nullptr;
    int flushedSamples = 0;
    if (resampler.Flush(&flushed, &flushedSamples)) {
        totalOutputSamples += flushedSamples;
        if (flushedSamples > 0) {
            ASSERT_NE(flushed, nullptr);
            const float* left = reinterpret_cast<const float*>(flushed[0]);
            const float* right = reinterpret_cast<const float*>(flushed[1]);
            for (int i = 0; i < flushedSamples; ++i) {
                EXPECT_NEAR(left[i], right[i], 1e-5f);
            }
        }
        AudioResampler::FreeOutputBuffer(flushed);
    }

    EXPECT_GT(totalOutputSamples, static_cast<int>(monoSamples.size()));
}

TEST(AudioResamplerTest, ResetClearsBufferedDelayAndClockTracking) {
    AudioResampler resampler;
    AudioResampler::InputFormat inFmt{};
    inFmt.channels = 2;
    inFmt.sampleRate = 44100;
    inFmt.bitsPerSample = 16;
    inFmt.validBitsPerSample = 16;
    inFmt.isFloat = false;
    inFmt.blockAlign = 4;

    AudioResampler::OutputFormat outFmt{};
    outFmt.channels = 2;
    outFmt.sampleRate = 48000;
    outFmt.sampleFmt = AV_SAMPLE_FMT_FLTP;

    ASSERT_TRUE(resampler.Init(inFmt, outFmt));

    const std::array<int16_t, 12> input = {
        32767, -32768, 1000, -1000, 2000, -2000, 3000, -3000, 4000, -4000, 5000, -5000,
    };
    uint8_t** output = nullptr;
    int outputSamples = 0;
    ASSERT_TRUE(resampler.Process(reinterpret_cast<const uint8_t*>(input.data()),
                                  static_cast<int>(input.size() * sizeof(int16_t)), &output, &outputSamples));
    AudioResampler::FreeOutputBuffer(output);

    EXPECT_GT(resampler.GetDelay(), 0);
    resampler.AdjustForClockDrift(1000, 49000);
    EXPECT_NE(resampler.GetCurrentCompensationDelta(), 0);

    ASSERT_TRUE(resampler.Reset());
    EXPECT_EQ(resampler.GetDelay(), 0);
    EXPECT_EQ(resampler.GetCurrentCompensationDelta(), 0);
    EXPECT_DOUBLE_EQ(resampler.GetCurrentCompensationPercent(), 0.0);
}
