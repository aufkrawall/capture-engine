#pragma once

namespace ce::encoder_iteration {

// A terminal output failure must win over a phase's next-iteration request.
// Queued frames and CFR debt cannot keep an encoder with no usable output alive.
template <typename Steps, typename RunStep, typename IsTerminal, typename NextIteration>
bool RunSteps(const Steps& steps, RunStep&& runStep, IsTerminal&& isTerminal,
              NextIteration&& nextIteration) {
    for (const auto& step : steps) {
        runStep(step);
        if (isTerminal()) {
            return false;
        }
        if (nextIteration()) {
            return true;
        }
    }
    return true;
}

}  // namespace ce::encoder_iteration
