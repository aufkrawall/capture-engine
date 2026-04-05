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

inline uint64_t RawQpcToHundredNanoseconds(uint64_t rawQpc, uint64_t qpcFrequency) {
    if (qpcFrequency == 0) {
        return 0;
    }

    const uint64_t wholeSeconds = rawQpc / qpcFrequency;
    const uint64_t remainingTicks = rawQpc % qpcFrequency;
    return (wholeSeconds * kHundredNanosecondsPerSecond) +
           ((remainingTicks * kHundredNanosecondsPerSecond) / qpcFrequency);
}

}  // namespace ce::audio
