#include "overlay_metrics_publisher.h"

#include <atomic>

#include "hook/fg/fg_detection.h"
#include "hook/runtime/hook_common.h"
#include "overlay_fg_metric_policy.h"
#include "overlay_fg_publication_log.h"
#include "hook/metrics/performance_metrics.h"

namespace ce::overlay_metrics {

void PublishOverlayFGMetrics(PerformanceMetrics* metrics, const PublicationInput& input) {
    if (!metrics) {
        return;
    }

    // DLSS-G enabled and generating nothing, by the two proofs the limiter already paces on (menus, loading screens). The
    // overlay read "DLSS FG 2x" through it with base and output both at 144 (20261104_221303), which XeFG stopped doing in
    // the same menu. Only what the overlay shows changes: the runtime mode and its routes stay DLSS FG.
    const bool dlssGIdle = input.effectiveFGActive && input.runtimeMode == fg_runtime::RuntimeMode::kDLSSFG &&
                           g_FGCompat.IsDLSSGGenerationIdle();
    static std::atomic<bool> s_dlssGIdleLogged{false};
    if (s_dlssGIdleLogged.exchange(dlssGIdle, std::memory_order_relaxed) != dlssGIdle) {
        HookLogImportant("FG: DLSS-G is enabled and %s", dlssGIdle ? "generates nothing - status reads Off"
                                                                   : "no longer idle - status follows the runtime");
    }
    const int publishedType =
        dlssGIdle ? 0 : static_cast<int>(ResolveFGMetricType(input.effectiveFGActive, input.runtimeMode));
    const bool publishedActive = publishedType != 0;
    const int publishedMultiplier = publishedActive ? (input.multiplier >= 2 ? input.multiplier : 2) : 1;
    const float publishedOutputFPS = publishedActive ? input.outputFPS : 0.0f;
    const float publishedBaseFPS = publishedActive ? input.baseFPS : 0.0f;
    const fg_runtime::RuntimeMode loggedRuntimeMode =
        publishedActive ? input.runtimeMode : fg_runtime::RuntimeMode::kOff;

    if (input.effectiveFGActive && publishedType == 0 && !dlssGIdle) {
        HookLogImportant("FG publication invariant: source=%s active=1 runtime=%s published_type=0 multiplier=%d",
                         input.publicationSource ? input.publicationSource : "unknown",
                         ce::fg_runtime::GetRuntimeModeName(input.runtimeMode), input.multiplier);
    }

    metrics->SetFGMetrics(publishedOutputFPS, publishedBaseFPS, publishedMultiplier, publishedType);

    static FGPublicationLogGate logGate;
    if (const auto verdict = logGate.Observe(publishedActive, static_cast<int>(loggedRuntimeMode), publishedType,
                                             publishedMultiplier);
        verdict.log) {
        HookLogImportant(
            "FG publication: source=%s runtime=%s active=%d published_type=%d published_multiplier=%d base_fps=%.2f "
            "output_fps=%.2f factorChanges=%u",
            input.publicationSource ? input.publicationSource : "unknown",
            ce::fg_runtime::GetRuntimeModeName(loggedRuntimeMode), publishedActive ? 1 : 0, publishedType,
            publishedMultiplier, publishedBaseFPS, publishedOutputFPS, verdict.factorChanges);
    }
}

void PublishDetectedOverlayFGMetrics(PerformanceMetrics* metrics, const char* publicationSource) {
    PublishOverlayFGMetrics(metrics,
                            {
                                .effectiveFGActive = g_FGCompat.IsFGActive(),
                                .runtimeMode = g_FGCompat.GetRuntimeMode(),
                                .outputFPS = g_FGCompat.GetOutputFPS(),
                                .baseFPS = g_FGCompat.GetBaseFPS(),
                                .multiplier = g_FGCompat.GetFGMultiplier(),
                                .publicationSource = publicationSource,
                            });
}

}  // namespace ce::overlay_metrics
