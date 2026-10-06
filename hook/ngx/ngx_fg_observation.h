#pragma once

namespace ce::ngx {

struct FGCreationObservation {
    int featureId = 0;
    int configuredMultiplier = 0;
    int parameterMultiplier = 0;
};

// Called after a successful SDK operation. The publication boundary owns accepted-OFF precedence,
// creation defaults/latched factors, and the matching compatibility/shared status updates.
void ObserveFGCreation(const FGCreationObservation& observation);
void ObserveFGEvaluation(int evaluatedMultiplier, const char* api);

}  // namespace ce::ngx
