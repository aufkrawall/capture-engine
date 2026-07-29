#include "overlay_metrics_publisher.h"

#include "fg_session_state.h"
#include "hook_common.h"
#include "overlay_fg_metric_policy.h"

namespace ce::overlay_metrics {
namespace {

struct PreferredOverrideState {
    bool valid = false;
    bool plannerFGActive = false;
    fg_runtime::RuntimeMode plannerRuntimeMode = fg_runtime::RuntimeMode::kOff;
    uint64_t plannerSequence = 0;
    bool preferredFGActive = false;
    fg_runtime::RuntimeMode preferredRuntimeMode = fg_runtime::RuntimeMode::kOff;
    uint64_t preferredSequence = 0;
    bool usingPreferredState = false;
};

PreferredOverrideState& LastPreferredOverrideState() {
    static PreferredOverrideState state;
    return state;
}

struct PlannerState {
    bool valid = false;
    bool fgActive = false;
    fg_runtime::RuntimeMode runtimeMode = fg_runtime::RuntimeMode::kOff;
    uint64_t sequence = 0;
};

PlannerState& LastPlannerState() {
    static PlannerState state;
    return state;
}

}  // namespace

void PublishOverlayFGMetrics(PerformanceMetrics* metrics, const fg_session::FGActionPlan& plan, float outputFPS,
                             float baseFPS, int multiplier, const char* publicationSource) {
    auto& lastPlanner = LastPlannerState();
    if (!lastPlanner.valid || lastPlanner.fgActive != plan.publishFGActive ||
        lastPlanner.runtimeMode != plan.publishRuntimeMode) {
        lastPlanner.valid = true;
        lastPlanner.fgActive = plan.publishFGActive;
        lastPlanner.runtimeMode = plan.publishRuntimeMode;
        lastPlanner.sequence = HookAllocateOverlayFGPublicationSequence();
    }

    PublicationInput input{
        .effectiveFGActive = plan.publishFGActive,
        .runtimeMode = plan.publishRuntimeMode,
        .outputFPS = outputFPS,
        .baseFPS = baseFPS,
        .multiplier = multiplier,
        .publicationSource = publicationSource,
    };

    PreferredOverlayFGPublicationState preferredState;
    if (HookTryGetPreferredOverlayFGPublicationState(&preferredState) && preferredState.valid) {
        const bool statesDiffer =
            preferredState.active != plan.publishFGActive || preferredState.runtimeMode != plan.publishRuntimeMode;
        const bool publishedTypesDiffer = DoPublishedFGTypesDiffer(
            plan.publishFGActive, plan.publishRuntimeMode, preferredState.active, preferredState.runtimeMode);
        const bool usePreferredState =
            statesDiffer && publishedTypesDiffer && preferredState.sequence >= lastPlanner.sequence;

        if (statesDiffer && publishedTypesDiffer) {
            auto& lastOverride = LastPreferredOverrideState();
            if (!lastOverride.valid || lastOverride.plannerFGActive != plan.publishFGActive ||
                lastOverride.plannerRuntimeMode != plan.publishRuntimeMode ||
                lastOverride.plannerSequence != lastPlanner.sequence ||
                lastOverride.preferredFGActive != preferredState.active ||
                lastOverride.preferredRuntimeMode != preferredState.runtimeMode ||
                lastOverride.preferredSequence != preferredState.sequence ||
                lastOverride.usingPreferredState != usePreferredState) {
                HookLogImportant(
                    "FG publication preferred override: source=%s planner(active=%d mode=%s) "
                    "preferred(active=%d mode=%s) winner=%s planner_sequence=%llu preferred_sequence=%llu",
                    publicationSource ? publicationSource : "unknown", plan.publishFGActive ? 1 : 0,
                    ce::fg_runtime::GetRuntimeModeName(plan.publishRuntimeMode), preferredState.active ? 1 : 0,
                    ce::fg_runtime::GetRuntimeModeName(preferredState.runtimeMode),
                    usePreferredState ? "preferred" : "planner", static_cast<unsigned long long>(lastPlanner.sequence),
                    static_cast<unsigned long long>(preferredState.sequence));
                lastOverride.valid = true;
                lastOverride.plannerFGActive = plan.publishFGActive;
                lastOverride.plannerRuntimeMode = plan.publishRuntimeMode;
                lastOverride.plannerSequence = lastPlanner.sequence;
                lastOverride.preferredFGActive = preferredState.active;
                lastOverride.preferredRuntimeMode = preferredState.runtimeMode;
                lastOverride.preferredSequence = preferredState.sequence;
                lastOverride.usingPreferredState = usePreferredState;
            }
        }

        if (usePreferredState) {
            input.effectiveFGActive = preferredState.active;
            input.runtimeMode = preferredState.runtimeMode;
        }
    }

    PublishOverlayFGMetrics(metrics, input);
}

}  // namespace ce::overlay_metrics
