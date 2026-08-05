#include "main_internal.h"

static PVOID g_DllNotificationCookie = nullptr;

static VOID CALLBACK OverlayDllNotificationCallback(ULONG reason,
                                                    PCLDR_DLL_NOTIFICATION_DATA data,
                                                    PVOID /*context*/) {
  if (!data || !data->BaseDllName || !data->BaseDllName->Buffer ||
      data->BaseDllName->Length == 0) {
    return;
  }
  // Narrow the (short) base name on the stack — loader-safe, no allocation.
  char base[128];
  const wchar_t *wide = data->BaseDllName->Buffer;
  const size_t wideChars = data->BaseDllName->Length / sizeof(wchar_t);
  size_t n = 0;
  for (; n < wideChars && n < (sizeof(base) - 1); ++n) {
    const wchar_t c = wide[n];
    base[n] = (c > 0 && c < 128) ? static_cast<char>(c) : '?';
  }
  base[n] = '\0';

  if (reason == LDR_DLL_NOTIFICATION_REASON_LOADED) {
    const char *matched = ce::overlay_compat::NoteModuleLoadedForOverlayCache(base);
    if (matched) {
      HookLog("DllNotification: third-party overlay module loaded: %s", matched);
    }
  } else if (reason == LDR_DLL_NOTIFICATION_REASON_UNLOADED) {
    const char *matched = ce::overlay_compat::NoteModuleUnloadedForOverlayCache(base);
    if (matched) {
      HookLog("DllNotification: third-party overlay module unloaded: %s", matched);
    }
    // Games can unload the whole Streamline stack when toggling DLSS FG off.
    // Stale CE hook slots pointing into the departing image generation must be
    // invalidated here (loader-safe: interlocked/atomic writes + light logging),
    // or the next reload can land a different sl.* module inside the old range
    // and stale trampolines jump mid-instruction into it (20260612_003407).
    if (ce::streamline_runtime_policy::ShouldInvalidateStreamlineHooksOnModuleUnload(base)) {
      StreamlineHook::OnModuleUnloaded(data->DllBase, data->SizeOfImage, base);
    }
  }
}

// Seeds already-loaded overlays and registers the load/unload notification. MUST be called
// off the Present thread and after DllMain returns (i.e. from HookThread) — registering and
// the one-time GetModuleHandleA seed walk are not safe under the DllMain loader lock.
void InitializeThirdPartyOverlayDetection() {
  static std::atomic<bool> s_initialized{false};
  bool expected = false;
  if (!s_initialized.compare_exchange_strong(expected, true)) {
    return;
  }

  // 1) Seed overlays already present before our hooks installed (one-time loader walk).
  const uint32_t seeded = ce::overlay_compat::SeedThirdPartyOverlayModuleCacheFromLoader();
  const char *seededName = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
  HookLog("Third-party overlay detection: seed scan bits=0x%X active=%s", seeded,
          seededName ? seededName : "none");

  // 2) Register for all subsequent load/unload events (covers every load mechanism + unloads).
  HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
  auto registerFn = ntdll ? reinterpret_cast<PFN_LdrRegisterDllNotification>(
                                GetProcAddress(ntdll, "LdrRegisterDllNotification"))
                          : nullptr;
  if (registerFn) {
    const NTSTATUS status =
        registerFn(0, &OverlayDllNotificationCallback, nullptr, &g_DllNotificationCookie);
    if (status == 0) {
      HookLog("Third-party overlay detection: LdrRegisterDllNotification active (cookie=%p)",
              g_DllNotificationCookie);
    } else {
      HookLog("Third-party overlay detection: LdrRegisterDllNotification FAILED (0x%lX) — "
              "falling back to LoadLibrary/LdrLoadDll notifications only",
              static_cast<unsigned long>(status));
    }
  } else {
    HookLog("Third-party overlay detection: LdrRegisterDllNotification unavailable — falling "
            "back to LoadLibrary/LdrLoadDll notifications only");
  }
}

void NotifyHookModuleLoaded(HMODULE module, const char *moduleNameOrPath) {
  if (!module)
    return;

  // A DLL just loaded. Update third-party-overlay detection ONLY if this module is itself a
  // known overlay module — a cheap base-name compare, no loader walk. Unrelated loads (e.g.
  // d3d11.dll churn during the Alt+Tab mode switch) must NOT touch the detection state, so the
  // Present hot path never has to re-walk the loader. Full load/unload coverage is provided by
  // the LdrRegisterDllNotification callback; this is the belt-and-suspenders load path.
  ce::overlay_compat::NoteModuleLoadedForOverlayCache(moduleNameOrPath);

  TryInstallMiniDumpWriteDumpHookForModule(module, moduleNameOrPath);
  StreamlineHook::OnModuleLoaded(module, moduleNameOrPath);

  // Detect nvapi64.dll loading — trigger Reflex limiter initialization immediately
  // so our dynamic hook is registered before the game calls GetProcAddress.
  if (moduleNameOrPath) {
    const char *baseName = strrchr(moduleNameOrPath, '\\');
    baseName = baseName ? baseName + 1 : moduleNameOrPath;
    const char *slash = strrchr(baseName, '/');
    baseName = slash ? slash + 1 : baseName;
    if (_stricmp(baseName, "d3d8.dll") == 0) {
      HookLog("NotifyHookModuleLoaded: d3d8.dll detected - preparing DX8 hooks");
      DX8Hook_OnModuleLoaded();
    }
    if (_stricmp(baseName, "nvapi64.dll") == 0 || _stricmp(baseName, "nvapi.dll") == 0) {
      HookLog("NotifyHookModuleLoaded: %s detected — initializing Reflex limiter", baseName);
      g_ReflexLimiter.Init();
    }
    if (ce::overlay_compat::IsFFXFrameGenerationModulePath(moduleNameOrPath)) {
      HookLogImportant(
          "NotifyHookModuleLoaded: %s detected - initializing FFX hooks immediately for native FSR callback bridge",
          baseName);
      FFXHook::Init();
    }
    // CRITICAL FIX: Hook d3d11.dll at LoadLibrary time, BEFORE the game calls
    // GetProcAddress. UE3 caches D3D11 context vtable function pointers (Draw
    // etc.) at startup. If our IAT/dynamic hooks are installed too late (e.g.
    // from the HookThread's periodic scan), the game gets the real
    // D3D11CreateDevice pointer, creates the device, and UE3 caches the
    // original vtable entries — completely bypassing our vtable detours.
    //
    // By installing D3D11 hooks here, inside LoadLibrary before it returns,
    // our GetProcAddress hook intercepts the game's subsequent
    // GetProcAddress(d3d11.dll, "D3D11CreateDevice") and returns
    // Wrapped_D3D11CreateDevice instead. The wrapper creates a wrapped
    // device/context, and vtable hooks installed during device creation
    // actually intercept Draw calls.
    if (_stricmp(baseName, "d3d11.dll") == 0) {
      HookLog("NotifyHookModuleLoaded: d3d11.dll detected — installing D3D11 hooks "
              "immediately (pre-GetProcAddress)");
      IATHook::InitializeD3D11Hooks();
      // CRITICAL: Install the full GetProcAddress hook NOW, before LoadLibrary
      // returns. This ensures GetProcAddress(d3d11.dll, "D3D11CreateDevice")
      // called by ANY module (EXE or game DLL) is intercepted.
      //
      // We do NOT use PatchEAT() — the real 32-bit D3D11CreateDeviceAndSwapChain
      // on Win10/11 reads its own EAT entry internally, causing infinite
      // recursion if the EAT is patched.
      //
      // Prior crash (Steam overlay) was caused by EAT patching combined with
      // GetProcAddress hook, not by the hook alone. Without EAT patching,
      // oGetProcAddress returns the real function address, and the system-DLL
      // bypass in DetourGetProcAddress correctly returns the real function
      // to system callers.
      FFXHook::RegisterDynamicHooks();
      IATHook::InitializeGetProcAddressHook();
    }
  }


  if (g_hCheckHooksEvent)
    SetEvent(g_hCheckHooksEvent);
}

void ArmManualReflexQueryHookIfConfigured(const char *source) {
  if (!g_ReflexLimiter.IsManualLimiterConfiguredOrActive())
    return;

  // Games can cache NvAPI function pointers long before CE's first Present.
  // Arm the filtered QueryInterface path as soon as config/shared-memory state
  // proves manual Reflex mode is wanted, while keeping the existing caller
  // filter inside the Reflex limiter.
  g_ReflexLimiter.SetManualLimiterConfiguredOrActive(true);

  static std::atomic<bool> s_loggedEarlyManualReflexArm{false};
  if (!s_loggedEarlyManualReflexArm.exchange(true, std::memory_order_acq_rel)) {
    HookLogImportant(
        "ReflexLimiter: Early filtered nvapi_QueryInterface hook armed from %s "
        "manual Reflex configuration",
        source && source[0] ? source : "current");
  }
}
