// Entry point of the flow-test build of the hook DLL (build/tests/flow/capture_hook_x64.dll); it replaces
// hook/runtime/main_dllmain.cpp and the hook thread. The test host loads the DLL like an injection and runs
// CE's own hook installation and hook-thread service passes synchronously, so a scenario is one thread of
// deterministic calls. The DLL never reaches a running CaptureEngine: no IPC, no discovery mapping, its own
// log directory, and the host's never-activated window stands in for the foreground window.

#include "hook/d3d12/dx12_hook.h"
#include "hook/d3d12/dx12_hook_internal.h"
#include "hook/overlay/custom_overlay_dx12.h"
#include "hook/present/dxgi_shared_internal.h"
#include "hook/runtime/hook_clock.h"
#include "hook/runtime/main_internal.h"
#include "hook/wrappers/wrapper_hooks.h"
#include "tests/flow/flow_api.h"

extern "C" BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        // Before anything reads the time: CE runs on the game's frame clock from its first reading.
        ce::hook_clock::EnableVirtual();
        g_hModule = module;
        DisableThreadLibraryCalls(module);
        char logsDirectory[MAX_PATH] = {};
        if (!GetEnvironmentVariableA(kCEFlowLogDirectoryVariable, logsDirectory, MAX_PATH)) {
            GetModuleFileNameA(module, logsDirectory, MAX_PATH);
            if (char* slash = strrchr(logsDirectory, '\\'))
                strcpy_s(slash, logsDirectory + MAX_PATH - slash, "\\logs");
        }
        CreateDirectoryA(logsDirectory, nullptr);
        IsolateHookFromCaptureEngineHost(logsDirectory);
        char exePath[MAX_PATH] = {};
        if (GetModuleFileNameA(nullptr, exePath, MAX_PATH)) {
            const char* slash = strrchr(exePath, '\\');
            strncpy_s(g_ProcessName, slash ? slash + 1 : exePath, _TRUNCATE);
        }
    } else if (reason == DLL_PROCESS_DETACH && reserved != nullptr) {
        // As main_dllmain.cpp: at process exit every entry point turns into a pass-through.
        g_ProcessTerminating.store(true, std::memory_order_release);
        RequestHookShutdown();
    }
    return TRUE;
}

// The host's shared memory attached to CE's IPC client as main_hookthread.cpp attaches a connected one.
void AttachIsolatedHost(SharedMemoryLayout* memory) {
    g_IPC = new IPCClient();
    g_IPC->AttachIsolatedHostMemory(memory);
    g_pSharedMem = memory;
    g_pSharedMem->SetSourcePid(GetCurrentProcessId());
    ce::CreateHookContext();
    if (auto* context = ce::GetHookContext()) {
        context->hookModule = g_hModule;
        ce::SyncWithLegacyGlobals();
        context->hookLifecycle.TransitionTo(ce::HookState::Connected);
    }
}

// What DllMain and the hook thread do before the hook thread's service loop, minus threads and the IPC
// connection (main_dllmain.cpp, then main_hookthread.cpp up to InstallHookThreadHooks).
extern "C" __declspec(dllexport) bool CEFlow_Init(const char* configPath, SharedMemoryLayout* hostMemory) {
    if (!hostMemory)
        return false;
    InitializeHookLifecycleControl();
    EnsureLocalConfigAllocated();
    LoadConfig(configPath, *g_pLocalConfig);
    g_LocalConfigLoaded.store(true, std::memory_order_release);
    GetActiveGraphicsConfig();
    AttachIsolatedHost(hostMemory);
    InstallKernel32LoaderHooks("DllMain");
    InitializeWrapperHooks();
    InstallGlobalVTableHooks();
    InitializeThirdPartyOverlayDetection();
    InstallHookThreadHooks();
    HookLogImportant("CEFlow: hook initialized for an isolated test host (config=%s)", configPath);
    return true;
}

// One pass of the hook thread's service loop (main_hookthread.cpp) that matters to a D3D12 game.
extern "C" __declspec(dllexport) void CEFlow_PumpHookThread() {
    RefreshThirdPartyOverlayIdentityCache();
    CheckAndInstallHooks();
    if (g_DX12Hook)
        g_DX12Hook->ServicePendingPresentHooks();
    CustomOverlay::CollectRetiredDX12Backends();
}

extern "C" __declspec(dllexport) void CEFlow_SetForegroundWindow(HWND window) {
    SetIsolatedHostForegroundWindow(window);
}

extern "C" __declspec(dllexport) void CEFlow_GetOverlayCoverage(CEFlowOverlayCoverage* out) {
    const DX12OverlayCoverageSnapshot snapshot = GetOverlayCoverageSnapshot();
    out->presents = snapshot.totalPresents;
    out->uncovered = snapshot.uncoveredPresents;
    out->currentUncoveredStreak = snapshot.currentStreak;
    out->longestUncoveredStreak = snapshot.longestStreak;
    out->doubleDraws = snapshot.doubleDraws;
}

extern "C" __declspec(dllexport) void CEFlow_GetPublishedFG(CEFlowPublishedFG* out) {
    *out = {};
    if (PerformanceMetrics* metrics = DXGIShared::GetPerformanceMetrics()) {
        out->type = metrics->GetFGType();
        out->multiplier = metrics->GetFGMultiplier();
    }
}

extern "C" __declspec(dllexport) void CEFlow_AdvanceClock(int64_t microseconds) {
    ce::hook_clock::AdvanceMicroseconds(microseconds);
}

extern "C" __declspec(dllexport) int64_t CEFlow_ClockMicroseconds() {
    LARGE_INTEGER counter{};
    LARGE_INTEGER frequency{};
    ce::hook_clock::QueryCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return counter.QuadPart / frequency.QuadPart * 1000000 + counter.QuadPart % frequency.QuadPart * 1000000 /
                                                                  frequency.QuadPart;
}

extern "C" __declspec(dllexport) void CEFlow_Shutdown() {
    RequestHookShutdown();
}
