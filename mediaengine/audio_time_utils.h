#pragma once

#include <cstdint>

namespace ce::audio {

constexpr uint64_t kHundredNanosecondsPerSecond = 10000000ULL;
constexpr uint64_t kHundredNanosecondsPerMillisecond = 10000ULL;

inline uint64_t HundredNanosecondsToMilliseconds(uint64_t hundredNanoseconds) {
    return hundredNanoseconds / kHundredNanosecondsPerMillisecond;
}

inline double HundredNanosecondsToSeconds(uint64_t hundredNanoseconds) {
    return static_cast<double>(hundredNanoseconds) / static_cast<double>(kHundredNanosecondsPerSecond);
}

inline uint64_t HundredNanosecondsToSamples(uint64_t hundredNanoseconds, int sampleRate) {
    if (sampleRate <= 0 || hundredNanoseconds == 0) {
        return 0;
    }

    const uint64_t wholeSeconds = hundredNanoseconds / kHundredNanosecondsPerSecond;
    const uint64_t remainingHundredNanoseconds = hundredNanoseconds % kHundredNanosecondsPerSecond;
    return (wholeSeconds * static_cast<uint64_t>(sampleRate)) +
           ((remainingHundredNanoseconds * static_cast<uint64_t>(sampleRate)) + (kHundredNanosecondsPerSecond / 2)) /
               kHundredNanosecondsPerSecond;
}

inline uint64_t AudioFramesToHundredNanoseconds(uint64_t frames, int sampleRate) {
    if (sampleRate <= 0 || frames == 0) {
        return 0;
    }
    return ((frames * kHundredNanosecondsPerSecond) + (static_cast<uint64_t>(sampleRate) / 2)) /
           static_cast<uint64_t>(sampleRate);
}

inline uint64_t RawQpcToHundredNanoseconds(uint64_t rawQpc, uint64_t qpcFrequency) {
    if (qpcFrequency == 0) {
        return 0;
    }

    const uint64_t wholeSeconds = rawQpc / qpcFrequency;
    const uint64_t remainingTicks = rawQpc % qpcFrequency;
    return (wholeSeconds * kHundredNanosecondsPerSecond) +
           ((remainingTicks * kHundredNanosecondsPerSecond) / qpcFrequency);
}

inline uint64_t ApplyCaptureLatencyCompensation(uint64_t qpcPosition100ns, uint64_t streamLatency100ns,
                                                bool compensate) {
    if (!compensate || streamLatency100ns == 0) {
        return qpcPosition100ns;
    }
    if (qpcPosition100ns <= streamLatency100ns) {
        return 0;
    }
    return qpcPosition100ns - streamLatency100ns;
}

inline uint64_t ApplyProcessLoopbackPacketTimestampCompensation(uint64_t qpcPosition100ns, uint64_t frames,
                                                                int sampleRate) {
    // Process loopback reports period-based QPC positions on some systems. Using the
    // period center avoids placing the whole packet one engine quantum early while
    // still removing the observed end-of-period bias relative to render loopback.
    const uint64_t packetBias100ns = AudioFramesToHundredNanoseconds(frames, sampleRate) / 2;
    if (qpcPosition100ns <= packetBias100ns) {
        return 0;
    }
    return qpcPosition100ns - packetBias100ns;
}

}  // namespace ce::audio
