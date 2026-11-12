#pragma once

// How far the frame-time graph moved between two presented outputs, measured from their pixels alone
// (output_pixel_probe.h records each output's graph line height per panel column). The plotted values scroll left, so
// the current output's column x shows what the previous output showed at column x + shift. Independent of CE's own
// cursor: a graph that holds on one output and catches up on the next shows exactly that here.

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace ce::flow::graph_motion {

constexpr uint16_t kNoColumn = 0xFFFF;

// Beyond this mean height difference no shift explains the change: the graph was rescaled or redrawn from another
// history, which says nothing about its horizontal motion.
constexpr double kMaxResidualPixels = 0.3;

struct Shift {
    bool measured = false;  // false: no distinctive line shape in common, or no consistent match
    bool rescaled = false;  // a shape in common, but no shift reproduces it (kMaxResidualPixels)
    double pixels = 0.0;
    double residual = 0.0;  // mean height difference at the best shift, in pixels
};

// Profiles hold heights in quarter pixels (kNoColumn where no line is drawn).
inline Shift EstimateShift(const uint16_t* previous, const uint16_t* current, size_t columns, int maxShift) {
    Shift result;
    if (!previous || !current || columns < 64 || maxShift < 1)
        return result;
    // A flat line matches itself at every shift; only a shape (a frame-time spike) pins the motion down.
    uint16_t low = kNoColumn;
    uint16_t high = 0;
    for (size_t x = 0; x < columns; ++x) {
        if (current[x] == kNoColumn)
            continue;
        low = (std::min)(low, current[x]);
        high = (std::max)(high, current[x]);
    }
    if (low == kNoColumn || high - low < 16)  // under four pixels of relief
        return result;
    std::vector<double> errors(static_cast<size_t>(maxShift) + 1, -1.0);
    for (int shift = 0; shift <= maxShift; ++shift) {
        double sum = 0.0;
        size_t matched = 0;
        for (size_t x = 0; x + static_cast<size_t>(shift) < columns; ++x) {
            const uint16_t now = current[x];
            const uint16_t before = previous[x + static_cast<size_t>(shift)];
            if (now == kNoColumn || before == kNoColumn)
                continue;
            sum += std::abs(static_cast<double>(now) - static_cast<double>(before));
            ++matched;
        }
        if (matched >= columns / 2)
            errors[static_cast<size_t>(shift)] = sum / static_cast<double>(matched);
    }
    int best = -1;
    for (int shift = 0; shift <= maxShift; ++shift) {
        const double error = errors[static_cast<size_t>(shift)];
        if (error >= 0.0 && (best < 0 || error < errors[static_cast<size_t>(best)]))
            best = shift;
    }
    if (best < 0)
        return result;
    double refined = best;
    if (best > 0 && best < maxShift && errors[best - 1] >= 0.0 && errors[best + 1] >= 0.0) {
        // Sub-pixel motion: the minimum of the parabola through the best shift and its neighbours.
        const double a = errors[best - 1];
        const double b = errors[best];
        const double c = errors[best + 1];
        const double curvature = a - 2.0 * b + c;
        if (curvature > 0.0)
            refined += (std::max)(-0.5, (std::min)(0.5, 0.5 * (a - c) / curvature));
    }
    result.residual = errors[static_cast<size_t>(best)] / 4.0;
    result.rescaled = result.residual > kMaxResidualPixels;
    result.measured = !result.rescaled;
    result.pixels = refined;
    return result;
}

struct Verdict {
    size_t pairs = 0;
    size_t measured = 0;
    size_t rescaled = 0;
    size_t backwards = 0;
    size_t holds = 0;
    size_t jumps = 0;
    double medianPixels = 0.0;
    std::string firstViolation;
    std::vector<size_t> violations;  // step indices, the first 16
};

// Uniform motion: each output steps by the same distance as the outputs around it, never backwards, never holding
// and never catching up. `steps[i]` is the motion from output i to i+1; `expectedRatio[i]` scales the expected step
// (the elapsed output time relative to the typical one; 1.0 when every output represents the same time).
inline Verdict JudgeSteps(const std::vector<Shift>& steps, const std::vector<double>& expectedRatio,
                          double tolerance) {
    Verdict verdict;
    verdict.pairs = steps.size();
    std::vector<double> normalized;
    for (size_t i = 0; i < steps.size(); ++i) {
        const double ratio = i < expectedRatio.size() && expectedRatio[i] > 0.0 ? expectedRatio[i] : 1.0;
        if (steps[i].measured)
            normalized.push_back(steps[i].pixels / ratio);
        verdict.rescaled += steps[i].rescaled ? 1 : 0;
    }
    verdict.measured = normalized.size();
    if (normalized.empty())
        return verdict;
    std::vector<double> sorted = normalized;
    std::nth_element(sorted.begin(), sorted.begin() + sorted.size() / 2, sorted.end());
    verdict.medianPixels = sorted[sorted.size() / 2];
    const double median = verdict.medianPixels;
    for (size_t i = 0; i < steps.size(); ++i) {
        if (!steps[i].measured)
            continue;
        const double ratio = i < expectedRatio.size() && expectedRatio[i] > 0.0 ? expectedRatio[i] : 1.0;
        const double value = steps[i].pixels / ratio;
        const char* kind = nullptr;
        if (steps[i].pixels < -0.25) {
            ++verdict.backwards;
            kind = "backwards";
        } else if (value < median * (1.0 - tolerance)) {
            ++verdict.holds;
            kind = "hold";
        } else if (value > median * (1.0 + tolerance)) {
            ++verdict.jumps;
            kind = "jump";
        }
        if (kind && verdict.violations.size() < 16)
            verdict.violations.push_back(i);
        if (kind && verdict.firstViolation.empty()) {
            char text[160];
            std::snprintf(text, sizeof(text), "%s at step %zu: %.2f px (expected %.2f px, residual %.2f px)", kind, i,
                          steps[i].pixels, median * ratio, steps[i].residual);
            verdict.firstViolation = text;
        }
    }
    return verdict;
}

}  // namespace ce::flow::graph_motion
