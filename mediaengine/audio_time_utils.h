#pragma once

#include <cerrno>
#include <climits>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <string>

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
constexpr uint64_t kDefaultCaptureQpcFutureToleranceUnits = kHundredNanosecondsPerSecond;       // 1 s
constexpr uint64_t kDefaultCaptureQpcPastToleranceUnits = 5ULL * kHundredNanosecondsPerSecond;  // 5 s

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
    // Compare deltas instead of adding tolerances to absolute counters. QPC
    // positions can legitimately be close to UINT64_MAX after conversion, and
    // overflowing either addition would invert the range check.
    if (reportedQpc100ns > nowQpc100ns && reportedQpc100ns - nowQpc100ns > futureToleranceUnits) {
        return nowQpc100ns;
    }
    if (reportedQpc100ns < nowQpc100ns && nowQpc100ns - reportedQpc100ns > pastToleranceUnits) {
        return nowQpc100ns;
    }
    return reportedQpc100ns;
}

// GetNextPacketSize and the immediately following GetBuffer describe the same
// packet. Reject impossible driver output before using it for allocations or
// pointer reads. A zero configuredBufferFrames means GetBufferSize telemetry was
// unavailable. Keep an independent live-capture bound in that case: no healthy
// endpoint packet can contain multiple seconds of audio, while accepting an
// arbitrary matching uint32_t count would permit a corrupt driver to request a
// multi-gigabyte allocation.
constexpr uint32_t kMaxWasapiCapturePacketDurationSeconds = 2;

inline uint32_t MaxWasapiCapturePacketFrames(uint32_t sampleRate) {
    if (sampleRate == 0) {
        return 0;
    }
    const uint64_t maxFrames =
        static_cast<uint64_t>(sampleRate) * static_cast<uint64_t>(kMaxWasapiCapturePacketDurationSeconds);
    return maxFrames > std::numeric_limits<uint32_t>::max() ? std::numeric_limits<uint32_t>::max()
                                                            : static_cast<uint32_t>(maxFrames);
}

inline bool IsWasapiCapturePacketFrameCountValid(uint32_t announcedFrames, uint32_t actualFrames,
                                                 uint32_t configuredBufferFrames, uint32_t sampleRate) {
    const uint32_t absoluteMaxFrames = MaxWasapiCapturePacketFrames(sampleRate);
    return announcedFrames != 0 && actualFrames == announcedFrames && absoluteMaxFrames != 0 &&
           actualFrames <= absoluteMaxFrames && (configuredBufferFrames == 0 || actualFrames <= configuredBufferFrames);
}

// Checked frame-to-byte conversion for WASAPI packet allocations. This matters
// for 32-bit captureengine builds, where a corrupt frame count can otherwise wrap
// size_t and turn the subsequent memcpy into an out-of-bounds write.
inline bool TryComputeAudioPacketByteSize(uint32_t frames, uint32_t blockAlign, size_t* byteSize) {
    if (!byteSize || frames == 0 || blockAlign == 0 ||
        static_cast<size_t>(frames) > std::numeric_limits<size_t>::max() / static_cast<size_t>(blockAlign)) {
        return false;
    }
    *byteSize = static_cast<size_t>(frames) * static_cast<size_t>(blockAlign);
    return true;
}

// Safe parse of a config sample-rate string into Hz. Accepts the "default"
// sentinel (or empty) -> defaultRate, or a string that is ENTIRELY a positive
// decimal integer. Unlike std::stoi this NEVER throws and is strict: any
// non-numeric, partial ("48kHz"), non-positive, or out-of-range value falls back
// to defaultRate rather than silently parsing a nonsense leading number. A
// hand-edited config typo must never crash encoder init with an unhandled
// std::invalid_argument / std::out_of_range, nor produce a bogus 48 Hz stream.
inline int ParseSampleRateOr(const std::string& value, int defaultRate = 48000) {
    if (value.empty() || value == "default") {
        return defaultRate;
    }
    errno = 0;
    char* end = nullptr;
    const long parsed = std::strtol(value.c_str(), &end, 10);
    if (end == value.c_str() || end == nullptr || *end != '\0' || errno == ERANGE || parsed <= 0 || parsed > INT_MAX) {
        return defaultRate;  // not a pure positive integer -> safe fallback
    }
    return static_cast<int>(parsed);
}

}  // namespace ce::audio
