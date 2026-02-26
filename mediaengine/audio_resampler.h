#pragma once

#include <cstdint>
#include <vector>

extern "C" {
#include <libavutil/channel_layout.h>
#include <libavutil/samplefmt.h>
#include <libswresample/swresample.h>
}

/**
 * High-quality audio resampler with proper format handling.
 *
 * Key features:
 * - Proper 24-bit audio support (both packed and in 32-bit container)
 * - Mono to stereo upconversion
 * - Sample rate conversion with high quality filtering
 * - Format conversion (int16, int24, int32, float) to encoder format
 */
class AudioResampler {
public:
    // Input format description (from WASAPI)
    struct InputFormat {
        int channels;
        int sampleRate;
        int bitsPerSample;       // Container size (16, 24, 32)
        int validBitsPerSample;  // Actual valid bits (from WAVEFORMATEXTENSIBLE)
        bool isFloat;
        int blockAlign;  // Bytes per frame (all channels)
    };

    // Output format description (for encoder)
    struct OutputFormat {
        int channels;
        int sampleRate;
        AVSampleFormat sampleFmt;  // e.g., AV_SAMPLE_FMT_S32P, AV_SAMPLE_FMT_FLTP
    };

    AudioResampler();
    ~AudioResampler();

    // Non-copyable
    AudioResampler(const AudioResampler&) = delete;
    AudioResampler& operator=(const AudioResampler&) = delete;

    /**
     * Initialize or reinitialize the resampler.
     * Call this when format changes or at start.
     *
     * @param input  Source format from WASAPI
     * @param output Target format for encoder
     * @return true on success
     */
    bool Init(const InputFormat& input, const OutputFormat& output);

    /**
     * Process audio samples.
     *
     * @param inputData    Raw input audio data from WASAPI
     * @param inputBytes   Size of input data in bytes
     * @param outputData   [out] Pointer to allocated output buffer (caller frees)
     * @param outputSamples [out] Number of output samples per channel
     * @return true on success
     */
    bool Process(const uint8_t* inputData, int inputBytes, uint8_t*** outputData, int* outputSamples);

    /**
     * Flush remaining samples from internal buffers.
     *
     * @param outputData    [out] Pointer to allocated output buffer
     * @param outputSamples [out] Number of flushed samples
     * @return true if samples were flushed
     */
    bool Flush(uint8_t*** outputData, int* outputSamples);

    /**
     * Get the delay in samples currently buffered in the resampler.
     * Used for A/V sync calculations.
     */
    int64_t GetDelay() const;

    /**
     * Check if resampler is initialized and ready.
     */
    bool IsReady() const {
        return swrCtx != nullptr;
    }

    /**
     * Get the current output format.
     */
    const OutputFormat& GetOutputFormat() const {
        return outFmt;
    }

    /**
     * Free output buffers allocated by Process/Flush.
     */
    static void FreeOutputBuffer(uint8_t** data);

    /**
     * Adjust resampler ratio for clock drift compensation.
     * Call this periodically with current video time and audio sample count.
     * Uses swr_set_compensation() to gradually adjust output rate.
     *
     * @param videoElapsedMs  Current video elapsed time in milliseconds
     * @param audioSamplesOutput  Total audio samples output so far
     */
    void AdjustForClockDrift(int64_t videoElapsedMs, int64_t audioSamplesOutput);

    /**
     * Reset the resampler internal state and buffers.
     */
    bool Reset();

    /**
     * Reset clock drift tracking (call at start of recording).
     */
    void ResetClockTracking();

private:
    SwrContext* swrCtx;
    InputFormat inFmt;
    OutputFormat outFmt;

    // Intermediate buffer for 24-bit unpacking (std::vector avoids raw new[]/delete[])
    std::vector<uint8_t> unpackBuffer;

    // Clock drift compensation state
    int64_t lastDriftSamples = 0;
    bool compensationActive = false;

    // Smoothing state for drift correction
    double smoothedDrift = 0.0;
    int32_t currentDelta = 0;

    // PI Controller State
    double integralError = 0.0;
    // Steady-state gains (relaxed after initial convergence)
    static constexpr double kKpSteady = 0.08;   // Proportional
    static constexpr double kKiSteady = 0.015;  // Integral
    // Fast-convergence gains (first 10 seconds of recording)
    static constexpr double kKpFast = 0.15;  // Proportional (fast mode)
    static constexpr double kKiFast = 0.03;  // Integral (fast mode)
    // Smoothing alpha: 0.95 = ~20 update time constant (~2s at 10Hz updates)
    static constexpr double kSmoothingAlpha = 0.95;
    static const int COMPENSATION_PERIOD_SEC = 10;

    // Track elapsed time for fast/steady mode transition
    bool fastModeActive = true;

    // Rate limiting updates
    int64_t lastCompensationTimeMs = 0;

    // Per-instance log counters (avoid static race conditions)
    int largeSkipCounter_ = 0;
    int limitLogCounter_ = 0;

    // Detect and handle 24-bit in 32-bit container
    AVSampleFormat DetermineInputFormat() const;

    // Unpack 24-bit samples to 32-bit for processing
    bool Unpack24BitTo32Bit(const uint8_t* input, int inputBytes, uint8_t** output, int* outputBytes);
};
