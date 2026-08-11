#include "main_internal.h"
#include "common/fps_limiter.h"

namespace {

HANDLE g_HostStoppingEvent = nullptr;
HANDLE g_InjectReactivateEvent = nullptr;
HANDLE g_InjectDormantEvent = nullptr;
std::atomic<bool> g_HookLifecycleBootstrapComplete{false};

void QuiesceHostBoundCaptureResources() {
  // Keep the WndProc link resident. A later third-party WndProc can retain
  // HookWndProc as its predecessor, and HookWndProc is already a transparent
  // forwarder. Rewriting GWLP_WNDPROC during cooperative dejection would add a
  // needless race with overlays installing or replacing their own link.
  if (g_DX12Hook)
    g_DX12Hook->OnHostDisconnect();
  if (g_DX11Hook)
    g_DX11Hook->OnHostDisconnect();
  if (g_DX9Hook)
    g_DX9Hook->OnHostDisconnect();
  if (g_DDrawHook)
    g_DDrawHook->OnHostDisconnect();
  if (g_DX8Hook)
    g_DX8Hook->OnHostDisconnect();
  if (g_OpenGLHook)
    g_OpenGLHook->OnHostDisconnect();
}

bool TryReactivateHookRuntime(bool launcherOnly) {
  // Consume the wakeup before inspecting discovery. A newer host that signals
  // while this attempt is in flight then remains visible for the next attempt.
  if (g_InjectReactivateEvent) {
    ResetEvent(g_InjectReactivateEvent);
  }
  if (!g_IPC || !isProcessWhitelistedFast(g_ProcessName) || !g_IPC->Reconnect()) {
    return false;
  }

  SharedMemoryLayout* sharedMemory = g_IPC->GetSharedMem();
  if (!sharedMemory) {
    return false;
  }

  if (!launcherOnly) {
    g_pSharedMem = sharedMemory;
    sharedMemory->SetSourcePid(GetCurrentProcessId());
    sharedMemory->runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagInjectOverlayPending, true);
    g_SharedFpsLimiter.SetIPCClient(g_IPC);
  }
  ResumeHookRuntime();
  if (g_InjectDormantEvent) {
    ResetEvent(g_InjectDormantEvent);
  }

  if (launcherOnly) {
    HookLogImportant("[InjectLifecycle] Reactivated resident launcher hook for host PID %lu",
                     static_cast<unsigned long>(sharedMemory->GetHostPID()));
    return true;
  }

  const GraphicsConfig activeGraphicsConfig = GetActiveGraphicsConfig();

  // An initial late connection can occur before HookThread has installed its
  // loader hooks and created HookContext. The normal bootstrap immediately
  // performs those steps; only established runtimes should service graphics
  // reactivation from here.
  if (g_HookLifecycleBootstrapComplete.load(std::memory_order_acquire)) {
    RefreshThirdPartyOverlayIdentityCache();
    FFXHook::ReactivateResidentHooks();
    ce::dlss_indicator::Install(ce::dlss_indicator::ParseMode(
        g_pLocalConfig ? g_pLocalConfig->graphics.dlssDebugOverlay : std::string()));
    UE5::RefreshRayReconstructionOverride(activeGraphicsConfig.forceRayReconstruction);
    ArmManualReflexQueryHookIfConfigured("reconnected shared memory");
    ArmNgxFgPresetOverrideIfConfigured("reconnected shared memory");
    ce::SyncWithLegacyGlobals();
    CheckAndInstallHooks();
  }
  HookLogImportant("[InjectLifecycle] Reactivated resident hook for host PID %lu",
                   static_cast<unsigned long>(sharedMemory->GetHostPID()));
  return true;
}

}  // namespace

void InitializeHookLifecycleControl() {
  wchar_t eventName[64] = {};
  GenerateInjectHostStoppingEventName(eventName, _countof(eventName));
  g_HostStoppingEvent = CreateEventW(nullptr, TRUE, FALSE, eventName);
  GenerateInjectReactivateEventName(eventName, _countof(eventName), GetCurrentProcessId());
  g_InjectReactivateEvent = CreateEventW(nullptr, TRUE, FALSE, eventName);
  GenerateInjectDormantEventName(eventName, _countof(eventName), GetCurrentProcessId());
  g_InjectDormantEvent = CreateEventW(nullptr, TRUE, FALSE, eventName);
  if (g_InjectDormantEvent) {
    ResetEvent(g_InjectDormantEvent);
  }
  HookLog("[InjectLifecycle] Control events ready (reactivate=%p hostStopping=%p dormant=%p)",
          g_InjectReactivateEvent, g_HostStoppingEvent, g_InjectDormantEvent);
}

void MarkHookLifecycleBootstrapComplete() {
  // Adoption of a resident hook deliberately signals before the replacement
  // host can reconnect. A normal initial IPC connection has already consumed
  // its generation logically; clear any same-host wakeup now so later shutdown
  // cannot bounce through stale reactivation state.
  if (g_InjectReactivateEvent) {
    ResetEvent(g_InjectReactivateEvent);
  }
  g_HookLifecycleBootstrapComplete.store(true, std::memory_order_release);
}

bool DeactivateHookRuntimeAndWaitForHost(const char* reason, bool previousHostDied, bool launcherOnly) {
  RequestHookShutdown();
  if (!launcherOnly) {
    g_GraphicsOverridesActive.store(false, std::memory_order_release);
    ce::dlss_indicator::Install(ce::dlss_indicator::Mode::kPassthrough);
    ce::ngx_fg_preset::SetConfiguredPreset(0);
    UE5::ShutdownRayReconstructionOverride();
    FFXHook::EnterDormant();
    CaptureManager::Get().SetCaptureEnabled(false);
    g_SharedFpsLimiter.Shutdown();
    QuiesceHostBoundCaptureResources();
  }

  SharedMemoryLayout* oldSharedMemory = g_IPC ? g_IPC->GetSharedMem() : nullptr;
  if (oldSharedMemory && !launcherOnly) {
    oldSharedMemory->runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagInjectOverlayActive, false);
    oldSharedMemory->runtimeState.SetRuntimeFlag(kCaptureRuntimeFlagInjectOverlayPending, false);
    oldSharedMemory->SetSourcePid(0);
  }
  if (g_InjectDormantEvent) {
    SetEvent(g_InjectDormantEvent);
  }
  HookLogImportant(
      "[InjectLifecycle] Runtime dormant (%s); entry points are pass-through and resident pointers remain valid",
      reason ? reason : "host unavailable");

  if (!g_InjectReactivateEvent) {
    return false;
  }

  if (previousHostDied) {
    if (TryReactivateHookRuntime(launcherOnly)) {
      return true;
    }
  }

  for (;;) {
    if (WaitForSingleObject(g_InjectReactivateEvent, INFINITE) != WAIT_OBJECT_0) {
      return false;
    }
    if (TryReactivateHookRuntime(launcherOnly)) {
      return true;
    }

    // The wakeup was consumed before the attempt. Wait for another explicit
    // per-target signal rather than waiting out the advertised host: IPC setup
    // can fail transiently, and that host may signal again once its mapping or
    // whitelist generation is ready.
    HookLog("[InjectLifecycle] Reactivation attempt unavailable; waiting for the next target signal");
  }
}

void RunLauncherHookLifecycle() {
  g_IPC = new IPCClient();
  bool connected = g_IPC->Connect();
  if (!connected) {
    connected = DeactivateHookRuntimeAndWaitForHost("initial launcher IPC unavailable", true, true);
  } else {
    ResumeHookRuntime();
    if (g_InjectDormantEvent)
      ResetEvent(g_InjectDormantEvent);
  }

  while (connected) {
    SharedMemoryLayout* sharedMemory = g_IPC->GetSharedMem();
    const uint32_t hostPid = sharedMemory ? sharedMemory->GetHostPID() : 0;
    if (!sharedMemory || sharedMemory->GetRequestExit() || hostPid == 0) {
      connected = DeactivateHookRuntimeAndWaitForHost(
          sharedMemory && sharedMemory->GetRequestExit() ? "launcher host requested shutdown"
                                                        : "launcher host identity unavailable",
          hostPid == 0, true);
      continue;
    }

    HANDLE hostProcess = OpenProcess(SYNCHRONIZE, FALSE, hostPid);
    if (!hostProcess) {
      connected = DeactivateHookRuntimeAndWaitForHost("launcher host process inaccessible", true, true);
      continue;
    }

    HANDLE waits[2] = {};
    DWORD waitCount = 0;
    if (g_HostStoppingEvent)
      waits[waitCount++] = g_HostStoppingEvent;
    const DWORD hostProcessWaitIndex = waitCount;
    waits[waitCount++] = hostProcess;
    const DWORD waitResult = WaitForMultipleObjects(waitCount, waits, FALSE, INFINITE);
    CloseHandle(hostProcess);

    if (waitResult >= WAIT_OBJECT_0 + waitCount) {
      HookLogImportant("[InjectLifecycle] Launcher host wait failed (result=0x%08lX error=%lu)",
                       static_cast<unsigned long>(waitResult), static_cast<unsigned long>(GetLastError()));
      return;
    }

    const bool hostExited = waitResult == WAIT_OBJECT_0 + hostProcessWaitIndex;
    connected = DeactivateHookRuntimeAndWaitForHost(
        hostExited ? "launcher host process exited" : "launcher host stopping", hostExited, true);
  }
}
