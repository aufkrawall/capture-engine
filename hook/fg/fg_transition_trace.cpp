#include "fg_transition_trace.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <iterator>
#include <mutex>

#include "common/crash/crash_first_chance.h"
#include "common/logging/log_meter.h"
#include "common/platform/bounded_diagnostic_history.h"
#include "fg_session_state.h"
#include "hook/metrics/perf_logger.h"
#include "hook/present/scanout_stall_report.h"
#include "hook/streamline/streamline_unload_order.h"
#include "hook/runtime/hook_clock.h"
#include "hook/runtime/hook_common.h"

namespace ce::fg_session {
namespace {

struct TransitionRecord {
    ULONGLONG timestampMs = 0;
    const char* boundary = nullptr;
    const char* name = nullptr;
    uint32_t status = 0;
    void* ptrA = nullptr;
    void* ptrB = nullptr;
    char details[112] = {};
};

struct PublishedFGState {
    FGSessionSnapshot snapshot;
    FGActionPlan plan;
};

BoundedDiagnosticHistory<TransitionRecord, 256> g_History;
BoundedDiagnosticHistory<PublishedFGState, 2> g_PublishedState;

std::atomic<void*> g_PendingStartupOutput{nullptr};
std::atomic<void*> g_PendingNativePresent{nullptr};
std::atomic<bool> g_PendingTitleMarker{false};
std::mutex g_StreamlineStartupMutex;
uint64_t g_StreamlineHandoff = 0;
int64_t g_StreamlineHandoffUs = 0;
int64_t g_FirstTitlePresentUs = 0;
DWORD g_FirstTitleThread = 0;

void Record(const char* boundary, const char* name, uint32_t status, void* ptrA, void* ptrB,
            const char* format, ...) __attribute__((format(printf, 6, 7)));

void Record(const char* boundary, const char* name, uint32_t status, void* ptrA, void* ptrB,
            const char* format, ...) {
    TransitionRecord record{ce::hook_clock::TickCount64(), boundary, name, status, ptrA, ptrB};
    va_list args;
    va_start(args, format);
    std::vsnprintf(record.details, sizeof(record.details), format, args);
    va_end(args);
    g_History.Append(record);
}

}  // namespace

void RecordStreamlineStartupHandoff(void* swapchain, int64_t timeUs) {
    uint64_t handoff = 0;
    {
        std::lock_guard lock(g_StreamlineStartupMutex);
        handoff = ++g_StreamlineHandoff;
        g_StreamlineHandoffUs = timeUs;
        g_FirstTitlePresentUs = 0;
        g_FirstTitleThread = 0;
        g_PendingTitleMarker.store(true, std::memory_order_release);
        g_PendingStartupOutput.store(swapchain, std::memory_order_release);
        g_PendingNativePresent.store(swapchain, std::memory_order_release);
    }
    HookLogImportant("[SL STARTUP] handoff=%llu sc=%p qpcUs=%lld", static_cast<unsigned long long>(handoff), swapchain,
                     static_cast<long long>(timeUs));
    // How long until this chain reaches the screen: the black seconds after GTA's DLSS-G handoff
    // (session 20261110_182518) show up as one `firstScreenChange` line.
    ce::scanout_stall::ArmFirstScreenChange(handoff, static_cast<IDXGISwapChain*>(swapchain));
}

void RecordStreamlineStartupTitlePresent(uint32_t frameId, int64_t timeUs) {
    if (!g_PendingTitleMarker.load(std::memory_order_relaxed)) {
        return;
    }
    uint64_t handoff = 0;
    int64_t elapsedUs = 0;
    const DWORD thread = GetCurrentThreadId();
    {
        std::lock_guard lock(g_StreamlineStartupMutex);
        if (!g_PendingTitleMarker.load(std::memory_order_relaxed) || timeUs < g_StreamlineHandoffUs) {
            return;
        }
        g_FirstTitlePresentUs = timeUs;
        g_FirstTitleThread = thread;
        elapsedUs = timeUs - g_StreamlineHandoffUs;
        g_PendingTitleMarker.store(false, std::memory_order_relaxed);
        handoff = g_StreamlineHandoff;
    }
    HookLogImportant("[SL STARTUP] handoff=%llu firstTitlePresentStart frame=%u tid=0x%04lX qpcUs=%lld "
                     "sinceHandoffUs=%lld", static_cast<unsigned long long>(handoff), frameId, thread,
                     static_cast<long long>(timeUs), static_cast<long long>(elapsedUs));
}

void RecordStreamlineStartupNativePresent(void* swapchain, const char* method) {
    ce::streamline_unload_order::NoteNativePresent(static_cast<IDXGISwapChain*>(swapchain));
    if (!swapchain || g_PendingNativePresent.load(std::memory_order_relaxed) != swapchain) {
        return;
    }
    const auto timeUs = PerfLogger::GetQpcUs();
    uint64_t handoff = 0;
    int64_t handoffUs = 0;
    {
        std::lock_guard lock(g_StreamlineStartupMutex);
        if (g_PendingNativePresent.load(std::memory_order_relaxed) != swapchain || timeUs < g_StreamlineHandoffUs) {
            return;
        }
        g_PendingNativePresent.store(nullptr, std::memory_order_relaxed);
        handoff = g_StreamlineHandoff;
        handoffUs = g_StreamlineHandoffUs;
    }
    HookLogImportant("[SL STARTUP] handoff=%llu sc=%p firstNativePresent=%s qpcUs=%lld sinceHandoffUs=%lld",
                     static_cast<unsigned long long>(handoff), swapchain, method,
                     static_cast<long long>(timeUs), static_cast<long long>(timeUs - handoffUs));
}

StreamlineStartupOutput BeginStreamlineStartupOutput(void* swapchain) {
    if (!swapchain || g_PendingStartupOutput.load(std::memory_order_acquire) != swapchain) {
        return {};
    }
    StreamlineStartupOutput output;
    int64_t handoffUs = 0;
    int64_t titlePresentUs = 0;
    DWORD titleThread = 0;
    {
        std::lock_guard lock(g_StreamlineStartupMutex);
        if (g_PendingStartupOutput.load(std::memory_order_relaxed) != swapchain) {
            return {};
        }
        output = {swapchain, g_StreamlineHandoff, PerfLogger::GetQpcUs()};
        handoffUs = g_StreamlineHandoffUs;
        titlePresentUs = g_FirstTitlePresentUs;
        titleThread = g_FirstTitleThread;
        g_PendingStartupOutput.store(nullptr, std::memory_order_release);
        g_PendingTitleMarker.store(false, std::memory_order_relaxed);
    }
    HookLogImportant("[SL STARTUP] handoff=%llu sc=%p firstOutputCallbackEnter qpcUs=%lld sinceHandoffUs=%lld "
                     "sinceFirstTitleMarkerUs=%lld titleTid=0x%04lX",
                     static_cast<unsigned long long>(output.handoff), swapchain,
                     static_cast<long long>(output.enteredUs), static_cast<long long>(output.enteredUs - handoffUs),
                     static_cast<long long>(titlePresentUs == 0 ? -1 : output.enteredUs - titlePresentUs), titleThread);
    return output;
}

void EndStreamlineStartupOutput(const StreamlineStartupOutput& output) {
    if (!output.swapchain) {
        return;
    }
    const auto elapsedUs = PerfLogger::GetQpcUs() - output.enteredUs;
    HookLogImportant("[SL STARTUP] handoff=%llu sc=%p firstOutputCallbackReturn overlayCallbackUs=%lld",
                     static_cast<unsigned long long>(output.handoff), output.swapchain,
                     static_cast<long long>(elapsedUs));
    Record("STREAMLINE", "FirstOutputCallback", 0, output.swapchain, nullptr, "handoff=%llu elapsedUs=%lld",
           static_cast<unsigned long long>(output.handoff), static_cast<long long>(elapsedUs));
}

void RecordPublishedFGState(const FGSessionSnapshot& snapshot, const FGActionPlan& plan) {
    g_PublishedState.Append({snapshot, plan});
}

void RecordFFXContextCreate(void* context, uint32_t effectId, uint32_t status, bool isSwapchain) {
    Record("FFX", "ContextCreate", status, context, nullptr, "effectId=0x%04X type=%s",
           effectId, isSwapchain ? "swapchain" : "effect");
}

void RecordFFXContextDestroy(void* context, uint32_t effectId, uint32_t status, bool isSwapchain) {
    Record("FFX", "ContextDestroy", status, context, nullptr, "effectId=0x%04X type=%s",
           effectId, isSwapchain ? "swapchain" : "effect");
}

void RecordFFXConfigure(void* context, bool enabled, void* originalCallback, void* bridgeKey, uint32_t status) {
    // Successful configures reach the trace only on a routing change. A provider can reject
    // the same packet every frame, so gate those independently before formatting.
    static thread_local log_meter::ChangeGate rejected;
    if (status != 0 && !rejected.Observe(log_meter::FieldKey(context, enabled, status))) {
        return;
    }
    if (status == 0) {
        rejected.Reset();
    }
    Record("FFX", "Configure", status, context, bridgeKey, "enabled=%d origCb=%p",
           enabled ? 1 : 0, originalCallback);
}

void RecordFFXCallbackArrival(void* userCtx, uint64_t frameId, bool isGenerated, bool bridgeValid) {
    // Real and generated outputs can alternate at display rate. A presenter
    // only records its first callback and changes of bridge identity or validity.
    struct ArrivalState {
        void* context = nullptr;
        bool valid = false;
        bool seen = false;
    };
    static thread_local ArrivalState previous;
    if (previous.seen && previous.context == userCtx && previous.valid == bridgeValid) {
        return;
    }
    previous = {userCtx, bridgeValid, true};
    Record("FFX", "CallbackArrived", 0, userCtx, nullptr, "frameId=%llu gen=%d bridgeValid=%d",
           static_cast<unsigned long long>(frameId), isGenerated ? 1 : 0, bridgeValid ? 1 : 0);
}

void RecordStreamlineDLSSGOptions(uint32_t viewport, bool enabled, uint32_t status) {
    static log_meter::StreamChangeGate<32> gate;
    const auto verdict = gate.Observe(viewport, log_meter::FieldKey(enabled, status));
    if (verdict) {
        Record("STREAMLINE", "DLSSGOptions", status, nullptr, nullptr, "viewport=%u enabled=%d%s",
               viewport, enabled ? 1 : 0,
               log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
}

void RecordStreamlineFeatureLifecycle(const char* api, uint32_t argument, uint32_t status) {
    Record("STREAMLINE", api, status, nullptr, nullptr, "feature=DLSS-G arg=%u", argument);
}

void RecordStreamlineReflexOptions(uint32_t mode, uint32_t status) {
    static log_meter::ChangeGate gate;
    const auto verdict = gate.Observe(log_meter::FieldKey(mode, status));
    if (verdict) {
        Record("STREAMLINE", "ReflexOptions", status, nullptr, nullptr, "mode=%u%s", mode,
               log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
}

void RecordStreamlineReflexSleepFailure(uint32_t status) {
    static log_meter::ChangeGate gate;
    const auto verdict = gate.Observe(status);
    if (verdict) {
        Record("STREAMLINE", "ReflexSleepFailed", status, nullptr, nullptr, "%s",
               log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
}

void RecordSwapchainTransition(const char* action, void* swapchain, void* device, void* queue, uint32_t status) {
    Record("DXGI_OVERLAY", "SwapchainTransition", status, swapchain, queue, "action=%s device=%p",
           action ? action : "unknown", device);
}

bool WriteTransitionHistoryFile(const char* sessionDir, const char* triggerReason, DWORD exitCode) {
    if (!sessionDir || !sessionDir[0]) {
        return false;
    }

    const DWORD pid = GetCurrentProcessId();
    char filePath[MAX_PATH] = {};
    const int pathLength = std::snprintf(filePath, sizeof(filePath), "%s\\fg_transition_trace_%lu.txt", sessionDir, pid);
    if (pathLength <= 0 || static_cast<size_t>(pathLength) >= sizeof(filePath)) {
        return false;
    }
    // The session can contain several injected processes and several exit hooks
    // can run within one process. Preserve the first trace for this PID.
    const HANDLE file = CreateFileA(filePath, GENERIC_WRITE, FILE_SHARE_READ, nullptr, CREATE_NEW,
                                    FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool succeeded = true;
    const auto writeLine = [&](const char* line) {
        if (succeeded) {
            DWORD written = 0;
            const auto length = static_cast<DWORD>(std::strlen(line));
            succeeded = WriteFile(file, line, length, &written, nullptr) && written == length;
        }
    };

    char line[512] = {};
    std::snprintf(line, sizeof(line),
                  "=== Capture Engine Frame Generation Transition Trace ===\r\n"
                  "PID: %lu\r\nExitCode: 0x%08lX\r\nTriggerReason: %.160s\r\nTimestampMs: %llu\r\n\r\n",
                  pid, exitCode, triggerReason ? triggerReason : "unknown",
                  static_cast<unsigned long long>(ce::hook_clock::TickCount64()));
    writeLine(line);

    writeLine("--- First-Chance Fault Candidates ---\r\n");
    ce::crash_first_chance::FaultCandidate candidates[ce::crash_first_chance::kFaultCandidateHistoryCapacity];
    const size_t candidateCount = ce::crash_first_chance::CopyFaultCandidates(candidates, std::size(candidates));
    if (candidateCount == 0) {
        writeLine("No first-chance fault candidates recorded.\r\n");
    }
    for (size_t i = 0; i < candidateCount; ++i) {
        const auto& candidate = candidates[i];
        std::snprintf(line, sizeof(line), "Candidate[%zu]: seq=%llu tid=%lu code=0x%08lX addr=%p\r\n",
                      i, static_cast<unsigned long long>(candidate.sequence), candidate.threadId,
                      candidate.exceptionCode, reinterpret_cast<void*>(candidate.instructionAddress));
        writeLine(line);
    }

    writeLine("\r\n--- Latest Published FG Session Snapshot ---\r\n");
    PublishedFGState latest{};
    bool hasSnapshot = false;
    g_PublishedState.VisitRecent([&](uint64_t, const PublishedFGState& state) {
        latest = state;
        hasSnapshot = true;
    });
    if (hasSnapshot) {
        const auto& [snapshot, plan] = latest;
        std::snprintf(line, sizeof(line),
                      "Epochs: session=%u runtime=%u swapchain=%u queue=%u\r\n"
                      "Mode: %s (active=%d)\r\n"
                      "Authority: %s StartupPhase: %s OverlayMode: %s\r\n"
                      "Queues: origGame=%p primary=%p swapchain=%p current=%p\r\n"
                      "Plan: route=%s transport=%s queueRole=%s backendMode=%s\r\n",
                      snapshot.sessionEpoch, snapshot.runtimeEpoch, snapshot.swapchainEpoch, snapshot.queueEpoch,
                      fg_runtime::GetRuntimeModeName(snapshot.effectiveRuntimeMode), snapshot.effectiveFGActive ? 1 : 0,
                      GetFGAuthorityKindName(snapshot.authority), GetFGStartupPhaseName(snapshot.startupPhase),
                      GetFGOverlayBackendModeName(snapshot.overlayMode), snapshot.originalGameQueue.ptr,
                      snapshot.primaryGameQueue.ptr, snapshot.swapchainQueue.ptr, snapshot.currentCommandQueue.ptr,
                      GetFGPresentRouteName(plan.route), GetFGPresentTransportName(plan.transport),
                      GetFGQueueRoleName(plan.selectedQueueRole), GetFGOverlayBackendModeName(plan.backendMode));
        writeLine(line);
    } else {
        writeLine("No FG session snapshot published.\r\n");
    }

    writeLine("\r\n--- Transition Event Timeline ---\r\n");
    g_History.VisitRecent([&](uint64_t sequence, const TransitionRecord& record) {
        std::snprintf(line, sizeof(line),
                      "[%04llu] t=%llu boundary=%s status=0x%X ptrA=%p ptrB=%p details=%s: %s\r\n",
                      static_cast<unsigned long long>(sequence), static_cast<unsigned long long>(record.timestampMs),
                      record.boundary, record.status, record.ptrA, record.ptrB, record.name, record.details);
        writeLine(line);
    });

    CloseHandle(file);
    return succeeded;
}

}  // namespace ce::fg_session
