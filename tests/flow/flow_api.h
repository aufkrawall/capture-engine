#pragma once

// The C interface between the FG flow-test host (tests/flow/flow_host.cpp, inside fg_flow_tests.exe) and the
// flow-test build of the hook DLL (tests/flow/flow_hook_entry.cpp, build/tests/flow/capture_hook_x64.dll).
// The host resolves each export with GetProcAddress, as it would a real injected DLL's.

#include <windows.h>

#include <cstdint>

struct SharedMemoryLayout;  // common/ipc/shared_defs.h

extern "C" {

struct CEFlowOverlayCoverage {
    uint64_t presents = 0;   // presents CE accounted (one per physical Present it saw)
    uint64_t uncovered = 0;  // of those, presents that showed no overlay
    uint64_t currentUncoveredStreak = 0;
    uint64_t longestUncoveredStreak = 0;
    uint64_t doubleDraws = 0;  // presents that got the overlay from two routes
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
// The hook runs on a virtual clock (hook/runtime/hook_clock.h) that only these move.
using CEFlow_AdvanceClock_t = void (*)(int64_t microseconds);
using CEFlow_ClockMicroseconds_t = int64_t (*)();

}  // extern "C"
