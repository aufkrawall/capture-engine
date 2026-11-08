#pragma once

#include <windows.h>
#include <cstdint>

namespace ce::fg_session {

struct FGSessionSnapshot;
struct FGActionPlan;

// The trace stores state changes and callback arrival edges. Its exit writer
// reads only published data and skips any slot interrupted during publication.
void RecordPublishedFGState(const FGSessionSnapshot& snapshot, const FGActionPlan& plan);
void RecordFFXContextCreate(void* context, uint32_t effectId, uint32_t status, bool isSwapchain);
void RecordFFXContextDestroy(void* context, uint32_t effectId, uint32_t status, bool isSwapchain);
void RecordFFXConfigure(void* context, bool enabled, void* originalCallback, void* bridgeKey, uint32_t status);
void RecordFFXCallbackArrival(void* userCtx, uint64_t frameId, bool isGenerated, bool bridgeValid);
void RecordStreamlineDLSSGOptions(uint32_t viewport, bool enabled, uint32_t status);
void RecordStreamlineReflexOptions(uint32_t mode, uint32_t status);
void RecordStreamlineReflexSleepFailure(uint32_t status);
void RecordSwapchainTransition(const char* action, void* swapchain, void* device, void* queue, uint32_t status);

bool WriteTransitionHistoryFile(const char* sessionDir, const char* triggerReason, DWORD exitCode);

}  // namespace ce::fg_session
