#pragma once

#include <windows.h>
#include <cstdint>
#include <cstddef>

namespace ce::fg_session {

enum class FGBoundary : uint8_t {
    kUnknown = 0,
    kFFX = 1,
    kStreamline = 2,
    kDXGIAndOverlay = 3,
};

struct FGTransitionRecord {
    uint64_t sequence = 0;
    ULONGLONG timestampMs = 0;
    uint32_t sessionEpoch = 0;
    uint32_t runtimeEpoch = 0;
    uint32_t swapchainEpoch = 0;
    uint32_t queueEpoch = 0;
    FGBoundary boundary = FGBoundary::kUnknown;
    uint32_t status = 0;
    void* ptrA = nullptr;
    void* ptrB = nullptr;
    char details[96] = {};
};

void RecordTransitionEvent(FGBoundary boundary, const char* name, uint32_t status, void* ptrA = nullptr,
                           void* ptrB = nullptr, const char* details = nullptr);

// FFX boundary
void RecordFFXContextCreate(void* context, uint32_t effectId, uint32_t status, bool isSwapchain);
void RecordFFXContextDestroy(void* context, uint32_t effectId, uint32_t status, bool isSwapchain);
void RecordFFXConfigure(void* context, bool enabled, void* originalCallback, void* bridgeKey, uint32_t status);
void RecordFFXCallbackArrival(void* userCtx, uint64_t frameId, bool isGenerated, bool bridgeValid,
                              bool lifetimeChanged);

// Streamline boundary
void RecordStreamlineDLSSGOptions(bool enabled, uint32_t status);
void RecordStreamlineFirstGeneratedOutput(uint64_t frameId, void* swapchain);
void RecordStreamlineReflexOptions(uint32_t mode, uint32_t sleepResult);
void RecordStreamlineVersions(const char* interposerVersion, const char* pluginVersion);

// DXGI and Overlay boundary
void RecordSwapchainTransition(const char* action, void* swapchain, void* device, void* queue, uint32_t status);
void RecordPresentRouteTransition(const char* caller, const char* route, void* swapchain, void* queue);
void RecordRouteCoverageBoundary(const char* oldRoute, const char* newRoute, bool covered, uint64_t presentIndex);

// Snapshot and write-to-disk
size_t CopyRecentTransitionHistory(FGTransitionRecord* outRecords, size_t maxRecords);
bool WriteTransitionHistoryFile(const char* sessionDir, const char* triggerReason, DWORD exitCode = 0);
void ResetTransitionHistoryForTesting();

}  // namespace ce::fg_session
