#pragma once

// The C interface between the FG flow-test host (tests/flow/flow_host.cpp, inside fg_flow_tests.exe) and the
// flow-test build of the hook DLL (tests/flow/flow_hook_entry.cpp, build/tests/flow/capture_hook_x64.dll).
// The host resolves each export with GetProcAddress, as it would a real injected DLL's.

#include <windows.h>

#include <cstdint>

struct ID3D12CommandQueue;
struct ID3D12Device;
struct IDXGISwapChain;
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

// What CE's DX12 frame transaction did with the post-process pass (sharpen + display gamma) per normal-route frame.
struct CEFlowPostProcess {
    uint64_t frames = 0;
    uint64_t corrected = 0;
    uint64_t covered = 0;  // another route (PostSL, runtime output) corrects these frames itself
    uint64_t uncorrected = 0;
    uint64_t gapRuns = 0;
    uint64_t failed = 0;
    uint64_t unclassified = 0;
    uint64_t leftBeforeDecision = 0;
    uint64_t routeApplied = 0;  // corrected by a route outside the frame transaction (PostSL, FSR output)
    uint64_t routeFailed = 0;
};

// The raw D3D11 context's forced-AF work so far (hook/d3d11/dx11_hook_af_report.cpp; CE's context wrapper:
// CEFlow_GetD3D11WrapperSamplerCost, same order). The export writes
// plain values in this member order, so the hook needs no test header.
struct CEFlowD3D11SamplerCost {
    uint64_t srvBinds = 0;      // tracked pixel-stage PSSetShaderResources calls
    uint64_t samplerBinds = 0;  // tracked pixel-stage PSSetSamplers calls
    uint64_t passes = 0;        // dirty-slot reconciliations
    uint64_t fastSlots = 0;     // slots resolved from a memoized choice
    uint64_t slowSlots = 0;     // slots that classified sampler, shader or views
    uint64_t viewClasses = 0;   // view AF classes read back for a changed texture
    uint64_t rebinds = 0;       // physical sampler binds CE issued
    uint64_t evictions = 0;     // sampler choices evicted from the bounded per-context cache
    uint64_t shaderBinds = 0;   // tracked PSSetShader calls
    uint64_t timedBinds = 0;    // SRV and sampler binds whose cost was sampled (1 in 32 per thread)
    uint64_t timedShaderBinds = 0;  // shader binds whose cost, metadata lookup included, was sampled
    uint64_t shaderRepeats = 0;     // the tracked shader bound again
    uint64_t shaderMemoHits = 0;    // a shader switch resolved from the context's shader memo
    uint64_t shaderLookups = 0;     // a shader switch that read the shader's private data
    uint64_t shaderEvictions = 0;   // shader memo entries evicted (the memo holds 64)
};
using CEFlow_GetD3D11SamplerCost_t = uint32_t (*)(uint64_t* values, uint32_t count);
// The game's D3D11 sampler creations, process-wide (CEFlow_GetD3D11SamplerCreationStats, same order).
struct CEFlowD3D11SamplerCreationStats {
    uint64_t creates = 0;
    uint64_t tagsWritten = 0;
    uint64_t tagsKept = 0;      // the runtime returned an object already carrying the same tag
    uint64_t timedCreates = 0;  // creations whose cost was sampled (1 in 32 per thread)
};
// CE's D3D11 replacement samplers, process-wide (CEFlow_GetD3D11ReplacementStats, same order).
struct CEFlowD3D11ReplacementStats {
    uint64_t created = 0;         // created for a game sampler with none registered
    uint64_t hits = 0;            // found registered for the game's sampler
    uint64_t createFailures = 0;
    uint64_t registerFailures = 0;
    uint64_t entries = 0;         // game samplers holding replacements now
};
struct ID3D11DeviceContext;
struct ID3D11SamplerState;
using CEFlow_GetPhysicalPixelSampler_t = bool (*)(ID3D11DeviceContext* context, UINT slot,
                                                ID3D11SamplerState** sampler);

struct CEFlowPostSLLifecycle {
    uint32_t epoch = 0;
    uint32_t callbacksInFlight = 0;
    bool callbacksEnabled = false;
    bool confirmedInEpoch = false;
};

struct CEFlowQueueBinding {
    ID3D12CommandQueue* queue = nullptr;
    ID3D12Device* device = nullptr;
    IDXGISwapChain* successfulPostSLSwapchain = nullptr;
    IDXGISwapChain* capturedSwapchain = nullptr;
};

using CEFlow_GetQueueBinding_t = void (*)(CEFlowQueueBinding* out);
using CEFlow_AdoptQueue_t = void (*)(ID3D12CommandQueue*, bool discovery);
using CEFlow_BindSwapchainQueue_t = void (*)(ID3D12CommandQueue*, IDXGISwapChain*);

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
struct CEFlowEarlyLoaderProbe {
    const char* requestedPath = nullptr;
    const char* overrideDirectory = nullptr;
    HMODULE loadedModule = nullptr;
    bool disableNgxOta = false;
};
using CEFlow_Init_t = bool (*)(const char* configPath, SharedMemoryLayout* hostMemory,
                              CEFlowEarlyLoaderProbe* earlyLoaderProbe);
using CEFlow_PumpHookThread_t = void (*)();
using CEFlow_SetForegroundWindow_t = void (*)(HWND window);
using CEFlow_GetOverlayCoverage_t = void (*)(CEFlowOverlayCoverage* out);
using CEFlow_GetPublishedFG_t = void (*)(CEFlowPublishedFG* out);
using CEFlow_GetPostProcess_t = void (*)(CEFlowPostProcess* out);
using CEFlow_Shutdown_t = void (*)();
using CEFlow_TrackQueue_t = void (*)(ID3D12CommandQueue*);
using CEFlow_ResetQueueBindings_t = void (*)();
using CEFlow_ResetDeviceTrace_t = void (*)();
using CEFlow_QueueOriginal_t = void* (*)(ID3D12CommandQueue*);
using CEFlow_ForwardQueue_t = void (*)(ID3D12CommandQueue*);
using CEFlow_ForwardSignal_t = HRESULT (*)(ID3D12CommandQueue*, UINT64);
using CEFlow_RemoveSignalQueue_t = bool (*)(ID3D12CommandQueue*);
using CEFlow_RepairPresentHooks_t = void (*)();
using CEFlow_RetainRealSwapchain_t = IDXGISwapChain* (*)(IDXGISwapChain*);
using CEFlow_ReleasePresentVTableHooks_t = void (*)();
using CEFlow_InstallPresentVTableHooks_t = bool (*)(IDXGISwapChain*);
using CEFlow_GetPostSLLifecycle_t = void (*)(CEFlowPostSLLifecycle* out);
// The descriptor-free overlay backend drawing an opaque red quad into a fresh render target per format (the first
// builds the backend, the rest retarget it); `firstPixels` gets each target's first texel as stored.
using CEFlow_ProbeDescFreeTargetFormats_t = bool (*)(ID3D12Device*, ID3D12CommandQueue*, const int* formats,
                                                     uint32_t count, uint32_t* firstPixels, uint32_t* pipelineFormats);
using CEFlow_TryConfirmPostSLEpoch_t = bool (*)(uint32_t epoch);
// The hook runs on a virtual clock (hook/runtime/hook_clock.h) that only these move.
using CEFlow_AdvanceClock_t = void (*)(int64_t microseconds);
using CEFlow_ClockMicroseconds_t = int64_t (*)();

}  // extern "C"
