#pragma once
#include "pacing_trace.h"
#include "pacing_trace_boundary.h"
#include <algorithm>
#include <cstdlib>
#include <map>
#include <tuple>

namespace ce::pacing_trace {
struct TraceMetric {
    uint64_t samples = 0, meanUs = 0, p95Us = 0, maxUs = 0;
};

inline TraceMetric Summarize(std::vector<uint64_t> values) {
    if (values.empty()) return {};
    std::sort(values.begin(), values.end());
    uint64_t sum = 0;
    for (const auto value : values) sum += value;
    return {values.size(), sum / values.size(), values[(values.size() * 95 + 99) / 100 - 1], values.back()};
}

// Screen time decomposed against the producing callback rather than against
// Present alone. Under free-running VRR the flip happens when the frame is
// finished, so a Present-anchored latency cannot separate "the runtime paced it
// late" from "it was still not on screen"; pacerWait and callbackToDisplay do.
struct DisplayAnchoredMetrics {
    TraceMetric pacerWait;         // runtime Present entry - callback end (runtime's own hold)
    TraceMetric presentToDisplay;  // screen time - host PresentStart
    TraceMetric callbackToDisplay; // screen time - callback end (the composite gate)
    // Present only when CE_FG_GPU_TIMING armed the GPU bracket. gpuStartDelay is
    // how long after the callback CE's own commands actually began executing:
    // it separates "the GPU was already behind" from "the frame waited after CE
    // was done", which no CPU span can distinguish.
    TraceMetric gpuStartDelay;     // GPU start of CE's commands - callback end
    TraceMetric gpuDuration;       // GPU end - GPU start of CE's commands
};

struct TraceAnalysis {
    int64_t windowUs = 0;
    uint64_t epoch = 0;
    TraceMetric proxyPrework, proxyRuntime, detour, forwarding, callback;
    TraceMetric completedMarkerAge, pendingMarkerAge;
    uint64_t matchedPresents = 0, unmatchedBegins = 0, unmatchedEnds = 0, invalidPresents = 0;
    uint64_t unmatchedMarkers = 0, latestComplete = 0, latestPending = 0;
    DisplayAnchoredMetrics application, generated;
    uint64_t displayPairs = 0, displayPairsMatched = 0, displayPairsUnresolved = 0;
};

// A host PresentStart and CE's own Present-entry timestamp describe the same
// call from two clocks, measured a few hundred microseconds apart. The tolerance
// stays far below one output interval so a pair can never bind to its neighbour.
constexpr int64_t kDisplayAssociationToleranceUs = 1500;

// Background-only analysis of a copied, QPC-sorted snapshot. No GPU operations.
// Inclusive nested durations must not be added or mistaken for driver/GPU time.
inline TraceAnalysis Analyze(const std::vector<Event>& events, int64_t windowUs = 10'000'000) {
    TraceAnalysis result;
    if (events.empty() || windowUs <= 0) return result;
    const auto end = events.back().timeUs;
    const auto start = end - windowUs;
    result.epoch = events.back().epoch;
    result.windowUs = std::min(windowUs, end - events.front().timeUs);
    using Key = std::pair<uint32_t, uint64_t>;
    struct Pending { Event begin; int64_t forward = 0; bool invalid = false; };
    std::map<Key, Pending> begins;
    using MarkerKey = std::tuple<uint64_t, uint64_t, uint64_t>;
    std::map<MarkerKey, int64_t> markers;
    std::vector<uint64_t> prework, runtime, detour, forwarding, callback, completeAge, pendingAge;
    // Presenter-thread frames, in Present order: callback end plus the Present
    // entry that carried it. A thread that does not produce the exact
    // begin/end/Present triple contributes nothing rather than a guess.
    struct PresenterFrame { int64_t callbackEnd = 0, presentBegin = 0; bool generated = false; };
    struct PresenterState { int64_t callbackEnd = 0; bool generated = false, armed = false; };
    std::map<uint32_t, PresenterState> presenters;
    std::vector<PresenterFrame> frames;
    struct DisplaySample { int64_t screen = 0, presentStart = 0; };
    std::vector<DisplaySample> displays;
    std::vector<Event> gpuSpans;
    for (const auto& event : events) {
        if (event.epoch != result.epoch) continue;
        // Retain preceding marker records to resolve observations near the window edge.
        if (event.kind == Kind::Marker)
            markers[{event.c, event.b, event.a}] = event.timeUs;
        if (event.timeUs < start) continue;
        if (event.kind == Kind::CallbackBegin) {
            auto& state = presenters[event.thread];
            state.generated = event.flags != 0;
            state.armed = false;
            state.callbackEnd = 0;
        } else if (event.kind == Kind::CallbackEnd) {
            callback.push_back(event.a);
            auto& state = presenters[event.thread];
            state.callbackEnd = event.timeUs;
            state.armed = true;
        } else if (event.kind == Kind::GpuSpan) {
            // A calibration that has not settled yet publishes an unusable pair
            // rather than a plausible one; drop it instead of averaging it in.
            if (event.c && event.a >= event.c && event.b >= event.a) gpuSpans.push_back(event);
        } else if (event.kind == Kind::DisplayPair) {
            ++result.displayPairs;
            if (event.flags == 0) ++result.displayPairsUnresolved;
            else if (event.a && event.timeUs >= static_cast<int64_t>(event.a))
                displays.push_back({event.timeUs, static_cast<int64_t>(event.a)});
        }
        if (event.kind == Kind::MarkerObserved && event.flags == 1) {
            const bool complete = event.a == event.b;
            if (complete) ++result.latestComplete;
            else ++result.latestPending;
            const auto marker = markers.find({event.object, event.c, event.b});
            if (marker == markers.end() || marker->second > event.timeUs) ++result.unmatchedMarkers;
            else (complete ? completeAge : pendingAge).push_back(
                static_cast<uint64_t>(event.timeUs - marker->second));
        }
        const Key key{event.thread, event.id};
        if (event.kind == Kind::PresentBegin &&
            (event.flags == static_cast<uint32_t>(PresentStage::Detour) ||
             event.flags == static_cast<uint32_t>(PresentStage::Detour1))) {
            auto& state = presenters[event.thread];
            if (state.armed && state.callbackEnd <= event.timeUs)
                frames.push_back({state.callbackEnd, event.timeUs, state.generated});
            state.armed = false;
        }
        if (event.kind == Kind::PresentBegin) {
            const auto [it, inserted] = begins.emplace(key, Pending{event});
            if (!inserted) it->second.invalid = true;
        } else if (event.kind == Kind::PresentForward || event.kind == Kind::PresentEnd) {
            const auto it = begins.find(key);
            if (it == begins.end()) {
                if (event.kind == Kind::PresentEnd) ++result.unmatchedEnds;
                continue;
            }
            auto& pending = it->second;
            if (event.flags != pending.begin.flags || event.object != pending.begin.object ||
                event.timeUs < pending.begin.timeUs) pending.invalid = true;
            if (event.kind == Kind::PresentForward) {
                if (pending.forward) pending.invalid = true;
                pending.forward = event.timeUs;
                continue;
            }
            if (event.a != static_cast<uint64_t>(event.timeUs - pending.begin.timeUs) ||
                pending.forward > event.timeUs) pending.invalid = true;
            if (pending.invalid) ++result.invalidPresents;
            else {
                ++result.matchedPresents;
                if (event.flags <= 1 && pending.forward) {
                    prework.push_back(static_cast<uint64_t>(pending.forward - pending.begin.timeUs));
                    runtime.push_back(static_cast<uint64_t>(event.timeUs - pending.forward));
                } else if (event.flags == 2 || event.flags == 3) detour.push_back(event.a);
                else if (event.flags == 4 || event.flags == 5) forwarding.push_back(event.a);
            }
            begins.erase(it);
        }
    }
    result.unmatchedBegins = begins.size();
    result.proxyPrework = Summarize(std::move(prework));
    result.proxyRuntime = Summarize(std::move(runtime));
    result.detour = Summarize(std::move(detour));
    result.forwarding = Summarize(std::move(forwarding));
    result.callback = Summarize(std::move(callback));
    result.completedMarkerAge = Summarize(std::move(completeAge));
    result.pendingMarkerAge = Summarize(std::move(pendingAge));

    // Both series are already QPC-ordered, so one forward cursor associates them.
    struct Bucket { std::vector<uint64_t> pacerWait, presentToDisplay, callbackToDisplay, gpuStartDelay, gpuDuration; };
    Bucket application, generated;
    size_t cursor = 0;
    for (const auto& display : displays) {
        while (cursor + 1 < frames.size() &&
               std::abs(frames[cursor + 1].presentBegin - display.presentStart) <
                   std::abs(frames[cursor].presentBegin - display.presentStart))
            ++cursor;
        if (cursor >= frames.size()) break;
        const auto& frame = frames[cursor];
        if (std::abs(frame.presentBegin - display.presentStart) > kDisplayAssociationToleranceUs) continue;
        if (display.screen < frame.callbackEnd) continue;
        ++result.displayPairsMatched;
        auto& bucket = frame.generated ? generated : application;
        bucket.pacerWait.push_back(static_cast<uint64_t>(frame.presentBegin - frame.callbackEnd));
        bucket.presentToDisplay.push_back(static_cast<uint64_t>(display.screen - display.presentStart));
        bucket.callbackToDisplay.push_back(static_cast<uint64_t>(display.screen - frame.callbackEnd));
    }
    // GPU spans carry the callback end they belong to, so they need no join.
    for (const auto& event : gpuSpans) {
        auto& bucket = event.flags ? generated : application;
        bucket.gpuStartDelay.push_back(event.a - event.c);
        bucket.gpuDuration.push_back(event.b - event.a);
    }
    result.application = {Summarize(std::move(application.pacerWait)),
                          Summarize(std::move(application.presentToDisplay)),
                          Summarize(std::move(application.callbackToDisplay)),
                          Summarize(std::move(application.gpuStartDelay)),
                          Summarize(std::move(application.gpuDuration))};
    result.generated = {Summarize(std::move(generated.pacerWait)),
                        Summarize(std::move(generated.presentToDisplay)),
                        Summarize(std::move(generated.callbackToDisplay)),
                        Summarize(std::move(generated.gpuStartDelay)),
                        Summarize(std::move(generated.gpuDuration))};
    return result;
}
}  // namespace ce::pacing_trace
