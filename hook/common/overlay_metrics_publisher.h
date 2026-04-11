#pragma once

#include "fg_runtime_state.h"
#include "performance_metrics.h"

namespace ce::overlay_metrics {

struct PublicationInput {
    bool effectiveFGActive = false;
    fg_runtime::RuntimeMode runtimeMode = fg_runtime::RuntimeMode::kOff;
    float outputFPS = 0.0f;
    float baseFPS = 0.0f;
    int multiplier = 1;
    const char* publicationSource = nullptr;
};

void PublishOverlayFGMetrics(::PerformanceMetrics* metrics, const PublicationInput& input);

}  // namespace ce::overlay_metrics
