#pragma once

#include <algorithm>
#include <cstdint>

// Graph time for one display-change transition: the earliest moment the panel
// could have shown it, given what the kernel reported.
//
// A synchronized present (SyncInterval >= 1) cannot tear, and no display
// starts a new frame sooner than one period of its maximum refresh rate after
// the previous one, variable refresh included. Under FSR frame generation at
// that ceiling the kernel nevertheless reports alternate flips 3.5 and 10.4 ms
// apart on a 144 Hz panel: the flip is reported when the driver takes it, not
// when it reaches the screen. Every frame is shown (FIFO drops nothing) and the
// intervals average exactly one period, so the screen itself was even.
//
// Only an interval that is physically impossible is touched, and only by moving
// that transition LATER, to the earlier of:
//   - one refresh after the previous transition, and
//   - the first vertical blank actually observed at or after the report.
// The blank bound matters when the previous transition was itself reported
// late: then the new report sits on a real blank, the bound keeps it there,
// and no delay is carried forward into later frames.
//
// What this cannot do: hide a real hitch. Intervals of a period or longer are
// never changed, a transition never moves earlier, and a correction is limited
// to what an observed blank supports. Tearing, immediate flips, unknown sync
// intervals, an unknown refresh period and missing blanks all leave the kernel
// time untouched. The published screen time is never changed either; this is
// only what the overlay graph draws.
struct RefreshBoundDecision {
    int64_t graphUs = 0;
    // Synchronized present on a display with a known period, after an anchor.
    bool eligible = false;
    // The report was physically impossible and graphUs was moved later.
    bool bounded = false;
    // The report was physically impossible but no observed blank could bound
    // it, so it was left as reported.
    bool blankMissing = false;
};

class RefreshBoundedGraphTime {
public:
    // Reporting jitter on a correctly timed flip is tens of microseconds; one
    // sixteenth of a period (434 us at 144 Hz) stays far above that and far
    // below the half-period errors this exists for.
    static int64_t SlackUs(int64_t periodUs) { return periodUs / 16; }

    // How far before a report the blank that displayed it may be timestamped:
    // the VSync and flip-completion DPCs of one blank are separate events.
    static int64_t BlankLeadUs(int64_t periodUs) { return std::min<int64_t>(500, periodUs / 8); }

    // `firstBlank(from, until)` returns the earliest observed blank on the
    // flip's display in [from, until] in microseconds, or 0.
    template <typename FirstBlank>
    RefreshBoundDecision Apply(int64_t reportedUs, bool synchronizedPresent, int64_t periodUs,
                               FirstBlank&& firstBlank) {
        RefreshBoundDecision decision;
        decision.graphUs = reportedUs;
        if (lastGraphUs_ > 0 && synchronizedPresent && periodUs > 0) {
            decision.eligible = true;
            const int64_t earliest = lastGraphUs_ + periodUs;
            if (reportedUs < earliest - SlackUs(periodUs)) {
                // A blank the previous transition already occupies cannot show
                // this one; the search ends one refresh after the later of the
                // report and that transition.
                const int64_t from = std::max(reportedUs - BlankLeadUs(periodUs), lastGraphUs_ + 1);
                const int64_t until = std::max(reportedUs, lastGraphUs_) + periodUs + SlackUs(periodUs);
                const int64_t blank = firstBlank(from, until);
                if (blank > 0) {
                    const int64_t bound = std::max(reportedUs, std::min(earliest, blank));
                    if (bound > reportedUs) {
                        decision.graphUs = bound;
                        decision.bounded = true;
                    }
                } else {
                    decision.blankMissing = true;
                }
            }
        }
        // The overlay drops a transition that does not advance, so the anchor
        // follows the latest time it has actually drawn.
        lastGraphUs_ = std::max(lastGraphUs_, decision.graphUs);
        return decision;
    }

    void Reset() { lastGraphUs_ = 0; }

private:
    int64_t lastGraphUs_ = 0;
};
