#include "overlay_metrics_publisher.h"

#include "dx12_overlay_policy.h"
#include "hook_common.h"
#include "performance_metrics.h"

namespace ce::overlay_metrics {
namespace {

struct PublishedState {
    bool valid = false;
    bool effectiveFGActive = false;
    fg_runtime::RuntimeMode runtimeMode = fg_runtime::RuntimeMode::kOff;
    int publishedType = 0;
    int publishedMultiplier = 1;
};

PublishedState& LastPublishedState() {
    static PublishedState state;
    return state;
}

}  // namespace

void PublishOverlayFGMetrics(PerformanceMetrics* metrics, const PublicationInput& input) {
    if (!metrics) {
        return;
    }

    const int publishedType =
        ce::dx12_overlay_policy::ResolveOverlayFGMetricType(input.effectiveFGActive, input.runtimeMode);
    const bool publishedActive = publishedType != 0;
    const int publishedMultiplier = publishedActive ? (input.multiplier >= 2 ? input.multiplier : 2) : 1;
    const float publishedOutputFPS = publishedActive ? input.outputFPS : 0.0f;
    const float publishedBaseFPS = publishedActive ? input.baseFPS : 0.0f;

    if (input.effectiveFGActive && publishedType == 0) {
        HookLogImportant("FG publication invariant: source=%s active=1 runtime=%s published_type=0 multiplier=%d",
                         input.publicationSource ? input.publicationSource : "unknown",
                         ce::fg_runtime::GetRuntimeModeName(input.runtimeMode), input.multiplier);
    }

    metrics->SetFGMetrics(publishedOutputFPS, publishedBaseFPS, publishedMultiplier, publishedType);

    auto& last = LastPublishedState();
    if (!last.valid || last.effectiveFGActive != publishedActive || last.runtimeMode != input.runtimeMode ||
        last.publishedType != publishedType || last.publishedMultiplier != publishedMultiplier) {
        HookLogImportant(
            "FG publication: source=%s runtime=%s active=%d published_type=%d published_multiplier=%d base_fps=%.2f "
            "output_fps=%.2f",
            input.publicationSource ? input.publicationSource : "unknown",
            ce::fg_runtime::GetRuntimeModeName(input.runtimeMode), publishedActive ? 1 : 0, publishedType,
            publishedMultiplier, publishedBaseFPS, publishedOutputFPS);
        last.valid = true;
        last.effectiveFGActive = publishedActive;
        last.runtimeMode = input.runtimeMode;
        last.publishedType = publishedType;
        last.publishedMultiplier = publishedMultiplier;
    }
}

}  // namespace ce::overlay_metrics
