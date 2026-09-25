#pragma once

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <unordered_map>
#include <vector>

#include "../common/display_timing_shared.h"
#include "display_timing_health.h"
#include "display_timing_intervals.h"
#include "display_timing_refresh_bound.h"
#include "display_timing_service.h"

// One confirmed display transition, ready to be published to every overlay
// that follows its process.
struct DisplayTimingPublication {
    uint32_t processId = 0;
    int64_t timestampQpc = 0;
    int64_t presentStartQpc = 0;
    bool screenTimeResolved = true;
    // A kernel sync completion of a present that asked for SyncInterval >= 1:
    // the only transitions the refresh bound may apply to.
    bool synchronizedFlip = false;
    uint32_t displaySource = 0;
};

// Per-output publication state: ordering, the refresh-bounded graph time, and
// the window statistics the health line reports. Kept out of the service so
// that translation unit stays about ETW plumbing.
class DisplayTimingOutputs {
public:
    void SetQpcFrequency(int64_t frequency) { qpcFrequency_ = frequency; }
    void Track(SharedDisplayTiming* output) { outputs_.try_emplace(output); }
    void Forget(SharedDisplayTiming* output) { outputs_.erase(output); }

    // `periodUs(source)` is the display's minimum refresh period or 0;
    // `firstBlankQpc(source, from, until)` the earliest observed blank or 0.
    template <typename PeriodUs, typename FirstBlankQpc>
    void Publish(const std::vector<DisplayTimingTarget>& targets, const DisplayTimingPublication& sample,
                 int64_t publishUs, PeriodUs&& periodUs, FirstBlankQpc&& firstBlankQpc) {
        for (const auto& target : targets) {
            if (!target.output)
                continue;
            if (target.sourcePid != sample.processId && target.rendererPid != sample.processId)
                continue;
            const auto found = outputs_.find(target.output);
            if (found == outputs_.end())
                continue;
            OutputState& state = found->second;
            if (sample.timestampQpc <= state.lastTimestamp) {
                target.output->droppedTimestampCount.fetch_add(1, std::memory_order_relaxed);
                ++regressed_;
                continue;
            }
            state.lastTimestamp = sample.timestampQpc;
            const int64_t screenTimeUs = DisplayTimingQpcToUs(sample.timestampQpc, qpcFrequency_);
            const int64_t presentStartTimeUs = DisplayTimingQpcToUs(sample.presentStartQpc, qpcFrequency_);
            const int64_t period = sample.synchronizedFlip ? periodUs(sample.displaySource) : 0;
            const RefreshBoundDecision graph = state.graph.Apply(
                screenTimeUs, sample.synchronizedFlip, period, [&](int64_t fromUs, int64_t untilUs) {
                    const int64_t blank = firstBlankQpc(sample.displaySource,
                                                        DisplayTimingUsToQpc(fromUs, qpcFrequency_),
                                                        DisplayTimingUsToQpc(untilUs, qpcFrequency_));
                    return blank > 0 ? DisplayTimingQpcToUs(blank, qpcFrequency_) : 0;
                });
            state.Count(graph, screenTimeUs, period);
            state.intervals.Observe(screenTimeUs);
            state.graphIntervals.Observe(graph.graphUs);
            if (presentStartTimeUs > 0 && screenTimeUs >= presentStartTimeUs)
                state.presentToDisplay.Observe(screenTimeUs - presentStartTimeUs);
            target.output->Publish(screenTimeUs, publishUs, presentStartTimeUs, sample.screenTimeResolved,
                                   graph.graphUs);
            ++published_;
        }
    }

    // The busiest output is the one the overlay is reading; averaging several
    // would hide exactly the shape these statistics exist to expose. Every
    // output starts a new window.
    void Snapshot(DisplayTimingHealth& health) {
        OutputState* busiest = nullptr;
        for (auto& output : outputs_) {
            if (!busiest || output.second.intervals.count() > busiest->intervals.count())
                busiest = &output.second;
        }
        if (busiest) {
            SetPublishedIntervals(health, busiest->intervals);
            SetGraphIntervals(health, busiest->graphIntervals);
            SetPresentToDisplay(health, busiest->presentToDisplay);
            health.refreshPeriodUs = busiest->periodUs;
            health.refreshBoundEligible = busiest->eligible;
            health.refreshBoundApplied = busiest->bounded;
            health.refreshBoundBlankMissing = busiest->blankMissing;
            health.refreshBoundShiftMeanUs =
                busiest->bounded != 0 ? busiest->shiftTotalUs / static_cast<int64_t>(busiest->bounded) : 0;
            health.refreshBoundShiftMaxUs = busiest->shiftMaxUs;
        }
        for (auto& output : outputs_)
            output.second.StartWindow();
    }

    uint64_t published() const { return published_; }
    uint64_t regressed() const { return regressed_; }

private:
    struct OutputState {
        int64_t lastTimestamp = 0;
        RefreshBoundedGraphTime graph;
        DisplayIntervalStats intervals;
        DisplayIntervalStats graphIntervals;
        DisplayDurationStats presentToDisplay;
        int64_t periodUs = 0;
        uint64_t eligible = 0;
        uint64_t bounded = 0;
        uint64_t blankMissing = 0;
        int64_t shiftTotalUs = 0;
        int64_t shiftMaxUs = 0;

        void Count(const RefreshBoundDecision& decision, int64_t screenTimeUs, int64_t period) {
            if (period > 0)
                periodUs = period;
            eligible += decision.eligible ? 1u : 0u;
            blankMissing += decision.blankMissing ? 1u : 0u;
            if (!decision.bounded)
                return;
            ++bounded;
            const int64_t shift = decision.graphUs - screenTimeUs;
            shiftTotalUs += shift;
            shiftMaxUs = std::max(shiftMaxUs, shift);
        }

        void StartWindow() {
            intervals.StartWindow();
            graphIntervals.StartWindow();
            presentToDisplay.StartWindow();
            eligible = 0;
            bounded = 0;
            blankMissing = 0;
            shiftTotalUs = 0;
            shiftMaxUs = 0;
        }
    };

    std::unordered_map<SharedDisplayTiming*, OutputState> outputs_;
    int64_t qpcFrequency_ = 0;
    uint64_t published_ = 0;
    uint64_t regressed_ = 0;
};
