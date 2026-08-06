#pragma once

struct FGSessionSnapshot;
struct FGActionPlan;

namespace ce::fg_session {

// Logging helpers shared between the session-state core and the log unit.
const char* BoolName(bool value);
const char* SafeString(const char* value);
void LogSnapshotLine(const FGSessionSnapshot& snapshot);
void LogPlanLine(const FGSessionSnapshot& snapshot, const FGActionPlan& plan);
void LogPlanDiffIfNeeded(const FGSessionSnapshot& previousSnapshot, const FGActionPlan& previousPlan,
                         const FGSessionSnapshot& currentSnapshot, const FGActionPlan& currentPlan);
void LogLegacyDecisionLine(const FGSessionSnapshot& snapshot, const FGActionPlan& plan);
void LogTransitionIfNeeded(const FGSessionSnapshot& previousSnapshot, const FGActionPlan& previousPlan,
                           const FGSessionSnapshot& currentSnapshot, const FGActionPlan& currentPlan,
                           const char* trigger);

}  // namespace ce::fg_session
