#pragma once

#include <windows.h>
#include <atomic>
#include <cstdint>

#include <dx12/ffx_api_dx12.h>
#include <dx12/ffx_api_framegeneration_dx12.h>
#include <ffx_framegeneration.h>

#include "dx12_fg_resources.h"
#include "testapp_common.h"

enum FsrInitResult {
    kFsrOk = 0,
    kFsrNoDll = 1,
    kFsrNoExports = 2,
    kFsrCreateFailed = 3,
    kFsrSwapChainFailed = 4,
};


extern int g_WindowWidth;
extern int g_WindowHeight;
extern int g_GpuLoadPasses;
extern int g_VSync;
extern int g_Fullscreen;
extern HMODULE g_FfxModule;
extern ffxContext g_FfxCtx;
extern ffxContext g_FfxSwapChainCtx;
extern PfnFfxCreateContext g_FfxCreateContext;
extern PfnFfxConfigure g_FfxConfigure;
extern PfnFfxDispatch g_FfxDispatch;
extern PfnFfxDestroyContext g_FfxDestroyContext;
extern bool g_FsrInitialized;
extern bool g_FsrEnabled;
extern bool g_FsrEnableAttempted;
extern uint64_t g_FrameIdCounter;
extern uint64_t g_LastFsrPrepareLogFrame;
extern uint64_t g_LastFsrUiRegisterLogFrame;
extern std::atomic<uint64_t> g_FsrPresentCallbackCount;
extern std::atomic<uint64_t> g_FsrFrameGenerationCallbackCount;

static constexpr uint64_t kNoFsrUiRegisterLogFrame = static_cast<uint64_t>(-1);

ffxReturnCode_t TestPresentCallback(ffxCallbackDescFrameGenerationPresent* params, void* userData);
ffxReturnCode_t TestFrameGenerationCallback(ffxDispatchDescFrameGeneration* params, void* userData);
const char* FfxReturnName(ffxReturnCode_t code);
void PreloadAmdCompanionDlls();
FsrInitResult LoadFSRRuntime();
void UnloadFSRRuntime();
