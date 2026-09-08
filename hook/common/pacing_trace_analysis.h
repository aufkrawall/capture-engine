#pragma once
#include "pacing_trace.h"
#include <algorithm>
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

struct TraceAnalysis {
    int64_t windowUs = 0;
    uint64_t epoch = 0;
    TraceMetric proxyPrework, proxyRuntime, detour, forwarding, callback;
    TraceMetric completedMarkerAge, pendingMarkerAge;
    uint64_t matchedPresents = 0, unmatchedBegins = 0, unmatchedEnds = 0, invalidPresents = 0;
    uint64_t unmatchedMarkers = 0, latestComplete = 0, latestPending = 0;
};

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
    for (const auto& event : events) {
        if (event.epoch != result.epoch) continue;
        // Retain preceding marker records to resolve observations near the window edge.
        if (event.kind == Kind::Marker)
            markers[{event.c, event.b, event.a}] = event.timeUs;
        if (event.timeUs < start) continue;
        if (event.kind == Kind::CallbackEnd) callback.push_back(event.a);
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
    return result;
}
}  // namespace ce::pacing_trace
