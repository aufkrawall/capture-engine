#pragma once

// The C interface between the FG flow-test host (tests/flow/flow_host.cpp, inside fg_flow_tests.exe) and the
// flow-test build of the hook DLL (tests/flow/flow_hook_entry.cpp, build/tests/flow/capture_hook_x64.dll).
// The host resolves each export with GetProcAddress, as it would a real injected DLL's.

#include <windows.h>

#include <cstdint>

struct ID3D12CommandQueue;
struct SharedMemoryLayout;  // common/ipc/shared_defs.h

extern "C" {

struct CEFlowOverlayCoverage {
    uint64_t presents = 0;   // presents CE accounted (one per physical Present it saw)
    uint64_t uncovered = 0;  // of those, presents that showed no overlay
    uint64_t currentUncoveredStreak = 0;
    uint64_t longestUncoveredStreak = 0;
    uint64_t doubleDraws = 0;  // presents that got the overlay from two routes
    // Frame generation runtime outputs whose game frame the fake runtime reported (CEFlow_NoteRuntimeOutputFrame)
    // while CE had attributed them to a frame, and of those, outputs CE attributed to another frame than the ones
    // before them in that presenter lifetime (CE counts from its first Present, the fake from the proxy's).
    uint64_t outputFrameChecks = 0;
    uint64_t outputFrameMismatches = 0;
    // Of those, outputs whose frame's recorded overlay owner did not draw them exactly once: a topmost-owned frame's
    // output without the final-batch draw, or a UI-baseline frame's output with it. Judged at the output itself,
    // independent of the coverage ledger.
    uint64_t outputOwnerViolations = 0;
};

struct CEFlowPostSLLifecycle {
    uint32_t epoch = 0;
    uint32_t callbacksInFlight = 0;
    bool callbacksEnabled = false;
    bool confirmedInEpoch = false;
};

struct CEFlowPublishedFG {
    int type = 0;        // 0 none, 1 DLSS FG, 2 FSR FG, 3 NVIDIA Smooth Motion (PerformanceMetrics::GetFGType)
    int multiplier = 0;  // below 2: frame generation shown as off
};

// Environment variables the host sets before loading the DLL: the directory the hook logs into, and the frame
// interval of its game in microseconds (the fake runtimes space generated frames inside it).
constexpr const char* kCEFlowLogDirectoryVariable = "CE_FLOW_LOG_DIR";
constexpr const char* kCEFlowFrameIntervalVariable = "CE_FLOW_FRAME_INTERVAL_US";

// `hostMemory` is the inject host's shared memory, published by the test game as CaptureEngine publishes it
// (UpdateSharedMemoryFromConfig); CE uses it as a connected host's.
using CEFlow_Init_t = bool (*)(const char* configPath, SharedMemoryLayout* hostMemory);
using CEFlow_PumpHookThread_t = void (*)();
using CEFlow_SetForegroundWindow_t = void (*)(HWND window);
using CEFlow_GetOverlayCoverage_t = void (*)(CEFlowOverlayCoverage* out);
using CEFlow_GetPublishedFG_t = void (*)(CEFlowPublishedFG* out);
using CEFlow_Shutdown_t = void (*)();
using CEFlow_TrackQueue_t = void (*)(ID3D12CommandQueue*);
using CEFlow_ResetQueueBindings_t = void (*)();
using CEFlow_QueueOriginal_t = void* (*)(ID3D12CommandQueue*);
using CEFlow_ForwardQueue_t = void (*)(ID3D12CommandQueue*);
using CEFlow_GetPostSLLifecycle_t = void (*)(CEFlowPostSLLifecycle* out);
using CEFlow_TryConfirmPostSLEpoch_t = bool (*)(uint32_t epoch);
// The hook runs on a virtual clock (hook/runtime/hook_clock.h) that only these move.
using CEFlow_AdvanceClock_t = void (*)(int64_t microseconds);
using CEFlow_ClockMicroseconds_t = int64_t (*)();

}  // extern "C"
