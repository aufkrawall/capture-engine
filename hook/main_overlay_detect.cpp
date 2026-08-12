#include "main_internal.h"
#include "../common/module_enumeration.h"

static PVOID g_DllNotificationCookie = nullptr;
static std::atomic<bool> g_OverlayIdentityRefreshNeeded{true};

static VOID CALLBACK OverlayDllNotificationCallback(ULONG reason,
                                                    PCLDR_DLL_NOTIFICATION_DATA data,
                                                    PVOID /*context*/) {
  if (!data || !data->BaseDllName || !data->BaseDllName->Buffer ||
      data->BaseDllName->Length == 0) {
    return;
  }
  if (HookIsShuttingDown()) {
    // The next active service pass performs a full retained-module identity
    // refresh; do not install hooks or touch UE/vendor state under the loader
    // lock while the resident runtime is dormant.
    g_OverlayIdentityRefreshNeeded.store(true, std::memory_order_release);
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
    UE5::NotifyModuleLoaded(static_cast<HMODULE>(data->DllBase));
    // Real load notification: also record load order so a later overlay that
    // displaces another overlay's Present entry jump (e.g. RTSS after Steam)
    // can be identified as the chain owner.
    const char *matched = ce::overlay_compat::NoteModuleLoadedForOverlayCacheFromNotification(base);
    if (matched) {
      HookLog("DllNotification: third-party overlay module loaded: %s", matched);
      // Loader-free bookkeeping only. A second overlay joining a Present entry CE has
      // already prepended over is the one case the install-time
      // ShouldLeavePresentEntryToForeignOverlayChain decision cannot cover: from here on the
      // two of them restore/re-hook those bytes around CE, and whichever re-hooks first
      // records CE as its "next", dropping the other out of the chain. Make that state
      // visible instead of leaving it to be re-diagnosed from overlay-disappearance reports.
      // Only a real CE prepend has that problem. In the left-to-foreign-chain mode CE's
      // Present view is a deep body hook that owns no entry bytes, so a late-joining
      // overlay composes with the others exactly as it would without CE.
      if (DXGIShared::HasPrependedPresentEntryHook() &&
          ce::overlay_compat::CountLoadedTrackedOverlayModules(
              ce::overlay_compat::TrackedOverlaySubset::kOverlay) >= 2) {
        static std::atomic<bool> s_lateOverlayJoinLogged{false};
        if (!s_lateOverlayJoinLogged.exchange(true, std::memory_order_acq_rel)) {
          HookLogImportant(
              "DllNotification: third-party overlay %s joined a Present entry CE already prepended over; "
              "CE cannot leave that entry retroactively, so one of the foreign overlays may stop drawing "
              "(start it before the game to get the wrapper-only coexistence path)",
              matched);
        }
      }
    }
    // Record the resolved full path for the configurable graphics runtime
    // family on every load mechanism (LoadLibrary, LdrLoadDll, dependent
    // loads). This is the authoritative evidence of which physical DLL the
    // game actually uses - the redirect logs alone only prove the redirect
    // decision, not which copy won.
    // The NGX model cache (C:\ProgramData\NVIDIA\NGX\models\sl_*_0\...) loads
    // Streamline plugins under hashed names (1B0_E658703.dll etc.) that never
    // match the sl.* base-name family, so classify the resolved full path too.
    wchar_t fullPath[MAX_PATH] = {};
    const DWORD pathLen =
        GetModuleFileNameW(static_cast<HMODULE>(data->DllBase), fullPath, MAX_PATH);
    char narrowPath[MAX_PATH] = {};
    const bool hasPath = pathLen > 0 && pathLen < MAX_PATH;
    if (hasPath) {
      WideCharToMultiByte(CP_UTF8, 0, fullPath, -1, narrowPath, MAX_PATH, nullptr, nullptr);
    }
    if (ce::graphics_runtime::IsRuntimeModuleBaseName(base) ||
        (hasPath && ce::graphics_runtime::IsNgxModelRepositoryPath(narrowPath))) {
      if (hasPath) {
        HookLogImportant("Loader: runtime module loaded: %s -> %s (base=%p size=0x%zX%s)", base,
                         narrowPath, data->DllBase, data->SizeOfImage,
                         ce::graphics_runtime::IsNgxModelRepositoryPath(narrowPath)
                             ? ", NGX model repository"
                             : "");
      } else {
        HookLogImportant("Loader: runtime module loaded: %s (base=%p size=0x%zX, path unavailable)",
                         base, data->DllBase, data->SizeOfImage);
      }
    }
  } else if (reason == LDR_DLL_NOTIFICATION_REASON_UNLOADED) {
    UE5::NotifyModuleUnloaded(data->DllBase, data->SizeOfImage);
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
  g_OverlayIdentityRefreshNeeded.store(true, std::memory_order_release);
  if (g_hCheckHooksEvent)
    SetEvent(g_hCheckHooksEvent);
}

void RefreshThirdPartyOverlayIdentityCache() {
  if (!g_OverlayIdentityRefreshNeeded.exchange(false, std::memory_order_acq_rel))
    return;

  std::vector<HMODULE> modules;
  if (!ce::EnumerateProcessModules(GetCurrentProcess(), modules)) {
    // Preserve the last complete bank and retry on the next service pass. An
    // empty replacement published from a transient enumeration failure would
    // briefly misclassify generic ReShade/Special K proxy filenames.
    g_OverlayIdentityRefreshNeeded.store(true, std::memory_order_release);
    return;
  }

  struct IdentifiedPath {
    char narrow[MAX_PATH] = {};
    wchar_t wide[MAX_PATH] = {};
  } identifiedPaths[ce::overlay_compat::kIdentifiedOverlayPathSlotCount];
  size_t identifiedPathCount = 0;
  bool reshadeProxy = false;
  bool specialKProxy = false;
  bool optiScalerProxy = false;
  for (HMODULE module : modules) {
    HMODULE retained = nullptr;
    if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCSTR>(module), &retained))
      continue;
    char path[MAX_PATH] = {};
    if (GetModuleFileNameA(retained, path, MAX_PATH)) {
      const char* baseName = ce::overlay_compat::detail::ExtractBaseName(path);
      ce::overlay_compat::NoteModuleLoadedForOverlayCache(baseName);
      const bool proxyCandidate = ce::overlay_compat::IsThirdPartyGraphicsProxyCandidateName(baseName);
      const bool isReshade = GetProcAddress(retained, "ReShadeVersion") != nullptr ||
                             GetProcAddress(retained, "ReShadeRegisterAddon") != nullptr ||
                             GetProcAddress(retained, "ReShadeUnregisterAddon") != nullptr ||
                             (proxyCandidate && DllVersionStringContains(path, "ReShade"));
      const bool isSpecialK = GetProcAddress(retained, "SK_GetDLL") != nullptr ||
                              GetProcAddress(retained, "SK_Inject_GetRecord") != nullptr ||
                              (proxyCandidate && DllVersionStringContains(path, "Special K"));
      const bool isOptiScaler = proxyCandidate && DllVersionStringContains(path, "OptiScaler");
      if (isReshade || isSpecialK || isOptiScaler) {
        if (identifiedPathCount < ce::overlay_compat::kIdentifiedOverlayPathSlotCount) {
          auto& identified = identifiedPaths[identifiedPathCount++];
          strncpy_s(identified.narrow, _countof(identified.narrow), path, _TRUNCATE);
          GetModuleFileNameW(retained, identified.wide, MAX_PATH);
        }
        reshadeProxy |= isReshade;
        specialKProxy |= isSpecialK;
        optiScalerProxy |= isOptiScaler;
      }
    }
    FreeLibrary(retained);
  }

  // Start the publication transaction only after every loader/file-version
  // query has completed. Once the sequence is odd, the remainder is fixed-size
  // atomic publication and cannot abandon the bank halfway through.
  const uint32_t identityBank = ce::overlay_compat::BeginIdentifiedThirdPartyOverlayModulePathRefresh();
  for (size_t i = 0; i < identifiedPathCount; ++i) {
    ce::overlay_compat::PublishIdentifiedThirdPartyOverlayModulePathToBank(identifiedPaths[i].narrow, identityBank);
    if (identifiedPaths[i].wide[0]) {
      ce::overlay_compat::PublishIdentifiedThirdPartyOverlayModulePathToBank(identifiedPaths[i].wide, identityBank);
    }
  }

  ce::overlay_compat::SetIdentifiedOverlayIdentityLoaded("CE.ReShadeProxyIdentity", reshadeProxy);
  ce::overlay_compat::SetIdentifiedOverlayIdentityLoaded("CE.SpecialKProxyIdentity", specialKProxy);
  ce::overlay_compat::SetIdentifiedOverlayIdentityLoaded("CE.OptiScalerProxyIdentity", optiScalerProxy);
  ce::overlay_compat::CommitIdentifiedThirdPartyOverlayModulePathRefresh(identityBank);

  const uint32_t identityMask = (reshadeProxy ? 1u : 0u) | (specialKProxy ? 2u : 0u) |
                                (optiScalerProxy ? 4u : 0u);
  static std::atomic<uint32_t> lastIdentityMask{UINT32_MAX};
  if (lastIdentityMask.exchange(identityMask, std::memory_order_acq_rel) != identityMask) {
    HookLogImportant("Third-party overlay identities changed (ReShade=%d SpecialK=%d OptiScaler=%d)",
                     reshadeProxy ? 1 : 0, specialKProxy ? 1 : 0, optiScalerProxy ? 1 : 0);
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

  // Register before the seed walk. A DLL that loads or unloads during the walk
  // is then reflected by the callback as well, so there is no observation gap
  // between the initial snapshot and continuous notification coverage.
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

  const uint32_t seeded = ce::overlay_compat::SeedThirdPartyOverlayModuleCacheFromLoader();
  RefreshThirdPartyOverlayIdentityCache();
  const char *seededName = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
  HookLog("Third-party overlay detection: seed scan bits=0x%X active=%s", seeded,
          seededName ? seededName : "none");
}

void NotifyHookModuleLoaded(HMODULE module, const char *moduleNameOrPath) {
  if (!module || HookIsShuttingDown())
    return;

  // Modules that loaded after the initial IAT snapshot keep their real
  // LoadLibrary* imports; patch them here so their internal runtime loads
  // (e.g. sl.common.dll loading the DLSS plugins) reach the redirect and
  // module-load observation. No-op when no redirection overrides are
  // configured.
  PatchLoadLibraryIatForLateLoadedModule(module, moduleNameOrPath);

  // A DLL just loaded. Update third-party-overlay detection ONLY if this module is itself a
  // known overlay module — a cheap base-name compare, no loader walk. Unrelated loads (e.g.
  // d3d11.dll churn during the Alt+Tab mode switch) must NOT touch the detection state, so the
  // Present hot path never has to re-walk the loader. Full load/unload coverage is provided by
  // the LdrRegisterDllNotification callback; this is the belt-and-suspenders load path.
  ce::overlay_compat::NoteModuleLoadedForOverlayCacheFromNotification(moduleNameOrPath);
  g_OverlayIdentityRefreshNeeded.store(true, std::memory_order_release);

  TryInstallMiniDumpWriteDumpHookForModule(module, moduleNameOrPath);
  StreamlineHook::OnModuleLoaded(module, moduleNameOrPath);
  UE5::NotifyModuleLoaded(module);

  // nvngx.dll is loaded by sl.common.dll, which then resolves every NGX entry
  // point with GetProcAddress. Hook its exports here, inside LoadLibrary,
  // so the first resolution already lands on our detours.
  NVNGXHook::Get().OnModuleLoaded(module, moduleNameOrPath);

  // Inject-side fallback for OpenGL and late/reloaded ICDs. LoadLibrary/LdrLoadDll
  // notifications run after the original loader call returns, so Vulkan's required
  // pre-device timing is owned by the Vulkan layer instead.
  ce::nv_lod_spread::OnModuleLoaded(module, moduleNameOrPath);

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
      ArmNgxFgPresetOverrideIfConfigured(baseName);
    }
    // The DLSS-G snippet reads the frame generation render preset out of the
    // driver settings through nvapi_QueryInterface, which it resolves with
    // GetProcAddress after its own load completes. Patch that import here so the
    // first resolution already reaches CE's dispatcher.
    if (ce::ngx_fg_preset::IsFrameGenerationSnippetModulePath(moduleNameOrPath)) {
      ArmNgxFgPresetOverrideIfConfigured(baseName);
      if (ce::ngx_fg_preset::IsArmed()) {
        void *originalGetProcAddress = nullptr;
        const bool patched =
            IATHook::PatchIAT(module, "kernel32.dll", "GetProcAddress",
                              reinterpret_cast<void *>(&IATHook::DetourGetProcAddress),
                              &originalGetProcAddress);
        HookLogImportant(
            "NGX FG preset: GetProcAddress import patch on %s %s (module=%p orig=%p)",
            baseName, patched ? "installed" : "FAILED", (void *)module,
            originalGetProcAddress);
      }
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

void ArmNgxFgPresetOverrideIfConfigured(const char *source) {
  // GetActiveGraphicsConfig() publishes the resolved preset to the override unit.
  GetActiveGraphicsConfig();
  const uint32_t preset = ce::ngx_fg_preset::GetConfiguredPreset();
  if (preset == 0)
    return;

  // nvngx_dlssg resolves NvAPI_DRS_GetSetting lazily and caches the pointer for
  // the rest of the process, so the filtered nvapi_QueryInterface path has to
  // exist before the first frame generation feature is created.
  g_ReflexLimiter.EnsureNvApiQueryInterfaceInterception();

  static std::atomic<bool> s_loggedArm{false};
  if (!s_loggedArm.exchange(true, std::memory_order_acq_rel)) {
    HookLogImportant(
        "NGX FG preset: armed the DRS render-preset override for preset '%c' from %s",
        ce::ngx_fg_preset::PresetIdToLetter(preset),
        source && source[0] ? source : "current");
  }
}
