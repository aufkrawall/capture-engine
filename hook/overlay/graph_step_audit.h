#pragma once

// How the shared graph cursor actually moved, per overlay route, against the movement the elapsed time asks for.
//
// The cursor runs on the game's draw-time clock (graph_scroll_policy.h), but clamping to the newest published sample, a
// resync or a route change can still make one draw hold and the next catch up - exactly what a juddering graph looks
// like. Application draws (the proxy prework, a native Present) and distinct runtime outputs (the final-batch route)
// run on different clocks, so they are judged apart: a 10 s summary that mixed them hid a failing menu interval
// (GTA V Enhanced, session gtajudder). Bounded: counters only, reset by the caller's summary.

#include <cmath>
#include <cstdint>
#include <cstdio>

#include "hook/overlay/graph_scroll_policy.h"

class GraphStepAudit {
public:
    struct Route {
        uint64_t draws = 0;
        uint64_t judged = 0;  // draws with a previous draw of this route to compare with
        uint64_t holds = 0;   // under a quarter slot although at least half a slot of time elapsed
        uint64_t jumps = 0;   // three quarters of a slot or more beyond the elapsed time
        double residualSum = 0.0;
        double maxStep = 0.0;
    };

    // One prepared graph frame. Expected movement: one slot per draw under presentation timing; under display timing
    // the draw interval in sample slots for application draws, one display interval for each new runtime output and
    // nothing for a repeat of the same output.
    void Observe(GraphOutputKind kind, uint64_t outputId, double position, int64_t drawUs, double samplePeriodUs,
                 double displayPeriodUs, bool presentationTimed) {
        const bool distinct = kind == GraphOutputKind::kDistinctOutput;
        Route& route = distinct ? outputRoute_ : applicationRoute_;
        Last& last = distinct ? output_ : application_;
        ++route.draws;
        double expected = -1.0;
        if (presentationTimed)
            expected = 1.0;
        else if (samplePeriodUs > 0.0 && distinct)
            expected = outputId == last.outputId ? 0.0 : displayPeriodUs / samplePeriodUs;
        else if (samplePeriodUs > 0.0 && last.valid && drawUs > last.drawUs)
            expected = static_cast<double>(drawUs - last.drawUs) / samplePeriodUs;
        if (last.valid && expected >= 0.0) {
            const double step = position - last.position;
            ++route.judged;
            route.residualSum += std::fabs(step - expected);
            route.maxStep = step > route.maxStep ? step : route.maxStep;
            if (expected >= 0.5 && step < 0.25)
                ++route.holds;
            else if (step >= expected + 0.75)
                ++route.jumps;
        }
        last.valid = true;
        last.position = position;
        last.drawUs = drawUs;
        last.outputId = outputId;
    }

    // A new history or cursor epoch: positions before it are not comparable.
    void Restart() {
        application_.valid = false;
        output_.valid = false;
    }

    const Route& application() const { return applicationRoute_; }
    const Route& output() const { return outputRoute_; }

    // "app=draws/holds/jumps/meanResidual/maxStep out=..." for the summary line.
    void Format(char* buffer, size_t size) const {
        std::snprintf(buffer, size, "app=%llu/%llu/%llu/%.2f/%.2f out=%llu/%llu/%llu/%.2f/%.2f",
                      static_cast<unsigned long long>(applicationRoute_.draws),
                      static_cast<unsigned long long>(applicationRoute_.holds),
                      static_cast<unsigned long long>(applicationRoute_.jumps), MeanResidual(applicationRoute_),
                      applicationRoute_.maxStep, static_cast<unsigned long long>(outputRoute_.draws),
                      static_cast<unsigned long long>(outputRoute_.holds),
                      static_cast<unsigned long long>(outputRoute_.jumps), MeanResidual(outputRoute_),
                      outputRoute_.maxStep);
    }

    void ResetCounters() {
        applicationRoute_ = {};
        outputRoute_ = {};
    }

private:
    struct Last {
        bool valid = false;
        double position = 0.0;
        int64_t drawUs = 0;
        uint64_t outputId = 0;
    };

    static double MeanResidual(const Route& route) {
        return route.judged ? route.residualSum / static_cast<double>(route.judged) : 0.0;
    }
    Route applicationRoute_;
    Route outputRoute_;
    Last application_;
    Last output_;
};
