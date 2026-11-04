#include "overlay_metrics_publisher.h"

#include "common/logging/log_meter.h"
#include "hook/fg/fg_session_state.h"
#include "hook/runtime/hook_common.h"
#include "overlay_fg_metric_policy.h"

namespace ce::overlay_metrics {
namespace {

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
            // The preferred state takes a new sequence every frame, so a sequence is not what makes this a new line
            // (118 lines in 1.4 s, 20261104_183540).
            static ce::log_meter::ChangeGate s_overrideGate;
            if (const auto verdict = s_overrideGate.Observe(
                    ce::log_meter::FieldKey(plan.publishFGActive, plan.publishRuntimeMode, preferredState.active,
                                            preferredState.runtimeMode, usePreferredState))) {
                HookLogImportant(
                    "FG publication preferred override: source=%s planner(active=%d mode=%s) "
                    "preferred(active=%d mode=%s) winner=%s planner_sequence=%llu preferred_sequence=%llu%s",
                    publicationSource ? publicationSource : "unknown", plan.publishFGActive ? 1 : 0,
                    ce::fg_runtime::GetRuntimeModeName(plan.publishRuntimeMode), preferredState.active ? 1 : 0,
                    ce::fg_runtime::GetRuntimeModeName(preferredState.runtimeMode),
                    usePreferredState ? "preferred" : "planner", static_cast<unsigned long long>(lastPlanner.sequence),
                    static_cast<unsigned long long>(preferredState.sequence),
                    ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
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
