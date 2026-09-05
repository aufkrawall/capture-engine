#include "pacing_health_telemetry.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <vector>

namespace ce::pacing_health {
namespace {

// 2048 samples retain 20+ s at 90 Hz output cadence and cost 8 KB per channel.
constexpr uint32_t kRingCapacity = 2048;
// Everything above this is a stall/hitch already covered by dedicated logs,
// not part of the cadence this ring describes.
constexpr int64_t kMaxRetainedIntervalUs = 5'000'000;

std::atomic<uint32_t> g_ring[static_cast<size_t>(Channel::kCount)][kRingCapacity];
std::atomic<uint64_t> g_total[static_cast<size_t>(Channel::kCount)]{};

uint32_t ReadRing(Channel channel, uint32_t backIndex) {
    const auto ch = static_cast<size_t>(channel);
    const uint64_t total = g_total[ch].load(std::memory_order_relaxed);
    const uint64_t index = (total - backIndex) % kRingCapacity;
    return g_ring[ch][index].load(std::memory_order_relaxed);
}

}  // namespace

void Observe(Channel channel, int64_t intervalUs) {
    if (channel >= Channel::kCount || intervalUs <= 0 || intervalUs > kMaxRetainedIntervalUs) {
        return;
    }
    const auto ch = static_cast<size_t>(channel);
    const uint64_t total = g_total[ch].load(std::memory_order_relaxed);
    // Single writer per channel, so the slot cannot be concurrently rewritten.
    g_ring[ch][total % kRingCapacity].store(static_cast<uint32_t>(intervalUs), std::memory_order_relaxed);
    g_total[ch].store(total + 1, std::memory_order_relaxed);
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
    stats.stddevUs = static_cast<uint32_t>(std::sqrt(std::max(0.0, (sumSq / count) - (mean * mean))));
    stats.maxUs = sorted[count - 1];
    stats.latePermille = static_cast<uint32_t>((late * 1000 + count / 2) / count);
    return stats;
}

namespace {

// Oldest-to-newest copy of the most recent `count` samples of a channel.
std::vector<uint32_t> CollectRecent(Channel channel, uint32_t count) {
    std::vector<uint32_t> samples(count);
    for (uint32_t i = 0; i < count; i++) {
        samples[i] = ReadRing(channel, count - i);
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
