#pragma once

#include <span>

#include "process_audio_session_monitor.h"
#include "process_tree_selection.h"

namespace ce::process_loopback {

struct CaptureSessionBinding {
    uint32_t processId = 0;
    uint64_t generation = 0;
    bool qualified = false;
    bool observedTargetSession = false;
};

enum class SessionActivityAction { Continue, RebindSession, ReselectProcess };

struct SessionActivityDecision {
    SessionActivityAction action = SessionActivityAction::Continue;
    uint32_t sessionProcessId = 0;
    ProcessNameSelection selection;
};

// A new render session can invalidate a healthy binding too: include-tree
// loopback starts mixing newly audible descendants, including excluded ones.
// Resolve ownership before the pre-session qualification/recovery policy.
inline SessionActivityDecision ResolveSessionActivity(
    const std::vector<ProcessTreeEntry>& processes, std::span<const AudioSessionCreation> creations,
    const std::string& processSelector, const std::vector<WhitelistEntry>& exclusions,
    const CaptureSessionBinding& binding) {
    if (creations.empty())
        return {};
    const bool rootObserved = std::any_of(processes.begin(), processes.end(), [&](const ProcessTreeEntry& process) {
        return process.processId == binding.processId;
    });
    // A missing/failed snapshot is not proof of an ownership change. The
    // capture loop retains its existing consecutive-miss process-exit policy.
    if (!processSelector.empty() && !exclusions.empty() && binding.processId != 0 && rootObserved) {
        const auto selection = SelectProcessTreeRootByName(processes, processSelector, exclusions, binding.processId);
        if (selection.selectedProcessId != binding.processId)
            return {SessionActivityAction::ReselectProcess, 0, selection};
    }
    for (const auto& creation : creations) {
        if (ShouldRecycleCaptureForSessionCreation(processes, binding.qualified, binding.observedTargetSession,
                                                  binding.processId, creation.processId, binding.generation,
                                                  creation.generation))
            return {SessionActivityAction::RebindSession, creation.processId, {}};
    }
    return {};
}

}  // namespace ce::process_loopback
