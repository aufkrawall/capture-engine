#include "audio_resampler.h"
#include <cmath>
#include <cstring>
#include "mediaengine.h"  // For DLL_Log

extern "C" {
#include <libavutil/opt.h>
}

AudioResampler::AudioResampler() : swrCtx(nullptr) {
    inFmt = {};
    outFmt = {};
}

AudioResampler::~AudioResampler() {
    if (swrCtx) {
        swr_free(&swrCtx);
        swrCtx = nullptr;
    }
    // unpackBuffer is std::vector — no manual cleanup needed
}

AVSampleFormat AudioResampler::DetermineInputFormat() const {
    if (inFmt.isFloat) {
        // Always 32-bit float from WASAPI
        return AV_SAMPLE_FMT_FLT;
    }

    // For integer formats, check valid bits vs container bits
    // WASAPI may report bitsPerSample=32 but validBitsPerSample=24
    int effectiveBits = inFmt.validBitsPerSample > 0 ? inFmt.validBitsPerSample : inFmt.bitsPerSample;

    switch (effectiveBits) {
        case 16:
            return AV_SAMPLE_FMT_S16;
        case 24:
            // 24-bit is tricky - if in 32-bit container, we treat as S32
            // If truly packed 24-bit (3 bytes), we need to unpack first
            if (inFmt.bitsPerSample == 32) {
                // 24-bit in 32-bit container - treat as S32 (left-justified)
                return AV_SAMPLE_FMT_S32;
            } else {
                // Packed 24-bit - we'll unpack to S32 before resampling
                return AV_SAMPLE_FMT_S32;
            }
        case 32:
            return AV_SAMPLE_FMT_S32;
        default:
            DLL_Log("[AudioResampler] Unknown bit depth %d, defaulting to S16", effectiveBits);
            return AV_SAMPLE_FMT_S16;
    }
}

bool AudioResampler::Unpack24BitTo32Bit(const uint8_t* input, int inputBytes, uint8_t** output, int* outputBytes) {
    // Calculate number of 24-bit samples
    int bytesPerSample = 3;  // Packed 24-bit
    int numSamples = inputBytes / bytesPerSample;
    int needed = numSamples * 4;  // 32-bit output

    // Ensure buffer is large enough (resize only when needed)
    if (static_cast<int>(unpackBuffer.size()) < needed) {
        unpackBuffer.resize(needed + 1024);  // Add headroom
    }

    // Unpack 24-bit to 32-bit (sign-extend, left-justify)
    // 24-bit sample: [LSB, MID, MSB] -> 32-bit: [0, LSB, MID, MSB]
    int32_t* out32 = reinterpret_cast<int32_t*>(unpackBuffer.data());
    for (int i = 0; i < numSamples; i++) {
        int idx = i * 3;
        // Little-endian: LSB first
        int32_t sample = (static_cast<int32_t>(input[idx + 2]) << 24) |  // MSB
                         (static_cast<int32_t>(input[idx + 1]) << 16) |  // MID
                         (static_cast<int32_t>(input[idx]) << 8);        // LSB
        // Sample is now left-justified in 32-bit, sign-extended from bit 31
        out32[i] = sample;
    }

    *output = unpackBuffer.data();
    *outputBytes = needed;
    return true;
}

bool AudioResampler::Init(const InputFormat& input, const OutputFormat& output) {
    // Clean up any existing context
    if (swrCtx) {
        swr_free(&swrCtx);
        swrCtx = nullptr;
    }

    inFmt = input;
    outFmt = output;

    // Determine FFmpeg input format
    AVSampleFormat inputSampleFmt = DetermineInputFormat();

    // Setup channel layouts
    AVChannelLayout inLayout, outLayout;
    av_channel_layout_default(&inLayout, input.channels);
    av_channel_layout_default(&outLayout, output.channels);

    // Allocate swresample context
    int ret = swr_alloc_set_opts2(&swrCtx, &outLayout, output.sampleFmt, output.sampleRate, &inLayout, inputSampleFmt,
                                  input.sampleRate, 0, nullptr);

    if (ret < 0 || !swrCtx) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        DLL_Log("[AudioResampler] Failed to allocate context: %s", errbuf);
        return false;
    }

    // Set high quality resampling options
    // Default filter length is good, but we can tweak if needed
    av_opt_set_int(swrCtx, "filter_size", 32, 0);  // Higher = better quality

    // Use dithering for bit depth reduction
    if (av_get_bytes_per_sample(output.sampleFmt) < av_get_bytes_per_sample(inputSampleFmt)) {
        av_opt_set_int(swrCtx, "dither_method", SWR_DITHER_TRIANGULAR_HIGHPASS, 0);
    }

    ret = swr_init(swrCtx);
    if (ret < 0) {
        char errbuf[256];
        av_strerror(ret, errbuf, sizeof(errbuf));
        DLL_Log("[AudioResampler] Failed to init context: %s", errbuf);
        swr_free(&swrCtx);
        swrCtx = nullptr;
        return false;
    }

    DLL_Log("[AudioResampler] Initialized: %dHz %dch %s -> %dHz %dch fmt=%d", input.sampleRate, input.channels,
            input.isFloat ? "float" : "int", output.sampleRate, output.channels, (int)output.sampleFmt);

    return true;
}

bool AudioResampler::Process(const uint8_t* inputData, int inputBytes, uint8_t*** outputData, int* outputSamples) {
    if (!swrCtx) {
        DLL_Log("[AudioResampler] Not initialized");
        return false;
    }

    const uint8_t* srcData = inputData;
    int srcBytes = inputBytes;

    // Handle packed 24-bit case - needs unpacking
    bool isPacked24Bit = !inFmt.isFloat && inFmt.bitsPerSample == 24 &&
                         (inFmt.validBitsPerSample == 0 || inFmt.validBitsPerSample == 24);

    if (isPacked24Bit) {
        uint8_t* unpacked = nullptr;
        int unpackedBytes = 0;
        if (!Unpack24BitTo32Bit(inputData, inputBytes, &unpacked, &unpackedBytes)) {
            return false;
        }
        srcData = unpacked;
        srcBytes = unpackedBytes;
    }

    // Calculate input samples
    int bytesPerInputFrame;
    if (isPacked24Bit) {
        // After unpacking, we have 4 bytes per sample
        bytesPerInputFrame = 4 * inFmt.channels;
    } else if (inFmt.isFloat) {
        bytesPerInputFrame = 4 * inFmt.channels;
    } else {
        bytesPerInputFrame = (inFmt.bitsPerSample / 8) * inFmt.channels;
    }

    int inputSamplesCount = srcBytes / bytesPerInputFrame;
    if (inputSamplesCount <= 0) {
        return false;
    }

    // Calculate maximum output samples
    int64_t delay = swr_get_delay(swrCtx, inFmt.sampleRate);
    int maxOutputSamples = av_rescale_rnd(delay + inputSamplesCount, outFmt.sampleRate, inFmt.sampleRate, AV_ROUND_UP);

    // Allocate output buffer
    uint8_t** outBuf = nullptr;
    int ret =
        av_samples_alloc_array_and_samples(&outBuf, nullptr, outFmt.channels, maxOutputSamples, outFmt.sampleFmt, 0);

    if (ret < 0) {
        DLL_Log("[AudioResampler] Failed to allocate output buffer");
        return false;
    }

    // Convert
    int convertedSamples = swr_convert(swrCtx, outBuf, maxOutputSamples, &srcData, inputSamplesCount);

    if (convertedSamples < 0) {
        char errbuf[256];
        av_strerror(convertedSamples, errbuf, sizeof(errbuf));
        DLL_Log("[AudioResampler] Conversion failed: %s", errbuf);
        av_freep(&outBuf[0]);
        av_freep(&outBuf);
        return false;
    }

    *outputData = outBuf;
    *outputSamples = convertedSamples;
    return true;
}

bool AudioResampler::Flush(uint8_t*** outputData, int* outputSamples) {
    if (!swrCtx) {
        return false;
    }

    // Get remaining delay
    int64_t delay = swr_get_delay(swrCtx, outFmt.sampleRate);
    if (delay <= 0) {
        *outputData = nullptr;
        *outputSamples = 0;
        return false;
    }

    // Allocate output buffer
    uint8_t** outBuf = nullptr;
    int ret = av_samples_alloc_array_and_samples(&outBuf, nullptr, outFmt.channels, (int)delay, outFmt.sampleFmt, 0);

    if (ret < 0) {
        return false;
    }

    // Flush (pass nullptr as input)
    int convertedSamples = swr_convert(swrCtx, outBuf, (int)delay, nullptr, 0);

    if (convertedSamples <= 0) {
        av_freep(&outBuf[0]);
        av_freep(&outBuf);
        *outputData = nullptr;
        *outputSamples = 0;
        return false;
    }

    *outputData = outBuf;
    *outputSamples = convertedSamples;
    return true;
}

int64_t AudioResampler::GetDelay() const {
    if (!swrCtx) {
        return 0;
    }
    return swr_get_delay(swrCtx, outFmt.sampleRate);
}

void AudioResampler::FreeOutputBuffer(uint8_t** data) {
    if (data) {
        av_freep(&data[0]);
        av_freep(&data);
    }
}

bool AudioResampler::Reset() {
    if (!swrCtx)
        return false;

    int ret = swr_init(swrCtx);
    ResetClockTracking();
    return (ret >= 0);
}

void AudioResampler::ResetClockTracking() {
    lastDriftSamples = 0;
    compensationActive = false;
    smoothedDrift = 0.0;
    currentDelta = 0;
    integralError = 0.0;
    lastCompensationTimeMs = 0;
    fastModeActive = true;
}

void AudioResampler::AdjustForClockDrift(int64_t videoElapsedMs, int64_t audioSamplesOutput) {
    if (!swrCtx || videoElapsedMs <= 0 || outFmt.sampleRate <= 0) {
        return;
    }

    // THROTTLE UPDATES:
    // Updating resampling compensation too frequently (e.g. every 8ms frame)
    // causes audible "zipper noise" or crackling because the polyphase filter
    // doesn't have time to settle. Limit updates to 10Hz (every 100ms).
    if (lastCompensationTimeMs > 0 && (videoElapsedMs - lastCompensationTimeMs) < 100) {
        return;
    }
    lastCompensationTimeMs = videoElapsedMs;

    // Calculate expected audio samples for current video time
    // Expected = videoElapsedMs * sampleRate / 1000
    int64_t expectedSamples = (videoElapsedMs * outFmt.sampleRate) / 1000;

    // Calculate drift: positive = audio ahead (too many samples), negative =
    // audio behind
    int64_t driftSamples = audioSamplesOutput - expectedSamples;

    // DEADBAND: Ignore high-frequency jitter (up to 5ms = 240 samples at 48kHz)
    // from WASAPI block delivery intervals to prevent pitch oscillation
    if (std::abs(driftSamples) < 240) {
        driftSamples = 0;
    }

    // STARTUP GUARD: Skip pitch correction for the first 2 seconds to allow
    // the ring buffer to reach steady-state fill level. Beyond that, the
    // PI controller rate-limiting and smoothing are sufficient safeguards.
    const int64_t STARTUP_PERIOD_MS = 2000;
    if (videoElapsedMs < STARTUP_PERIOD_MS) {
        if (largeSkipCounter_++ % 100 == 0) {
            DLL_Log("[AudioResampler] Startup period (%lldms) - skipping pitch correction", videoElapsedMs);
        }
        return;
    }

    // Transition from fast to steady mode after 10 seconds
    if (fastModeActive && videoElapsedMs > 10000) {
        fastModeActive = false;
        DLL_Log("[AudioResampler] Switching from fast to steady-state gains");
    }

    // Select active PI gains based on mode
    double activeKp = fastModeActive ? kKpFast : kKpSteady;
    double activeKi = fastModeActive ? kKiFast : kKiSteady;

    // 1. SMOOTHING STAGE (Low Pass Filter)
    // Alpha 0.05 = ~20 update time constant (~2s at 10Hz updates)
    smoothedDrift = (smoothedDrift * kSmoothingAlpha) + ((double)driftSamples * (1.0 - kSmoothingAlpha));

    // CONTINUOUS PI CONTROLLER:
    // We calculate correction targeting a 10-second window for sub-sample
    // precision.

    // Variables declaration
    int32_t targetDelta = 0;
    int32_t maxDelta = 0;

    // 1. Integral Term
    integralError += smoothedDrift;

    // Anti-Windup: Clamp Integral term
    // Max correction +/- 10% speed (doubled from 5% to handle larger drifts).
    // In 10 seconds, 10% is 0.10 * 48000 * 10 = 48000 samples
    const double MAX_INTEGRAL_CORRECTION = (outFmt.sampleRate * COMPENSATION_PERIOD_SEC) * 0.10;
    if (integralError * activeKi > MAX_INTEGRAL_CORRECTION)
        integralError = MAX_INTEGRAL_CORRECTION / activeKi;
    if (integralError * activeKi < -MAX_INTEGRAL_CORRECTION)
        integralError = -MAX_INTEGRAL_CORRECTION / activeKi;

    // 2. Calculate Correction (PI Control)
    // Result is "samples to insert/drop over COMPENSATION_PERIOD_SEC"
    double correction = (activeKp * smoothedDrift) + (activeKi * integralError);

    targetDelta = (int32_t)correction;

    // 3. Absolute Safety Limits
    // +/- 10% of total period samples (doubled from 5% to handle larger drifts)
    maxDelta = (outFmt.sampleRate * COMPENSATION_PERIOD_SEC) / 10;

    // Log extreme correction (Logic Debug)
    if (std::abs(targetDelta) > maxDelta) {
        if (limitLogCounter_++ % 100 == 0) {
            DLL_Log("[AudioResampler] PI Saturated! Req=%d, Max=%d", targetDelta, maxDelta);
        }
    }

    // Clamp
    if (targetDelta > maxDelta)
        targetDelta = maxDelta;
    if (targetDelta < -maxDelta)
        targetDelta = -maxDelta;

    // 2. RATE LIMITING STAGE (Inertia)
    // Max change: 200 samples per update (at 10Hz = 2000/sec). This allows
    // reaching the 10% maximum correction (~48000) in about 24 seconds,
    // fast enough to drain accumulated lead without audible pitch steps.
    // At 0.042%/s acceleration even maximum ramp-up is inaudible.
    const int32_t maxChange = 200;

    if (currentDelta < targetDelta) {
        currentDelta += maxChange;
        if (currentDelta > targetDelta)
            currentDelta = targetDelta;
    } else if (currentDelta > targetDelta) {
        currentDelta -= maxChange;
        if (currentDelta < targetDelta)
            currentDelta = targetDelta;
    }

    // Apply compensation if needed
    if (currentDelta != 0 || compensationActive) {
        // Compensate over COMPENSATION_PERIOD_SEC.
        // swr_set_compensation(delta, distance): delta > 0 ADDS output samples (produces more per
        // input = slower drain); delta < 0 REMOVES output samples (produces fewer per input = faster
        // drain).  When the ring buffer is too full (currentDelta > 0) we need faster drain, so
        // negate: pass -currentDelta to drop output samples and force the resampler to consume more
        // input from the ring buffer per video frame.
        int ret = swr_set_compensation(swrCtx, -currentDelta, outFmt.sampleRate * COMPENSATION_PERIOD_SEC);

        if (ret >= 0) {
            // Log (scale back to samples/sec for readability)
            if (!compensationActive ||
                std::abs(currentDelta - (int32_t)lastDriftSamples) > 200) {  // Log every ~20 samples/sec change
                DLL_Log(
                    "[AudioResampler] Drift Comp: smoothed=%.1f, current=%d/10s "
                    "(%.3f%%)",
                    smoothedDrift, currentDelta,
                    (double)currentDelta * 100.0 / (outFmt.sampleRate * COMPENSATION_PERIOD_SEC));
                lastDriftSamples = currentDelta;
            }

            if (currentDelta != 0) {
                compensationActive = true;
            } else {
                compensationActive = false;
                DLL_Log("[AudioResampler] Drift stabilized.");
            }
        }
    }
}
