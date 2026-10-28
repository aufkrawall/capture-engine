#include "present_stall_period.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace ce::present_stall {

namespace {

constexpr double kTwoPi = 6.283185307179586;

// Resultant length of `us` folded onto `hz`, times relative to `originUs`.
double FoldedR(const std::vector<int64_t>& us, int64_t originUs, double hz) {
    if (us.empty()) {
        return 0.0;
    }
    double sumCos = 0.0;
    double sumSin = 0.0;
    for (const int64_t t : us) {
        const double phase = kTwoPi * hz * static_cast<double>(t - originUs) * 1e-6;
        sumCos += std::cos(phase);
        sumSin += std::sin(phase);
    }
    return std::hypot(sumCos, sumSin) / static_cast<double>(us.size());
}

double MedianIntervalUs(const std::vector<int64_t>& us) {
    if (us.size() < 2) {
        return 0.0;
    }
    std::vector<int64_t> intervals;
    intervals.reserve(us.size() - 1);
    for (size_t i = 1; i < us.size(); ++i) {
        intervals.push_back(us[i] - us[i - 1]);
    }
    const auto middle = intervals.begin() + static_cast<std::ptrdiff_t>(intervals.size() / 2);
    std::nth_element(intervals.begin(), middle, intervals.end());
    return static_cast<double>(*middle);
}

int Clamp(int written, size_t bufferSize) {
    if (written < 0) {
        return 0;
    }
    return static_cast<size_t>(written) >= bufferSize ? static_cast<int>(bufferSize - 1) : written;
}

}  // namespace

PeriodLock FindPeriodLock(const std::vector<int64_t>& callUs, const std::vector<int64_t>& slowUs) {
    PeriodLock lock;
    lock.calls = static_cast<uint32_t>(callUs.size());
    // Evenly thinned: the sweep's cost is steps x folded calls.
    std::vector<int64_t> folded;
    const size_t keep = std::min(slowUs.size(), kPeriodLockMaxSlowCalls);
    folded.reserve(keep);
    for (size_t i = 0; i < keep; ++i) {
        folded.push_back(slowUs[i * slowUs.size() / keep]);
    }
    lock.slowCalls = static_cast<uint32_t>(folded.size());
    if (folded.size() < kPeriodLockMinSlowCalls || folded.back() <= folded.front()) {
        return lock;
    }
    lock.analyzed = true;

    // A fold over span T dephases once the frequency is off by ~1/(2T): a
    // quarter of that keeps every peak on the grid.
    const int64_t originUs = folded.front();
    const double spanS = static_cast<double>(folded.back() - originUs) * 1e-6;
    const double stepHz = 1.0 / (4.0 * spanS);
    const size_t steps = static_cast<size_t>((kPeriodLockMaxHz - kPeriodLockMinHz) / stepHz) + 1;

    // Each call's phasor turns by a fixed angle per frequency step, so the
    // sweep needs one complex multiply per call and step instead of a sin/cos.
    const size_t n = folded.size();
    std::vector<double> re(n), im(n), turnRe(n), turnIm(n);
    for (size_t i = 0; i < n; ++i) {
        const double t = static_cast<double>(folded[i] - originUs) * 1e-6;
        re[i] = std::cos(kTwoPi * kPeriodLockMinHz * t);
        im[i] = std::sin(kTwoPi * kPeriodLockMinHz * t);
        turnRe[i] = std::cos(kTwoPi * stepHz * t);
        turnIm[i] = std::sin(kTwoPi * stepHz * t);
    }
    std::vector<double> r(steps);
    for (size_t k = 0; k < steps; ++k) {
        double sumRe = 0.0;
        double sumIm = 0.0;
        for (size_t i = 0; i < n; ++i) {
            sumRe += re[i];
            sumIm += im[i];
            const double nextRe = re[i] * turnRe[i] - im[i] * turnIm[i];
            im[i] = re[i] * turnIm[i] + im[i] * turnRe[i];
            re[i] = nextRe;
        }
        r[k] = std::hypot(sumRe, sumIm) / static_cast<double>(n);
    }

    std::vector<size_t> maxima;
    for (size_t k = 0; k < steps; ++k) {
        const bool aboveLeft = k == 0 || r[k] >= r[k - 1];
        const bool aboveRight = k + 1 == steps || r[k] > r[k + 1];
        if (aboveLeft && aboveRight) {
            maxima.push_back(k);
        }
    }
    std::sort(maxima.begin(), maxima.end(), [&](size_t a, size_t b) { return r[a] > r[b]; });

    const double callIntervalUs = MedianIntervalUs(callUs);
    // Side lobes sit ~1/T beside a peak; the next listed peak is a distinct one.
    const double distinctHz = 2.0 / spanS;
    std::array<double, kPeriodLockPeaks> acceptedHz{};
    for (const size_t k : maxima) {
        if (lock.peakCount == kPeriodLockPeaks) {
            break;
        }
        const double hz = kPeriodLockMinHz + static_cast<double>(k) * stepHz;
        bool distinct = true;
        for (size_t p = 0; p < lock.peakCount; ++p) {
            distinct = distinct && std::fabs(hz - acceptedHz[p]) >= distinctHz;
        }
        if (!distinct) {
            continue;
        }
        PeriodPeak peak;
        peak.periodMs = 1000.0 / hz;
        peak.r = r[k];
        peak.controlR = FoldedR(callUs, originUs, hz);
        peak.callsPerPeriod = callIntervalUs > 0.0 ? peak.periodMs * 1000.0 / callIntervalUs : 0.0;
        const double wholeCalls = std::round(peak.callsPerPeriod);
        peak.gameCadence = peak.controlR > kPeriodLockMaxControlR ||
                           (wholeCalls >= 1.0 && std::fabs(peak.callsPerPeriod - wholeCalls) < kPeriodLockWholeCallsSlack);
        acceptedHz[lock.peakCount] = hz;
        lock.peaks[lock.peakCount++] = peak;
    }
    if (lock.peakCount != 0) {
        lock.z = static_cast<double>(n) * lock.peaks[0].r * lock.peaks[0].r;
        lock.locked = !lock.peaks[0].gameCadence && lock.z >= kPeriodLockMinZ;
    }
    return lock;
}

void PeriodRecorder::Note(int64_t entryUs, bool slow) {
    if (entryUs <= 0 || writing_.test_and_set(std::memory_order_acquire)) {
        return;
    }
    const uint64_t index = written_.load(std::memory_order_relaxed);
    events_[index % kCapacity].store(entryUs * 2 + (slow ? 1 : 0), std::memory_order_relaxed);
    written_.store(index + 1, std::memory_order_release);
    writing_.clear(std::memory_order_release);
}

uint64_t PeriodRecorder::Drain(std::vector<int64_t>& callUs, std::vector<int64_t>& slowUs) {
    // Reading stays this far behind the writers: they would need that many
    // more calls during one drain to reach a slot being read.
    static constexpr uint64_t kReadable = kCapacity - 512;
    const uint64_t written = written_.load(std::memory_order_acquire);
    uint64_t lapped = 0;
    if (written - read_ > kReadable) {
        lapped = written - read_ - kReadable;
        read_ = written - kReadable;
    }
    for (; read_ < written; ++read_) {
        const int64_t value = events_[read_ % kCapacity].load(std::memory_order_relaxed);
        callUs.push_back(value / 2);
        if ((value & 1) != 0) {
            slowUs.push_back(value / 2);
        }
    }
    // Present threads finish out of entry order.
    std::sort(callUs.begin(), callUs.end());
    std::sort(slowUs.begin(), slowUs.end());
    return lapped;
}

int FormatPeriodLock(const PeriodLock& lock, uint64_t windowMs, uint64_t lapped, char* buffer, size_t bufferSize) {
    if (buffer == nullptr || bufferSize == 0) {
        return 0;
    }
    if (!lock.analyzed || lock.peakCount == 0) {
        return Clamp(std::snprintf(buffer, bufferSize,
                                   "[PRESENT STALL] period window=%llums calls=%u slow=%u lock=untested (too few) "
                                   "lapped=%llu",
                                   static_cast<unsigned long long>(windowMs), lock.calls, lock.slowCalls,
                                   static_cast<unsigned long long>(lapped)),
                     bufferSize);
    }
    const PeriodPeak& best = lock.peaks[0];
    const char* verdict = lock.locked ? "OUTSIDE CLOCK" : (best.gameCadence ? "game cadence" : "none, best");
    char peaks[192] = {};
    size_t used = 0;
    for (size_t i = 0; i < lock.peakCount && used + 1 < sizeof(peaks); ++i) {
        const int written = std::snprintf(peaks + used, sizeof(peaks) - used, " %.2f/%.2f%s", lock.peaks[i].periodMs,
                                          lock.peaks[i].r, lock.peaks[i].gameCadence ? "(game)" : "");
        if (written < 0) {
            break;
        }
        used += std::min(static_cast<size_t>(written), sizeof(peaks) - used - 1);
    }
    return Clamp(std::snprintf(buffer, bufferSize,
                               "[PRESENT STALL] period window=%llums calls=%u slow=%u lock=%s %.2fms (%.2f Hz) R=%.2f "
                               "Z=%.1f controlR=%.2f callsPerPeriod=%.2f | peaks(ms/R):%s | lapped=%llu",
                               static_cast<unsigned long long>(windowMs), lock.calls, lock.slowCalls, verdict,
                               best.periodMs, 1000.0 / best.periodMs, best.r, lock.z, best.controlR,
                               best.callsPerPeriod, peaks, static_cast<unsigned long long>(lapped)),
                 bufferSize);
}

PeriodRecorder& PresentPeriodRecorder() {
    static PeriodRecorder recorder;
    return recorder;
}

}  // namespace ce::present_stall
