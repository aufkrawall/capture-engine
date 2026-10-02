#pragma once

#include <cstdint>
#include <vector>

// Which process composes the desktop a composed present is drawn on. Every
// interactive session runs its own dwm.exe, so the one sharing the presenting
// process's session is the one whose flips show its frames.
struct CompositorCandidate {
    uint32_t processId = 0;
    uint32_t sessionId = 0;
    bool sessionKnown = false;
};

// The candidate in the target's session. Without a session match, a lone
// candidate is still unambiguous; several unmatched ones are not, and the
// caller then measures nothing rather than follow the wrong desktop.
inline uint32_t SelectCompositorProcess(const std::vector<CompositorCandidate>& candidates, uint32_t targetSession,
                                        bool targetSessionKnown) {
    if (targetSessionKnown) {
        for (const CompositorCandidate& candidate : candidates) {
            if (candidate.sessionKnown && candidate.sessionId == targetSession)
                return candidate.processId;
        }
    }
    return candidates.size() == 1 ? candidates.front().processId : 0;
}

// Windows lookup: the dwm.exe composing processId's session, or 0.
uint32_t FindCompositorProcessId(uint32_t processId);
