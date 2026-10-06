#include "ngx_fg_observation.h"

#include "nvngx_hook_internal.h"
#include "common/logging/log_meter.h"
#include "hook/streamline/streamline_hook.h"

namespace ce::ngx {
namespace {

enum class ObservationKind { kCreation, kEvaluation };

void PublishObservation(int multiplier, ObservationKind kind, const char* source, int featureId = 0) {
    if (multiplier <= 0)
        return;
    const bool heldOff = StreamlineHook::HoldsExplicitDLSSGOff();
    static ce::log_meter::ChangeGate creationGate;
    static ce::log_meter::ChangeGate evaluationGate;
    auto& gate = kind == ObservationKind::kCreation ? creationGate : evaluationGate;
    const auto verdict = gate.Observe(heldOff ? 1 : 0);
    if (heldOff) {
        if (verdict) {
            HookLogImportant(
                "NVNGX FG: %s observation (%dx feature=0x%X) while Streamline holds explicit DLSS-G OFF - "
                "preserving accepted OFF",
                source, multiplier, featureId);
        }
        return;
    }
    if (verdict && verdict.suppressed > 0) {
        HookLogImportant("NVNGX FG: %s observations admitted after held OFF (+%llu held-OFF repeats)", source,
                         static_cast<unsigned long long>(verdict.suppressed));
    }

    g_FGCompat.SetDLSSFGMultiplier(multiplier);
    g_FGCompat.SetDLSSFGActive(true);
    if (g_IPC && g_IPC->GetSharedMem()) {
        auto& state = g_IPC->GetSharedMem()->dlssState;
        state.PublishFGState(GetCurrentProcessId(), true, multiplier);
        if (kind == ObservationKind::kCreation && g_IPC->GetSharedMem()->GetDebugLogging()) {
            NVNGXLog("NVNGX FG: accepted feature creation ID 0x%X (%dx multiplier)", featureId, multiplier);
        }
    }
}

}  // namespace

void ObserveFGCreation(const FGCreationObservation& observation) {
    const bool multiFrame = observation.featureId == nvngx_hook_NVSDK_NGX_Feature_MultiFrameGeneration;
    if (!multiFrame && observation.featureId != nvngx_hook_NVSDK_NGX_Feature_FrameGeneration &&
        observation.featureId != nvngx_hook_NVSDK_NGX_Feature_FrameGeneration_11) {
        return;
    }
    const int resolved = ce::ngx_lifecycle::ResolveNVNGXFrameGenerationMultiplier(observation.configuredMultiplier,
                                                                                  observation.parameterMultiplier);
    // Legacy creation can lack a factor while Streamline already accepted 3x/4x. Keep that evidence;
    // an explicit modern factor and MFG feature creation retain their existing precedence.
    const int latched = g_FGCompat.GetFGMultiplier();
    const int multiplier = multiFrame || resolved >= 3 || latched < 2 ? resolved : latched;
    PublishObservation(multiplier, ObservationKind::kCreation, "CreateFeature", observation.featureId);
}

void ObserveFGEvaluation(int evaluatedMultiplier, const char* api) {
    PublishObservation(evaluatedMultiplier, ObservationKind::kEvaluation, api);
}

}  // namespace ce::ngx
