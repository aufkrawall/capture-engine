#include "fg_transition_trace.h"

#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>

#include "common/crash/crash_first_chance.h"
#include "fg_session_state.h"
#include "hook/runtime/hook_clock.h"

namespace ce::fg_session {

namespace {

struct alignas(16) HistorySlot {
    std::atomic<uint64_t> sequence{0};
    std::atomic<bool> writing{false};
    FGTransitionRecord record{};
};

constexpr size_t kHistoryCapacity = 256;
HistorySlot g_HistoryRing[kHistoryCapacity];
std::atomic<uint64_t> g_Counter{0};
std::atomic<bool> g_FirstGeneratedReported{false};
std::atomic<bool> g_FirstSleepReported{false};
std::atomic<bool> g_VersionsReported{false};

void RecordInternal(FGBoundary boundary, const char* name, uint32_t status, void* ptrA, void* ptrB,
                    const char* details) {
    const uint64_t seq = g_Counter.fetch_add(1, std::memory_order_relaxed) + 1;
    const size_t index = seq % kHistoryCapacity;
    HistorySlot& slot = g_HistoryRing[index];

    bool expectedWriting = false;
    if (!slot.writing.compare_exchange_strong(expectedWriting, true, std::memory_order_acq_rel)) {
        return;
    }

    const FGSessionSnapshot snapshot = GetLatestFGSessionSnapshot();
    slot.record.sequence = seq;
    slot.record.timestampMs = ce::hook_clock::TickCount64();
    slot.record.sessionEpoch = snapshot.sessionEpoch;
    slot.record.runtimeEpoch = snapshot.runtimeEpoch;
    slot.record.swapchainEpoch = snapshot.swapchainEpoch;
    slot.record.queueEpoch = snapshot.queueEpoch;
    slot.record.boundary = boundary;
    slot.record.status = status;
    slot.record.ptrA = ptrA;
    slot.record.ptrB = ptrB;

    if (name || details) {
        std::snprintf(slot.record.details, sizeof(slot.record.details), "%s%s%s", name ? name : "",
                      (name && details) ? ": " : "", details ? details : "");
    } else {
        slot.record.details[0] = '\0';
    }

    slot.sequence.store(seq, std::memory_order_release);
    slot.writing.store(false, std::memory_order_release);
}

}  // namespace

void RecordTransitionEvent(FGBoundary boundary, const char* name, uint32_t status, void* ptrA, void* ptrB,
                           const char* details) {
    RecordInternal(boundary, name, status, ptrA, ptrB, details);
}

void RecordFFXContextCreate(void* context, uint32_t effectId, uint32_t status, bool isSwapchain) {
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "effectId=0x%04X type=%s", effectId, isSwapchain ? "swapchain" : "effect");
    RecordInternal(FGBoundary::kFFX, "ContextCreate", status, context, nullptr, buf);
}

void RecordFFXContextDestroy(void* context, uint32_t effectId, uint32_t status, bool isSwapchain) {
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "effectId=0x%04X type=%s", effectId, isSwapchain ? "swapchain" : "effect");
    RecordInternal(FGBoundary::kFFX, "ContextDestroy", status, context, nullptr, buf);
}

void RecordFFXConfigure(void* context, bool enabled, void* originalCallback, void* bridgeKey, uint32_t status) {
    char buf[80] = {};
    std::snprintf(buf, sizeof(buf), "enabled=%d origCb=%p bridgeKey=%p", enabled ? 1 : 0, originalCallback, bridgeKey);
    RecordInternal(FGBoundary::kFFX, "Configure", status, context, bridgeKey, buf);
}

void RecordFFXCallbackArrival(void* userCtx, uint64_t frameId, bool isGenerated, bool bridgeValid,
                              bool lifetimeChanged) {
    char buf[80] = {};
    std::snprintf(buf, sizeof(buf), "frameId=%llu gen=%d valid=%d lifetimeChanged=%d",
                  static_cast<unsigned long long>(frameId), isGenerated ? 1 : 0, bridgeValid ? 1 : 0,
                  lifetimeChanged ? 1 : 0);
    RecordInternal(FGBoundary::kFFX, "CallbackArrived", lifetimeChanged ? 1 : 0, userCtx, nullptr, buf);
}

void RecordStreamlineDLSSGOptions(bool enabled, uint32_t status) {
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "optionsEnabled=%d", enabled ? 1 : 0);
    RecordInternal(FGBoundary::kStreamline, "DLSSGOptions", status, nullptr, nullptr, buf);
}

void RecordStreamlineFirstGeneratedOutput(uint64_t frameId, void* swapchain) {
    if (g_FirstGeneratedReported.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    char buf[64] = {};
    std::snprintf(buf, sizeof(buf), "firstGeneratedFrameId=%llu", static_cast<unsigned long long>(frameId));
    RecordInternal(FGBoundary::kStreamline, "FirstGeneratedOutput", 0, swapchain, nullptr, buf);
}

void RecordStreamlineReflexOptions(uint32_t mode, uint32_t sleepResult) {
    if (!g_FirstSleepReported.exchange(true, std::memory_order_acq_rel) || sleepResult != 0) {
        char buf[64] = {};
        std::snprintf(buf, sizeof(buf), "reflexMode=%u sleepResult=%u", mode, sleepResult);
        RecordInternal(FGBoundary::kStreamline, "ReflexOptions", sleepResult, nullptr, nullptr, buf);
    }
}

void RecordStreamlineVersions(const char* interposerVersion, const char* pluginVersion) {
    if (g_VersionsReported.exchange(true, std::memory_order_acq_rel)) {
        return;
    }
    char buf[96] = {};
    std::snprintf(buf, sizeof(buf), "interposer=%s plugin=%s", interposerVersion ? interposerVersion : "unknown",
                  pluginVersion ? pluginVersion : "unknown");
    RecordInternal(FGBoundary::kStreamline, "Versions", 0, nullptr, nullptr, buf);
}

void RecordSwapchainTransition(const char* action, void* swapchain, void* device, void* queue, uint32_t status) {
    char buf[80] = {};
    std::snprintf(buf, sizeof(buf), "act=%s sc=%p dev=%p q=%p", action ? action : "unknown", swapchain, device, queue);
    RecordInternal(FGBoundary::kDXGIAndOverlay, "SwapchainTransition", status, swapchain, queue, buf);
}

void RecordPresentRouteTransition(const char* caller, const char* route, void* swapchain, void* queue) {
    char buf[96] = {};
    std::snprintf(buf, sizeof(buf), "caller=%s route=%s sc=%p q=%p", caller ? caller : "unknown",
                  route ? route : "unknown", swapchain, queue);
    RecordInternal(FGBoundary::kDXGIAndOverlay, "PresentRoute", 0, swapchain, queue, buf);
}

void RecordRouteCoverageBoundary(const char* oldRoute, const char* newRoute, bool covered, uint64_t presentIndex) {
    char buf[96] = {};
    std::snprintf(buf, sizeof(buf), "old=%s new=%s covered=%d presentIdx=%llu", oldRoute ? oldRoute : "none",
                  newRoute ? newRoute : "none", covered ? 1 : 0, static_cast<unsigned long long>(presentIndex));
    RecordInternal(FGBoundary::kDXGIAndOverlay, "CoverageBoundary", covered ? 0 : 1, nullptr, nullptr, buf);
}

size_t CopyRecentTransitionHistory(FGTransitionRecord* outRecords, size_t maxRecords) {
    if (!outRecords || maxRecords == 0) {
        return 0;
    }
    size_t count = 0;
    for (HistorySlot& slot : g_HistoryRing) {
        const uint64_t seq = slot.sequence.load(std::memory_order_acquire);
        if (seq == 0 || slot.writing.load(std::memory_order_acquire)) {
            continue;
        }
        outRecords[count] = slot.record;
        ++count;
        if (count >= maxRecords) {
            break;
        }
    }
    return count;
}

bool WriteTransitionHistoryFile(const char* sessionDir, const char* triggerReason, DWORD exitCode) {
    if (!sessionDir || sessionDir[0] == '\0') {
        return false;
    }

    char filePath[MAX_PATH] = {};
    std::snprintf(filePath, sizeof(filePath), "%s\\fg_transition_trace.txt", sessionDir);

    HANDLE file = CreateFileA(filePath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_ALWAYS,
                              FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    auto writeLine = [&](const char* str) {
        if (!str) return;
        DWORD written = 0;
        WriteFile(file, str, static_cast<DWORD>(std::strlen(str)), &written, nullptr);
    };

    char header[512] = {};
    std::snprintf(header, sizeof(header),
                  "=== Capture Engine Frame Generation Transition Trace ===\r\n"
                  "PID: %lu\r\n"
                  "ExitCode: 0x%08lX\r\n"
                  "TriggerReason: %s\r\n"
                  "TimestampMs: %llu\r\n\r\n",
                  GetCurrentProcessId(), static_cast<unsigned long>(exitCode),
                  triggerReason ? triggerReason : "unknown",
                  static_cast<unsigned long long>(ce::hook_clock::TickCount64()));
    writeLine(header);

    // Section 1: Fault candidates from first-chance recorder
    writeLine("--- First-Chance Fault Candidates ---\r\n");
    ce::crash_first_chance::FaultCandidate candidates[16];
    const size_t candidateCount = ce::crash_first_chance::CopyFaultCandidates(candidates, 16);
    if (candidateCount == 0) {
        writeLine("No first-chance fault candidates recorded.\r\n\r\n");
    } else {
        for (size_t i = 0; i < candidateCount; ++i) {
            char candBuf[256] = {};
            std::snprintf(candBuf, sizeof(candBuf),
                          "Candidate[%zu]: seq=%llu tid=%lu code=0x%08lX addr=%p confirmed=%d hasContext=%d\r\n",
                          i, static_cast<unsigned long long>(candidates[i].sequence), candidates[i].threadId,
                          candidates[i].exceptionCode, reinterpret_cast<void*>(candidates[i].instructionAddress),
                          candidates[i].confirmedFault ? 1 : 0, candidates[i].hasRecordAndContext ? 1 : 0);
            writeLine(candBuf);
        }
        writeLine("\r\n");
    }

    // Section 2: Latest FG Session State Snapshot
    writeLine("--- Latest FG Session Snapshot ---\r\n");
    const FGSessionSnapshot snapshot = GetLatestFGSessionSnapshot();
    const FGActionPlan plan = GetLatestFGActionPlan();
    char snapBuf[512] = {};
    std::snprintf(snapBuf, sizeof(snapBuf),
                  "Epochs: session=%u runtime=%u swapchain=%u queue=%u\r\n"
                  "Mode: %s (active=%d)\r\n"
                  "Authority: %s StartupPhase: %s OverlayMode: %s\r\n"
                  "Queues: origGame=%p primary=%p swapchain=%p current=%p\r\n"
                  "Plan: route=%s transport=%s queueRole=%s backendMode=%s\r\n\r\n",
                  snapshot.sessionEpoch, snapshot.runtimeEpoch, snapshot.swapchainEpoch, snapshot.queueEpoch,
                  ce::fg_runtime::GetRuntimeModeName(snapshot.effectiveRuntimeMode),
                  snapshot.effectiveFGActive ? 1 : 0,
                  GetFGAuthorityKindName(snapshot.authority), GetFGStartupPhaseName(snapshot.startupPhase),
                  GetFGOverlayBackendModeName(snapshot.overlayMode),
                  snapshot.originalGameQueue.ptr, snapshot.primaryGameQueue.ptr, snapshot.swapchainQueue.ptr,
                  snapshot.currentCommandQueue.ptr,
                  GetFGPresentRouteName(plan.route), GetFGPresentTransportName(plan.transport),
                  GetFGQueueRoleName(plan.selectedQueueRole), GetFGOverlayBackendModeName(plan.backendMode));
    writeLine(snapBuf);

    // Section 3: Bounded Transition Event Timeline
    writeLine("--- Transition Event Timeline ---\r\n");
    FGTransitionRecord records[kHistoryCapacity];
    const size_t recordCount = CopyRecentTransitionHistory(records, kHistoryCapacity);
    if (recordCount == 0) {
        writeLine("No transition events recorded.\r\n");
    } else {
        // Sort chronologically by sequence
        for (size_t i = 0; i < recordCount; ++i) {
            for (size_t j = i + 1; j < recordCount; ++j) {
                if (records[j].sequence < records[i].sequence) {
                    FGTransitionRecord tmp = records[i];
                    records[i] = records[j];
                    records[j] = tmp;
                }
            }
        }
        for (size_t i = 0; i < recordCount; ++i) {
            const char* boundaryName = "UNKNOWN";
            switch (records[i].boundary) {
                case FGBoundary::kFFX: boundaryName = "FFX"; break;
                case FGBoundary::kStreamline: boundaryName = "STREAMLINE"; break;
                case FGBoundary::kDXGIAndOverlay: boundaryName = "DXGI_OVERLAY"; break;
                default: break;
            }
            char eventBuf[256] = {};
            std::snprintf(eventBuf, sizeof(eventBuf),
                          "[%04llu] t=%llu boundary=%s status=0x%X ptrA=%p ptrB=%p details=%s\r\n",
                          static_cast<unsigned long long>(records[i].sequence),
                          static_cast<unsigned long long>(records[i].timestampMs),
                          boundaryName, records[i].status, records[i].ptrA, records[i].ptrB, records[i].details);
            writeLine(eventBuf);
        }
    }

    CloseHandle(file);
    return true;
}

void ResetTransitionHistoryForTesting() {
    g_Counter.store(0, std::memory_order_release);
    g_FirstGeneratedReported.store(false, std::memory_order_release);
    g_FirstSleepReported.store(false, std::memory_order_release);
    g_VersionsReported.store(false, std::memory_order_release);
    for (HistorySlot& slot : g_HistoryRing) {
        slot.sequence.store(0, std::memory_order_release);
        slot.writing.store(false, std::memory_order_release);
        slot.record = {};
    }
}

}  // namespace ce::fg_session
