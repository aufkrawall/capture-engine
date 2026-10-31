#include "overlay_adapter_internal.h"
#include "hook/runtime/hook_clock.h"

// The frame-time graph changes every frame; the rest of the overlay only at its text update interval.
// It used to rebuild every row's text geometry each frame just to move the graph: 12.9 us of the
// overlay's CPU time per frame against 0.3 us for a cached frame (OverlayFrameCostTest), at 630 FPS
// in Mirror's Edge's menu (20261018_151515). Now the graph is drawn last and a graph-only frame
// rewinds to it and redraws just the line.

const char* OverlayAdapter::FrameTimingLabel(PerformanceMetrics::FrameTimingState state) {
    using State = PerformanceMetrics::FrameTimingState;
    switch (state) {
        case State::DisplayStarting:
            return "Display starting";
        case State::DisplayActive:
            return "Display";
        case State::Composed:
            return "Present (composed)";
        case State::Stale:
            return "Present (display stale)";
        case State::Unavailable:
            return "Present (no display data)";
        case State::Presentation:
            return "Present";
    }
    return "Present";
}

void OverlayAdapter::PrepareGraphFrame(GraphFrame& graph) {
    const auto state = metrics->GetFrameTimingState();
    const uint64_t identity = metrics->GetHistoryIdentity();
    const bool changed = identity != graphHistoryIdentity || state != graphTimingState;
    if (changed) {
        graphScroll.Reset();
        graphHistoryIdentity = identity;
        graphTimingState = state;
        cachedMaxFrameTime = 0.0f;
        cachedAvgFrameTimeForColor = 0.0f;
    }
    graph.waiting = state == PerformanceMetrics::FrameTimingState::DisplayStarting;
    // One slot per drawn frame, gently locked to the sample stream, so a
    // burst of display-change samples does not step the graph.
    if (!graph.waiting) {
        const double cursor = graphScroll.Advance(metrics->GetSampleCount());
        const double cursorSlot = std::floor(cursor);
        graph.scrollOffset = static_cast<float>(cursor - cursorSlot);
        metrics->GetHistoryEndingAt(static_cast<uint64_t>(cursorSlot) + 1, graph.data, kGraphPoints);
    }

    // Max frame time display values – recomputed at most once every 2 seconds so
    // the label doesn't flicker on every frame.
    graph.labelsChanged = false;
    const DWORD ftNow = ce::hook_clock::TickCount();
    if (changed || (ftNow - lastMaxFrameTimeUpdateTime) >= 2000) {
        lastMaxFrameTimeUpdateTime = ftNow;
        int samplesPerSecond = (cachedFPS > 0) ? (int)cachedFPS : 60;
        int samplesFor2Seconds = (std::min)(kGraphPoints, samplesPerSecond * 2);
        int startIdx = kGraphPoints - samplesFor2Seconds;

        float newMax = 0.0f, sum = 0.0f;
        int count = 0;
        for (int i = startIdx; i < kGraphPoints; i++) {
            float val = graph.data[i];
            if (val > newMax)
                newMax = val;
            sum += val;
            count++;
        }
        cachedMaxFrameTime = newMax;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        cachedAvgFrameTimeForColor = (count > 0) ? sum / count : 0.0f;
        graph.labelsChanged = true;
    }

    // Calculate graph scaling with a dynamic minimum for better vertical
    // centering: instead of always starting at 0ms, leave ~15% padding below
    // the lowest point.
    float minVal = FLT_MAX;
    for (int i = 0; i < kGraphPoints; i++) {
        if (graph.data[i] > 0.001f && graph.data[i] < minVal)
            minVal = graph.data[i];
    }
    if (minVal == FLT_MAX)
        minVal = 0.0f;

    // Smarter scaling to avoid over-dramatizing frame time variations.
    // Instead of just 10% above peak, use a scale that provides meaningful
    // context (e.g., 30fps vs 60fps threshold should be visible).
    float avgVal = 0;
    int avgCount = 0;
    for (int i = 0; i < kGraphPoints; i++) {
        if (graph.data[i] > 0.001f) {
            avgVal += graph.data[i];
            avgCount++;
        }
    }
    if (avgCount > 0)
        avgVal /= (float)avgCount;

    // Scale based on average with generous headroom:
    // - At least 50% above average (not peak) to avoid zooming in on noise
    // - At least 2x the minimum (so 8ms avg shows 0-16ms, not 0-12ms)
    // - Minimum 33ms range to show 30fps threshold
    float scaleBase = (std::max)(avgVal, minVal * 2.0f);
    float rawMax = (std::max)(scaleBase * 1.5f, 33.0f);

    // Dynamic minimum: leave ~15% padding below lowest point, but never go negative
    // This makes the graph line appear more vertically centered
    float range = rawMax - minVal;
    graph.minVal = (std::max)(0.0f, minVal - range * 0.15f);
    graph.maxVal = rawMax;
    if (graph.waiting) {
        std::snprintf(graph.label, sizeof(graph.label), "%s", FrameTimingLabel(state));
        std::snprintf(graph.peakLabel, sizeof(graph.peakLabel), "-- ms");
    } else {
        std::snprintf(graph.label, sizeof(graph.label), "%.0f ms | %s", graph.maxVal, FrameTimingLabel(state));
        std::snprintf(graph.peakLabel, sizeof(graph.peakLabel), "%.1f ms", cachedMaxFrameTime);
    }
}

void OverlayAdapter::DrawGraphTail(const GraphFrame& graph) {
    if (graph.waiting)
        return;
    renderer->DrawFrameTimeGraph(graphTail.x, graphTail.y, graphTail.width, graphTail.height, graph.data,
                                 kGraphPoints, graph.minVal, graph.maxVal, graphTail.color, graph.scrollOffset);
}

bool OverlayAdapter::RenderGraphOnlyFrame(const GraphFrame& graph) {
    if (!graphTail.valid || graph.labelsChanged)
        return false;
    // The scale marker text is drawn with the rows; a new value takes a full build.
    if (std::strcmp(graph.label, graphTail.scaleLabel) != 0)
        return false;
    if (!renderer->RewindToTail())
        return false;
    DrawGraphTail(graph);
    renderer->EndFrame();
    return true;
}
