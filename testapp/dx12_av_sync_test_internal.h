#pragma once

#include <windows.h>
#include <mutex>
#include <vector>
#include <wrl/client.h>

#include <d3d12.h>
#include <dxgi1_5.h>

#include "av_sync_stimulus.h"
#include "dx12_av_sync_audio.h"

using namespace Microsoft::WRL;

constexpr int kMarkerMargin = 24;
constexpr LONG kMarkerTile = 24;
constexpr LONG kMarkerGap = 6;

struct RuntimeSourceStall {
    testapp::avsync::SourceStallSpec spec;
    LARGE_INTEGER actualBeginQpc = {};
    LARGE_INTEGER actualEndQpc = {};
    uint64_t suppressedPresentCount = 0;
    bool active = false;
    bool completed = false;
};

extern int g_WindowWidth;
extern int g_WindowHeight;
extern int g_RequestedWidth;
extern int g_RequestedHeight;
extern int g_TargetFps;
extern int g_DurationSeconds;
extern int g_VSync;
extern int g_Fullscreen;
extern int g_WindowChrome;
extern int g_Topmost;
extern int g_GpuLoadPasses;
extern bool g_EncoderStressScene;
extern bool g_LogEveryFrame;
extern bool g_AudioEnabled;
extern bool g_AudioClockScheduling;
extern int g_AudioBufferMs;
extern double g_AudioLeadMs;
extern double g_AnalysisStartSeconds;
extern bool g_TearingRequested;
extern bool g_TearingSupported;
extern bool g_TearingActive;
extern bool g_Running;
extern LARGE_INTEGER g_QpcFreq;
extern LARGE_INTEGER g_StimulusStartQpc;
extern LARGE_INTEGER g_AppStartQpc;
extern uint64_t g_FrameId;
extern std::vector<RuntimeSourceStall> g_SourceStalls;
extern ComPtr<ID3D12CommandQueue> g_CommandQueue;
extern ComPtr<IDXGISwapChain3> g_SwapChain;
extern ComPtr<ID3D12Fence> g_Fence;
extern HANDLE g_FenceEvent;
extern HANDLE g_FrameTimer;
extern bool g_FrameTimerHighResolution;
extern UINT64 g_FenceValues[2];
extern UINT g_FrameIndex;
extern LARGE_INTEGER g_LastPresentQpc;
extern int g_LastPresentEventIndex;
extern double g_LastPresentDeltaMs;
extern double g_MaxPresentDeltaMs;
extern double g_PresentDeltaSumMs;
extern uint64_t g_PresentDeltaCount;
extern uint64_t g_FramePacingSpikeCount;
extern uint64_t g_WarmupPacingSpikeCount;
extern uint64_t g_EventBoundaryPacingSpikeCount;
extern uint64_t g_PlannedPresentGapCount;
extern bool g_PendingPlannedPresentGap;
extern std::mutex g_FrameSyncMutex;
extern testapp::avsync::AudioRenderer g_Audio;

double QpcToSeconds(LONGLONG deltaTicks);
LARGE_INTEGER QueryQpc();
double SecondsSinceStimulusStart();
void ScheduleStimulusStart(const char* reason);
double TargetFrameIntervalMs();
double FramePacingSpikeThresholdMs();
void InitializeFrameTimer();
void CloseFrameTimer();
bool WaitWithFrameTimer(LONGLONG remainingTicks);
std::wstring ExeSiblingPath(const wchar_t* fileName);
int ClampInt(int value, int lo, int hi);
void LoadConfig();
void ParseArgs(int argc, char** argv);
void RecordPresentTiming(const LARGE_INTEGER& presentQpc, int eventIndex);
void MoveToNextFrame();
void WaitForGpu();
void WriteManifest();
void LogEventSchedule();
void LogSourceStallSchedule();
