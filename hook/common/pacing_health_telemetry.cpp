#include "pacing_health_telemetry.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace ce::pacing_health {
namespace {

// 4096 samples retain a complete 10 s window through 4x FG at 360 Hz.
constexpr uint32_t kRingCapacity = 4096;
// Everything above this is a stall/hitch already covered by dedicated logs,
// not part of the cadence this ring describes.
constexpr int64_t kMaxRetainedIntervalUs = 5'000'000;

struct Sample {
    std::atomic<uint32_t> valueUs{0};
    std::atomic<int64_t> observationTimeUs{0};
    std::atomic<uint64_t> tag{0};
};

Sample g_ring[static_cast<size_t>(Channel::kCount)][kRingCapacity];
std::atomic<uint64_t> g_total[static_cast<size_t>(Channel::kCount)]{};

struct SampleValue {
    uint32_t valueUs = 0;
    int64_t observationTimeUs = 0;
    uint64_t tag = 0;
};

SampleValue ReadRing(Channel channel, uint64_t absoluteIndex) {
    const Sample& sample = g_ring[static_cast<size_t>(channel)][absoluteIndex % kRingCapacity];
    return {
        sample.valueUs.load(std::memory_order_relaxed),
        sample.observationTimeUs.load(std::memory_order_relaxed),
        sample.tag.load(std::memory_order_relaxed),
    };
}

}  // namespace

void Observe(Channel channel, int64_t valueUs, int64_t observationTimeUs, uint64_t tag) {
    if (channel >= Channel::kCount || valueUs <= 0 || valueUs > kMaxRetainedIntervalUs) {
        return;
    }
    const auto ch = static_cast<size_t>(channel);
    const uint64_t total = g_total[ch].load(std::memory_order_relaxed);
    // The release publishes all three fields as one logical sample. A reader
    // clamps itself to the last capacity entries before walking the slots.
    Sample& sample = g_ring[ch][total % kRingCapacity];
    sample.valueUs.store(static_cast<uint32_t>(valueUs), std::memory_order_relaxed);
    sample.observationTimeUs.store(observationTimeUs, std::memory_order_relaxed);
    sample.tag.store(tag, std::memory_order_relaxed);
    g_total[ch].store(total + 1, std::memory_order_release);
}

ChannelStats ComputeChannelStats(const uint32_t* samples, uint32_t count) {
    ChannelStats stats;
    if (!samples || count == 0) {
        return stats;
    }
    stats.samples = count;
    std::vector<uint32_t> sorted(samples, samples + count);
    std::sort(sorted.begin(), sorted.end());
    stats.medianUs = sorted[static_cast<size_t>(count * 5ULL / 10ULL)];
    const size_t p95Index = static_cast<size_t>(std::max<uint64_t>(1, count * 95ULL / 100ULL)) - 1;
    stats.p95Us = sorted[p95Index];
    double sum = 0;
    double sumSq = 0;
    uint64_t late = 0;
    for (uint32_t i = 0; i < count; i++) {
        const double sample = static_cast<double>(samples[i]);
        sum += sample;
        sumSq += sample * sample;
        late += samples[i] > stats.medianUs + kLateFlipThresholdUs ? 1u : 0u;
    }
    const double mean = sum / count;
    stats.meanUs = static_cast<uint32_t>(mean);
    stats.stddevUs = static_cast<uint32_t>(std::sqrt(std::max(0.0, (sumSq / count) - (mean * mean))));
    stats.minUs = sorted[0];
    stats.maxUs = sorted[count - 1];
    stats.latePermille = static_cast<uint32_t>((late * 1000 + count / 2) / count);
    return stats;
}

Signature Classify(const ChannelStats& display, const ChannelStats& presentation) {
    constexpr uint32_t kMinimumSamples = 120;
    // ETW display completion timestamps have a benign 8-12% tail just beyond
    // median+1.2 ms in the four clean Talos starts. The reproduced bad start is
    // unambiguous at 38% late and 2.46 ms stddev; do not label the normal tail as
    // a failure merely because it is more quantized than PresentStart.
    constexpr uint32_t kDegradedLatePermille = 150;
    constexpr uint32_t kDegradedStdDevUs = 1500;
    constexpr uint32_t kSmoothPresentationStdDevUs = 1200;
    if (display.samples < kMinimumSamples || presentation.samples < kMinimumSamples)
        return Signature::kInsufficient;
    const bool displayDegraded = display.latePermille >= kDegradedLatePermille ||
                                 display.stddevUs >= kDegradedStdDevUs;
    if (!displayDegraded)
        return Signature::kHealthy;
    if (presentation.stddevUs < kSmoothPresentationStdDevUs)
        return Signature::kDownstreamJitter;
    return Signature::kMixedJitter;
}

const char* SignatureName(Signature signature) {
    switch (signature) {
        case Signature::kHealthy:
            return "healthy";
        case Signature::kDownstreamJitter:
            return "downstream-jitter";
        case Signature::kMixedJitter:
            return "mixed-jitter";
        default:
            return "insufficient";
    }
}

namespace {

// Oldest-to-newest copy of the most recent `count` values of a channel.
std::vector<uint32_t> CollectRecent(Channel channel, uint32_t count) {
    std::vector<uint32_t> samples(count);
    const auto ch = static_cast<size_t>(channel);
    const uint64_t total = g_total[ch].load(std::memory_order_acquire);
    for (uint32_t i = 0; i < count; i++) {
        samples[i] = ReadRing(channel, total - count + i).valueUs;
    }
    return samples;
}

}  // namespace

ChannelStats Snapshot(Channel channel) {
    if (channel >= Channel::kCount) {
        return {};
    }
    const auto ch = static_cast<size_t>(channel);
    const uint64_t total = g_total[ch].load(std::memory_order_relaxed);
    const uint32_t count = static_cast<uint32_t>(std::min<uint64_t>(total, kRingCapacity));
    if (count == 0) {
        return {};
    }
    return ComputeChannelStats(CollectRecent(channel, count).data(), count);
}

ChannelStats SnapshotTaggedWindow(Channel channel, int64_t startTimeUs, int64_t endTimeUs) {
    if (channel >= Channel::kCount || startTimeUs <= 0 || endTimeUs < startTimeUs) {
        return {};
    }
    const auto ch = static_cast<size_t>(channel);
    const uint64_t total = g_total[ch].load(std::memory_order_acquire);
    const uint64_t first = total > kRingCapacity ? total - kRingCapacity : 0;
    std::vector<uint32_t> values;
    values.reserve(static_cast<size_t>(total - first));
    std::vector<uint64_t> tags;
    for (uint64_t i = first; i < total; ++i) {
        const SampleValue sample = ReadRing(channel, i);
        if (sample.tag == 0 || sample.observationTimeUs <= startTimeUs || sample.observationTimeUs > endTimeUs)
            continue;
        values.push_back(sample.valueUs);
        if (std::find(tags.begin(), tags.end(), sample.tag) == tags.end())
            tags.push_back(sample.tag);
    }
    ChannelStats stats = ComputeChannelStats(values.data(), static_cast<uint32_t>(values.size()));
    stats.segments = static_cast<uint32_t>(tags.size());
    return stats;
}

ChannelStats RecentCadence(Channel channel, uint32_t maxSamples) {
    if (channel >= Channel::kCount || maxSamples == 0) {
        return {};
    }
    const auto ch = static_cast<size_t>(channel);
    const uint64_t total = g_total[ch].load(std::memory_order_relaxed);
    const uint32_t count =
        static_cast<uint32_t>(std::min<uint64_t>(std::min<uint64_t>(total, maxSamples), kRingCapacity));
    if (count == 0) {
        return {};
    }
    return ComputeChannelStats(CollectRecent(channel, count).data(), count);
}

}  // namespace ce::pacing_health
