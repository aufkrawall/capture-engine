#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <mutex>

namespace ce::system_latency {

enum class Source : uint8_t {
    Unavailable,
    ReflexMarkers,
    Estimated,
};

struct Snapshot {
    float milliseconds = 0.0f;
    Source source = Source::Unavailable;
    uint32_t sampleCount = 0;
    bool valid = false;
};

struct NativeFrameReport {
    uint64_t frameId = 0;
    uint64_t inputSampleTimeUs = 0;
    uint64_t simulationStartTimeUs = 0;
    uint64_t presentStartTimeUs = 0;
    uint64_t gpuRenderEndTimeUs = 0;
};

struct NativeReport {
    static constexpr size_t kCapacity = 64;
    std::array<NativeFrameReport, kCapacity> frames{};
    size_t count = 0;
};

using SupplementalNativeReportProvider = bool (*)(NativeReport& report);

// Implemented separately for the injected DirectX hook and Vulkan layer.
bool QueryNativeReport(void* device, NativeReport& report);
void SetSupplementalNativeReportProvider(SupplementalNativeReportProvider provider);

inline const char* SourceLogLabel(Source source) {
    switch (source) {
        case Source::ReflexMarkers:
            return "Reflex/PCL markers (input wait estimated)";
        case Source::Estimated:
            return "presentation/display estimate";
        default:
            return "unavailable";
    }
}

inline const char* SourceOverlayLabel(Source source) {
    switch (source) {
        case Source::ReflexMarkers:
            return "PC Latency~";
        case Source::Estimated:
            return "Latency est.";
        default:
            return "PC Latency";
    }
}

class Tracker {
public:
    void ObservePresent(int64_t presentTimeUs) {
        // Native-report processing is infrequent but can scan up to 64 frames.
        // Never make the present hot path wait behind that diagnostic work.
        std::unique_lock<std::mutex> lock(mutex_, std::try_to_lock);
        if (!lock.owns_lock() || presentTimeUs <= 0)
            return;

        if (!presents_.Empty()) {
            const int64_t previous = presents_.Back();
            if (presentTimeUs <= previous) {
                if (previous - presentTimeUs > kClockResetThresholdUs)
                    ResetMeasurementsLocked();
                else
                    return;
            } else {
                const int64_t intervalUs = presentTimeUs - previous;
                if (intervalUs < kDuplicateThresholdUs)
                    return;
                if (intervalUs <= kMaximumIntervalUs)
                    presentIntervals_.Push(intervalUs);
            }
        }
        presents_.Push(presentTimeUs);
    }

    void ObserveDisplay(int64_t screenTimeUs, int64_t presentStartTimeUs = 0) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (screenTimeUs <= 0)
            return;
        if (!displays_.Empty() && screenTimeUs <= displays_.Back()) {
            if (displays_.Back() - screenTimeUs > kClockResetThresholdUs)
                ResetMeasurementsLocked();
            else
                return;
        }

        displays_.Push(screenTimeUs);
        displayPresentStarts_.Push(presentStartTimeUs);
        UpdateFallbackLocked(screenTimeUs);
    }

    void SetFrameGeneration(float baseFps, int multiplier) {
        // FG publication happens on the Present path. Keep this metadata
        // lock-free so an infrequent native-report scan can never stall it.
        fgBaseFps_.store(std::isfinite(baseFps) && baseFps > 0.0f ? baseFps : 0.0f,
                         std::memory_order_relaxed);
        fgMultiplier_.store(multiplier >= 2 ? multiplier : 1, std::memory_order_relaxed);
    }

    void SubmitNativeReport(const NativeReport& report) {
        std::array<NativeFrameReport, NativeReport::kCapacity> validFrames{};
        size_t validCount = 0;
        const size_t reportCount = (std::min)(report.count, report.frames.size());
        for (size_t i = 0; i < reportCount; ++i) {
            const auto& frame = report.frames[i];
            if (!IsValidNativeFrame(frame))
                continue;
            validFrames[validCount++] = frame;
        }
        if (validCount == 0)
            return;

        std::sort(validFrames.begin(), validFrames.begin() + validCount,
                  [](const NativeFrameReport& a, const NativeFrameReport& b) {
                      return a.presentStartTimeUs < b.presentStartTimeUs;
                  });
        int64_t samplingIntervalUs = MedianSimulationInterval(validFrames, validCount);

        std::lock_guard<std::mutex> lock(mutex_);
        if (samplingIntervalUs <= 0)
            samplingIntervalUs = ResolveWorkIntervalLocked();
        // NVIDIA documents the average input-to-frame-start heuristic as
        // invalid below 10 FPS. Fail closed instead of publishing a number
        // whose estimated input slice is outside that supported regime.
        if (samplingIntervalUs <= 0 || samplingIntervalUs > kMaximumSamplingIntervalUs)
            return;
        for (size_t displayIndex = 0; displayIndex < displays_.Size(); ++displayIndex) {
            const int64_t screenTimeUs = displays_.At(displayIndex);
            if (screenTimeUs <= lastNativeDisplayTimeUs_)
                continue;

            // The sensor associates every displayed transition with the
            // runtime PresentStart that produced it. Use that causal boundary
            // when available: DLSS-G's asynchronous pacer can let markers for
            // newer application frames occur before an older frame reaches
            // the screen, so screenTime alone selects a future frame.
            int64_t markerCutoffUs = screenTimeUs;
            if (displayIndex < displayPresentStarts_.Size()) {
                const int64_t associatedPresentStartUs = displayPresentStarts_.At(displayIndex);
                if (associatedPresentStartUs > 0 && associatedPresentStartUs <= screenTimeUs)
                    markerCutoffUs = associatedPresentStartUs;
            }

            const NativeFrameReport* candidate = nullptr;
            for (size_t frameIndex = 0; frameIndex < validCount; ++frameIndex) {
                const auto& frame = validFrames[frameIndex];
                if (frame.presentStartTimeUs <= static_cast<uint64_t>(lastNativePresentTimeUs_))
                    continue;
                if (frame.presentStartTimeUs > static_cast<uint64_t>(markerCutoffUs))
                    continue;
                if (NativeReadyTimeUs(frame) > static_cast<uint64_t>(screenTimeUs))
                    continue;
                if (!candidate || frame.presentStartTimeUs > candidate->presentStartTimeUs)
                    candidate = &frame;
            }
            if (!candidate)
                continue;

            const int64_t presentTimeUs = ToSignedTimestamp(candidate->presentStartTimeUs);
            const int64_t presentToDisplayUs = screenTimeUs - presentTimeUs;
            if (presentToDisplayUs < 0 || presentToDisplayUs > kMaximumPresentToDisplayUs)
                continue;

            const int64_t simulationStartUs = ToSignedTimestamp(candidate->simulationStartTimeUs);
            int64_t displayedSamplingIntervalUs = samplingIntervalUs;
            const int64_t displayedSimulationIntervalUs = simulationStartUs - lastNativeSimulationStartTimeUs_;
            if (lastNativeSimulationStartTimeUs_ > 0 &&
                displayedSimulationIntervalUs >= kDuplicateThresholdUs &&
                displayedSimulationIntervalUs <= kMaximumIntervalUs) {
                displayedSamplingIntervalUs = (std::max)(
                    displayedSamplingIntervalUs,
                    MedianWithCandidate(nativeDisplayedSimulationIntervals_, displayedSimulationIntervalUs));
            }

            // PCL uses half a base sampling interval for average input wait,
            // then adds every complete base interval whose frame was dropped.
            // Equivalently: displayed interval minus half the base interval.
            const int64_t estimatedInputWaitUs = displayedSamplingIntervalUs - samplingIntervalUs / 2;
            if (estimatedInputWaitUs > 0) {
                const int64_t estimatedInputTimeUs = simulationStartUs - estimatedInputWaitUs;
                if (estimatedInputTimeUs > 0 && screenTimeUs >= estimatedInputTimeUs) {
                    const int64_t totalUs = screenTimeUs - estimatedInputTimeUs;
                    if (IsValidTotalLatency(totalUs))
                        nativeEstimatedSamples_.Add(static_cast<float>(totalUs) / 1000.0f, screenTimeUs);
                }
            }

            if (lastNativeSimulationStartTimeUs_ > 0 &&
                displayedSimulationIntervalUs >= kDuplicateThresholdUs &&
                displayedSimulationIntervalUs <= kMaximumIntervalUs) {
                nativeDisplayedSimulationIntervals_.Push(displayedSimulationIntervalUs);
            }
            if (lastNativeSimulationStartTimeUs_ == 0 || simulationStartUs > lastNativeSimulationStartTimeUs_)
                lastNativeSimulationStartTimeUs_ = simulationStartUs;
            lastNativePresentTimeUs_ = presentTimeUs;
            lastNativeDisplayTimeUs_ = screenTimeUs;
        }
    }

    Snapshot GetSnapshot(int64_t currentQpcUs) const {
        std::lock_guard<std::mutex> lock(mutex_);
        if (nativeEstimatedSamples_.IsFresh(currentQpcUs))
            return nativeEstimatedSamples_.MakeSnapshot(Source::ReflexMarkers);
        if (fallbackSamples_.IsFresh(currentQpcUs))
            return fallbackSamples_.MakeSnapshot(Source::Estimated);
        return {};
    }

    void ResetDisplayHistory() {
        std::lock_guard<std::mutex> lock(mutex_);
        ResetMeasurementsLocked();
    }

private:
    static constexpr int64_t kDuplicateThresholdUs = 100;
    static constexpr int64_t kClockResetThresholdUs = 1'000'000;
    static constexpr int64_t kMaximumIntervalUs = 250'000;
    static constexpr int64_t kMaximumSamplingIntervalUs = 100'000;
    static constexpr int64_t kMaximumPresentToDisplayUs = 250'000;
    static constexpr int64_t kMaximumTotalLatencyUs = 500'000;
    static constexpr int64_t kFreshnessUs = 2'000'000;

    template <size_t Capacity>
    class ValueRing {
    public:
        void Push(int64_t value) {
            if (count_ < Capacity) {
                values_[(start_ + count_) % Capacity] = value;
                ++count_;
            } else {
                values_[start_] = value;
                start_ = (start_ + 1) % Capacity;
            }
        }

        int64_t At(size_t index) const {
            return values_[(start_ + index) % Capacity];
        }
        int64_t Back() const {
            return At(count_ - 1);
        }
        size_t Size() const {
            return count_;
        }
        bool Empty() const {
            return count_ == 0;
        }
        void Clear() {
            start_ = 0;
            count_ = 0;
        }

    private:
        std::array<int64_t, Capacity> values_{};
        size_t start_ = 0;
        size_t count_ = 0;
    };

    class SampleWindow {
    public:
        void Add(float milliseconds, int64_t sampleTimeUs) {
            if (lastSampleTimeUs_ > 0 && sampleTimeUs - lastSampleTimeUs_ > kFreshnessUs)
                Clear();
            values_[writeIndex_] = milliseconds;
            writeIndex_ = (writeIndex_ + 1) % values_.size();
            count_ = (std::min)(count_ + 1, values_.size());
            lastSampleTimeUs_ = sampleTimeUs;
        }

        bool IsFresh(int64_t currentQpcUs) const {
            return count_ > 0 && lastSampleTimeUs_ > 0 && currentQpcUs >= lastSampleTimeUs_ &&
                   currentQpcUs - lastSampleTimeUs_ <= kFreshnessUs;
        }

        Snapshot MakeSnapshot(Source source) const {
            std::array<float, 32> sorted{};
            for (size_t i = 0; i < count_; ++i)
                sorted[i] = values_[i];
            std::sort(sorted.begin(), sorted.begin() + count_);
            const size_t trim = count_ >= 8 ? 1 : 0;
            float sum = 0.0f;
            for (size_t i = trim; i < count_ - trim; ++i)
                sum += sorted[i];
            const size_t retained = count_ - 2 * trim;
            return {sum / static_cast<float>(retained), source, static_cast<uint32_t>(count_), true};
        }

        void Clear() {
            values_.fill(0.0f);
            writeIndex_ = 0;
            count_ = 0;
            lastSampleTimeUs_ = 0;
        }

    private:
        std::array<float, 32> values_{};
        size_t writeIndex_ = 0;
        size_t count_ = 0;
        int64_t lastSampleTimeUs_ = 0;
    };

    static int64_t ToSignedTimestamp(uint64_t timestampUs) {
        if (timestampUs == 0 || timestampUs > static_cast<uint64_t>((std::numeric_limits<int64_t>::max)()))
            return 0;
        return static_cast<int64_t>(timestampUs);
    }

    static bool IsValidNativeFrame(const NativeFrameReport& frame) {
        const int64_t simulationStartUs = ToSignedTimestamp(frame.simulationStartTimeUs);
        const int64_t presentStartUs = ToSignedTimestamp(frame.presentStartTimeUs);
        return simulationStartUs > 0 && presentStartUs >= simulationStartUs &&
               presentStartUs - simulationStartUs <= kMaximumIntervalUs;
    }

    static uint64_t NativeReadyTimeUs(const NativeFrameReport& frame) {
        if (frame.gpuRenderEndTimeUs >= frame.presentStartTimeUs &&
            frame.gpuRenderEndTimeUs - frame.presentStartTimeUs <= static_cast<uint64_t>(kMaximumTotalLatencyUs)) {
            return frame.gpuRenderEndTimeUs;
        }
        return frame.presentStartTimeUs;
    }

    static bool IsValidTotalLatency(int64_t totalUs) {
        return totalUs > 0 && totalUs <= kMaximumTotalLatencyUs;
    }

    static int64_t MedianSimulationInterval(
        const std::array<NativeFrameReport, NativeReport::kCapacity>& frames, size_t count) {
        std::array<int64_t, NativeReport::kCapacity - 1> intervals{};
        size_t intervalCount = 0;
        for (size_t i = 1; i < count; ++i) {
            const int64_t previous = ToSignedTimestamp(frames[i - 1].simulationStartTimeUs);
            const int64_t current = ToSignedTimestamp(frames[i].simulationStartTimeUs);
            const int64_t deltaUs = current - previous;
            if (deltaUs >= kDuplicateThresholdUs && deltaUs <= kMaximumIntervalUs)
                intervals[intervalCount++] = deltaUs;
        }
        if (intervalCount == 0)
            return 0;
        std::sort(intervals.begin(), intervals.begin() + intervalCount);
        return intervals[intervalCount / 2];
    }

    template <size_t Capacity>
    static int64_t MedianRing(const ValueRing<Capacity>& values) {
        if (values.Empty())
            return 0;
        std::array<int64_t, Capacity> sorted{};
        for (size_t i = 0; i < values.Size(); ++i)
            sorted[i] = values.At(i);
        std::sort(sorted.begin(), sorted.begin() + values.Size());
        return sorted[values.Size() / 2];
    }

    template <size_t Capacity>
    static int64_t MedianWithCandidate(const ValueRing<Capacity>& values, int64_t candidate) {
        std::array<int64_t, Capacity + 1> sorted{};
        const size_t first = values.Size() == Capacity ? 1 : 0;
        size_t count = 0;
        for (size_t i = first; i < values.Size(); ++i)
            sorted[count++] = values.At(i);
        sorted[count++] = candidate;
        std::sort(sorted.begin(), sorted.begin() + count);
        return sorted[count / 2];
    }

    int64_t ResolveFgBaseIntervalLocked() const {
        const int fgMultiplier = fgMultiplier_.load(std::memory_order_relaxed);
        const float fgBaseFps = fgBaseFps_.load(std::memory_order_relaxed);
        if (fgMultiplier < 2 || fgBaseFps <= 0.0f)
            return 0;
        return std::llround(1'000'000.0 / static_cast<double>(fgBaseFps));
    }

    int64_t ResolveWorkIntervalLocked() const {
        return (std::max)(MedianRing(presentIntervals_), ResolveFgBaseIntervalLocked());
    }

    void UpdateFallbackLocked(int64_t screenTimeUs) {
        int64_t matchedPresentUs = 0;
        for (size_t i = presents_.Size(); i > 0; --i) {
            const int64_t candidateUs = presents_.At(i - 1);
            if (candidateUs <= screenTimeUs && candidateUs > lastFallbackPresentTimeUs_) {
                matchedPresentUs = candidateUs;
                break;
            }
        }
        if (matchedPresentUs == 0)
            return;
        if (lastFallbackPresentTimeUs_ > 0) {
            const int64_t displayedPresentIntervalUs = matchedPresentUs - lastFallbackPresentTimeUs_;
            if (displayedPresentIntervalUs >= kDuplicateThresholdUs &&
                displayedPresentIntervalUs <= kMaximumIntervalUs) {
                fallbackDisplayedPresentIntervals_.Push(displayedPresentIntervalUs);
            }
        }
        lastFallbackPresentTimeUs_ = matchedPresentUs;

        const int64_t presentToDisplayUs = screenTimeUs - matchedPresentUs;
        const int64_t workIntervalUs = ResolveWorkIntervalLocked();
        if (presentToDisplayUs < 0 || presentToDisplayUs > kMaximumPresentToDisplayUs || workIntervalUs <= 0 ||
            workIntervalUs > kMaximumSamplingIntervalUs)
            return;

        // Without game markers, approximate one base frame of simulation/render
        // work. Input wait is half a base interval plus every full skipped
        // interval before a frame reaches the screen, matching PCL's dropped-
        // frame treatment. FG base cadence is a floor because generated
        // Presents do not add input-sampling points.
        const int64_t displayedInputIntervalUs =
            (std::max)(workIntervalUs, MedianRing(fallbackDisplayedPresentIntervals_));
        const int64_t estimatedInputWaitUs = displayedInputIntervalUs - workIntervalUs / 2;
        const int64_t totalUs = presentToDisplayUs + workIntervalUs + estimatedInputWaitUs;
        if (IsValidTotalLatency(totalUs))
            fallbackSamples_.Add(static_cast<float>(totalUs) / 1000.0f, screenTimeUs);
    }

    void ResetMeasurementsLocked() {
        presents_.Clear();
        presentIntervals_.Clear();
        displays_.Clear();
        displayPresentStarts_.Clear();
        fallbackDisplayedPresentIntervals_.Clear();
        nativeDisplayedSimulationIntervals_.Clear();
        nativeEstimatedSamples_.Clear();
        fallbackSamples_.Clear();
        lastFallbackPresentTimeUs_ = 0;
        lastNativePresentTimeUs_ = 0;
        lastNativeDisplayTimeUs_ = 0;
        lastNativeSimulationStartTimeUs_ = 0;
    }

    mutable std::mutex mutex_;
    ValueRing<256> presents_;
    ValueRing<32> presentIntervals_;
    ValueRing<256> displays_;
    ValueRing<256> displayPresentStarts_;
    ValueRing<32> fallbackDisplayedPresentIntervals_;
    ValueRing<32> nativeDisplayedSimulationIntervals_;
    SampleWindow nativeEstimatedSamples_;
    SampleWindow fallbackSamples_;
    int64_t lastFallbackPresentTimeUs_ = 0;
    int64_t lastNativePresentTimeUs_ = 0;
    int64_t lastNativeDisplayTimeUs_ = 0;
    int64_t lastNativeSimulationStartTimeUs_ = 0;
    std::atomic<float> fgBaseFps_{0.0f};
    std::atomic<int> fgMultiplier_{1};
};

}  // namespace ce::system_latency
