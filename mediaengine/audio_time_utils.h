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

// Sanity bounds for a WASAPI-reported capture QPC position. These are universal
// physical limits (not device tuning): a freshly-read capture timestamp cannot
// lie in the future, and a live packet cannot predate "now" by more than the
// endpoint buffer. Generous margins leave every healthy position untouched while
// still catching gross driver bugs.
constexpr uint64_t kDefaultCaptureQpcFutureToleranceUnits = kHundredNanosecondsPerSecond;        // 1 s
constexpr uint64_t kDefaultCaptureQpcPastToleranceUnits = 5ULL * kHundredNanosecondsPerSecond;   // 5 s

// IAudioCaptureClient::GetBuffer reports the QPC (in 100-ns units) at which the
// endpoint read the device position. That instant has, by definition, already
// happened, so a valid value can never lie meaningfully in the future relative to
// the current QPC, nor predate it by more than the endpoint's buffer. Some drivers
// (observed on a 192 kHz loopback endpoint) emit a garbage first-packet position
// hundreds of days in the future; downstream timeline math then turns that into an
// unbounded leading-silence allocation (bad_alloc -> std::terminate). Replace any
// out-of-domain position with the measured current QPC so the recording timeline
// stays in-domain on every device while leaving healthy positions bit-identical.
// nowQpc100ns == 0 means no reference clock is available, so the value is trusted.
inline uint64_t SanitizeCaptureQpcPosition(uint64_t reportedQpc100ns, uint64_t nowQpc100ns,
                                           uint64_t futureToleranceUnits = kDefaultCaptureQpcFutureToleranceUnits,
                                           uint64_t pastToleranceUnits = kDefaultCaptureQpcPastToleranceUnits) {
    if (nowQpc100ns == 0) {
        return reportedQpc100ns;
    }
    if (reportedQpc100ns > nowQpc100ns + futureToleranceUnits) {
        return nowQpc100ns;
    }
    if (reportedQpc100ns + pastToleranceUnits < nowQpc100ns) {
        return nowQpc100ns;
    }
    return reportedQpc100ns;
}

}  // namespace ce::audio
