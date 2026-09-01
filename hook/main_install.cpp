#include "main_internal.h"

#include "common/module_pin.h"
#include "common/vulkan_renderer_policy.h"

void CloseCheckHooksEvent() {
  HANDLE hEvent = reinterpret_cast<HANDLE>(InterlockedExchangePointer(
      reinterpret_cast<PVOID volatile *>(&g_hCheckHooksEvent), nullptr));
  if (hEvent) {
    CloseHandle(hEvent);
  }
}

// Centralized Hook Detection Logic (Executed by HookThread)
void CheckAndInstallHooks() {
  std::lock_guard<std::mutex> lock(g_HookMutex);

  // RTX Remix is a D3D9-to-Vulkan renderer, but its public control interface is
  // independent of whichever presentation path CE owns. Keep its scheduler
  // hook active even when the Vulkan layer suppresses ordinary D3D hooks.
  RemixHook::Install();

  const bool dxvkD3D11WrapperLoaded = IsDXVKD3D11WrapperLoaded();
  const bool dxvkD3D9WrapperLoaded = IsDXVKD3D9WrapperLoaded();

  // CRITICAL FIX: Skip all D3D/DXGI hooks when Vulkan is the primary API.
  // Vulkan games using WSI-to-DXGI mapping can freeze if we hook DXGI/D3D.
  // The Vulkan layer (VK_LAYER_CE_overlay) handles overlay for Vulkan apps.
  //
  // NOTE: Many games load vulkan-1.dll as a transitive dependency without using
  // Vulkan for rendering (e.g., games with Vulkan support flags but running DX12).
  // Only treat Vulkan as the primary renderer if:
  //   - vulkan-1.dll is loaded, AND
  //   - No D3D usage evidence is present
  // D3D usage evidence includes D3D11/12 device creation and legacy D3D module
  // presence (DX9/DX8/DDraw), then we must not stay locked in Vulkan mode.
  static bool s_checkedForVulkan = false;
  static bool s_vulkanActive = false;
  // Layer ownership is authoritative and can arrive long after the first check.
  // On late injection the resident Vulkan layer only wakes once CaptureEngine
  // signals its per-PID reactivation event, which normally lands after this hook
  // thread has already weighed D3D evidence. Latching "not Vulkan" before that
  // would leave the DXGI present/resize path doing CE work for the rest of the
  // process while the layer owns presentation. Only ownership may re-open the
  // decision: re-opening on mere vulkan-1.dll presence would bring back the
  // RoboCop DX12 regression this latch exists to prevent.
  bool vulkanLayerOwned = false;
  if (SharedMemoryLayout* sharedMemory = GetHookSharedMemory()) {
    const uint32_t currentPid = GetCurrentProcessId();
    const uint64_t claim = sharedMemory->runtimeState.vulkanLayerClaim.load(std::memory_order_acquire);
    vulkanLayerOwned = sharedMemory->runtimeState.IsVulkanLayerOwnedByProcess(currentPid) ||
                       sharedMemory->runtimeState.IsVulkanPresentRecentForProcess(
                           currentPid, GetTickCount64(), 2000);
    if (claim != 0 && !ce::vulkan_layer_claim::BelongsToProcess(claim, currentPid)) {
      static std::atomic<uint64_t> s_lastRejectedVulkanClaim{0};
      if (s_lastRejectedVulkanClaim.exchange(claim, std::memory_order_relaxed) != claim) {
        EarlyLog("CheckAndInstallHooks: ignoring unrelated Vulkan claim rendererPid=%lu clientPid=%lu "
                 "currentPid=%lu",
                 static_cast<unsigned long>(ce::vulkan_layer_claim::RendererPid(claim)),
                 static_cast<unsigned long>(ce::vulkan_layer_claim::ClientPid(claim)),
                 static_cast<unsigned long>(currentPid));
      }
    }
  }
  if (!s_checkedForVulkan || s_vulkanActive || vulkanLayerOwned) {
    HMODULE hVulkan = GetModuleHandleW(L"vulkan-1.dll");
    bool legacyD3DLoaded = (GetModuleHandleA("d3d9.dll") != nullptr) ||
                           (GetModuleHandleA("d3d8.dll") != nullptr) ||
                           (GetModuleHandleA("ddraw.dll") != nullptr);
    // D3D usage evidence includes D3D11/12 device creation, d3d12.dll/d3d11.dll
    // presence (UE5 loads vulkan-1.dll even for DX12 games, and our D3D12CreateDevice
    // wrapper may not be installed yet if d3d12.dll loaded after our initial IAT
    // scan), and legacy D3D module presence (DX9/DX8/DDraw). DXVK's d3d11.dll is
    // only a D3D front-end over Vulkan, so there only a real D3D12 device counts.
    const bool d3dUsageEvidence = ce::vulkan_renderer_policy::HasD3DUsageEvidence(
        dxvkD3D11WrapperLoaded, WasD3D12DeviceCreated(), WasD3D11Or10DeviceCreated(),
        legacyD3DLoaded, GetModuleHandleA("d3d12.dll") != nullptr,
        GetModuleHandleA("d3d11.dll") != nullptr);
    const bool shouldTreatVulkanActive =
        ce::vulkan_renderer_policy::ShouldTreatVulkanAsActiveRenderer(
            hVulkan != nullptr, vulkanLayerOwned, d3dUsageEvidence);
    if (vulkanLayerOwned) {
      if (!s_vulkanActive) {
        // On late injection this fires after D3D evidence already latched, so
        // record whether hooks were installed in the meantime: that is the
        // difference between a clean Vulkan process and one where the DXGI/D3D
        // paths must now stand down for the layer.
        EarlyLog("CheckAndInstallHooks: Vulkan layer ownership established, skipping D3D/DXGI hooks "
                 "(previouslyLatchedD3D=%d dx12Hook=%d dx11Hook=%d)",
                 s_checkedForVulkan ? 1 : 0, g_DX12Hook ? 1 : 0, g_DX11Hook ? 1 : 0);
      }
      s_vulkanActive = true;
      s_checkedForVulkan = true;
    } else if (shouldTreatVulkanActive) {
      if (!s_vulkanActive) {
        EarlyLog("CheckAndInstallHooks: Vulkan detected (vulkan-1.dll, no D3D usage evidence), "
                 "skipping D3D/DXGI hooks");
      }
      s_vulkanActive = true;
      s_checkedForVulkan = true;
    } else if (d3dUsageEvidence) {
      // D3D usage evidence present — even if vulkan-1.dll is present, use D3D hooks
      if (s_vulkanActive) {
        EarlyLog("CheckAndInstallHooks: D3D evidence appeared after Vulkan detection; enabling "
                 "D3D/DXGI hooks");
      }
      s_vulkanActive = false;
      s_checkedForVulkan = true;
    }
    // If neither Vulkan nor D3D evidence is present yet, don't lock in.
  }

  // Publish the evidence-based decision for the DXGI present/resize paths so
  // they agree with hook installation (a DX12 UE5 process that merely loads
  // vulkan-1.dll keeps full DXGI processing and the overlay).
  DXGIShared::SetVulkanActiveForDXGIPresentPath(s_vulkanActive);
  if (s_vulkanActive) {
    const bool vulkanLayerModuleLoaded =
        GetModuleHandleW(L"VK_LAYER_CE_overlay.dll") != nullptr ||
        GetModuleHandleW(L"VK_LAYER_CE_overlay_x86.dll") != nullptr;
    ce::vulkan_dxgi_fifo::RegisterDynamicFactoryHooks(vulkanLayerModuleLoaded);
  }

  // WRAPPER-ONLY ARCHITECTURE: We use IAT-patched wrapper hooks for ALL games.
  // This is more robust than vtable hooks and avoids Steam overlay recursion
  // issues. The wrappers (CWrapDXGISwapChain, CWrapDXGIFactory2) handle all
  // interception. NOTE: InitializeWrapperHooks is skipped for Vulkan to prevent
  // DXGI interference
  if (!s_vulkanActive && !dxvkD3D11WrapperLoaded) {
    InitializeWrapperHooks();
  } else if (!s_vulkanActive && dxvkD3D11WrapperLoaded) {
    static bool s_loggedDXVKD3D11WrapperDeferral = false;
    if (!s_loggedDXVKD3D11WrapperDeferral) {
      EarlyLog("CheckAndInstallHooks: DXVK d3d11 detected, deferring DXGI/D3D wrapper init to Vulkan layer");
      s_loggedDXVKD3D11WrapperDeferral = true;
    }
  }

  // DX12: Only initialize the hook instance for state tracking and
  // ExecuteCommandLists hooking. We do NOT install DXGI vtable hooks anymore -
  // wrappers handle Present/ResizeBuffers. NOTE: Skip for Vulkan games to
  // prevent DXGI interference
  {
    HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
    const bool d3d12DeviceCreated = WasD3D12DeviceCreated();
#ifdef ENABLE_D3D12_WRAPPER
    const bool shouldInitDX12Hook = d3d12DeviceCreated;
#else
#  ifdef _WIN64
    const bool shouldInitDX12Hook = true;
#  else
    // x86: consider DX12 hooks whenever d3d12.dll is loaded (matching 64-bit
    // behavior), except when a non-system D3D translation runtime owns the
    // process and no real D3D12 device was observed. The global DXGI factory vtable hooks on
    // CreateSwapChain/CreateSwapChainForHwnd MUST be installed during DllMain
    // (synchronously) before any game code runs — otherwise the swapchain is
    // never intercepted and the overlay has no target. The earlier conditional
    // (WasD3D11Or10DeviceCreated()) was too late: it only became true after the
    // HookThread retry loop (1s tick), by which point the swapchain already
    // existed. The third-party overlay interference concern (nvspcap.dll) is
    // handled inside DX12Hook::Init() which defers only the eager Present hook
    // install, NOT InstallGlobalVTableHooks().
    const bool shouldInitDX12Hook = true;
#  endif
#endif
    const bool suppressDX12HookForD3DTranslation =
        ce::vulkan_renderer_policy::ShouldSuppressSpeculativeDX12Bootstrap(
            d3d12DeviceCreated, dxvkD3D11WrapperLoaded, dxvkD3D9WrapperLoaded);
    if (!s_vulkanActive && !g_DX12Hook && hD3D12 && shouldInitDX12Hook &&
        !suppressDX12HookForD3DTranslation) {
      HookLogImportant(
          "Detected D3D12 runtime presence. Initializing DX12 hook instance... "
          "(deviceCreated=%d)",
          d3d12DeviceCreated ? 1 : 0);

      // STATIC DESTRUCTOR FIX: Dynamically allocate the hook instance
      if (!g_dx12HookInstance) {
        g_dx12HookInstance = new DX12Hook();
      }
      g_DX12Hook = g_dx12HookInstance;

      // In no-wrapper builds WasD3D12DeviceCreated() never flips true, so late
      // injection would otherwise skip DX12Hook::Init() entirely and never arm
      // the Present/swapchain recovery path.
      g_DX12Hook->Init();
      HookLogImportant("DX12 hook instance ready");
    } else if (!s_vulkanActive && !g_DX12Hook && hD3D12 && suppressDX12HookForD3DTranslation) {
      static bool s_loggedDX12SkipForD3DTranslation = false;
      if (!s_loggedDX12SkipForD3DTranslation) {
        HookLog("DX12 hook init deferred: d3d12.dll is present, but a non-system D3D translation runtime "
                "owns rendering (d3d11=%d d3d9=%d)",
                dxvkD3D11WrapperLoaded ? 1 : 0, dxvkD3D9WrapperLoaded ? 1 : 0);
        s_loggedDX12SkipForD3DTranslation = true;
      }
    } else if (!s_vulkanActive && !g_DX12Hook && hD3D12 && !shouldInitDX12Hook) {
      static bool s_loggedWaitingForRealDX12Use = false;
      if (!s_loggedWaitingForRealDX12Use) {
#  ifdef _WIN64
        HookLog("DX12 hook init deferred: waiting for confirmed D3D12 device creation");
#  else
        HookLog("DX12 hook init skipped (x86): d3d12.dll present but no D3D12 device created");
#  endif
        s_loggedWaitingForRealDX12Use = true;
      }
    } else if (!s_vulkanActive && !g_DX12Hook && !hD3D12) {
      static bool s_loggedNoD3D12Yet = false;
      if (!s_loggedNoD3D12Yet) {
        HookLog("DX12 hook init deferred: d3d12.dll not loaded yet");
        s_loggedNoD3D12Yet = true;
      }
    }
  }

  // IMPORTANT: Install DX11 hooks based on ACTUAL API usage, not just DLL
  // presence. On modern Windows, d3d12.dll is often loaded by the D3D11 runtime
  // (D3D11On12), even for pure DX11 applications. The old check
  // (!GetModuleHandleA("d3d12.dll")) was incorrectly preventing DX11 hooks from
  // being installed in DX11 apps.
  //
  // New logic: Install DX11 hooks if:
  //   1. D3D11/D3D10 DLLs are present, AND
  //   2. Either D3D11/D3D10 device creation was actually called, OR
  //      D3D12CreateDevice was NOT actually called (so it's not a real DX12

  //      app)
  bool d3d11Or10DllPresent =
      (!dxvkD3D11WrapperLoaded &&
       (GetModuleHandleA("d3d11.dll") || GetModuleHandleA("d3d10.dll") ||
        GetModuleHandleA("d3d10_1.dll")));
  bool legacyD3DLoaded = (GetModuleHandleA("d3d9.dll") != nullptr) ||
                         (GetModuleHandleA("d3d8.dll") != nullptr) ||
                         (GetModuleHandleA("ddraw.dll") != nullptr);
  bool d3d11Or10DeviceCreated = !dxvkD3D11WrapperLoaded && WasD3D11Or10DeviceCreated();
  bool d3d12DeviceCreated = WasD3D12DeviceCreated();

  // Log third-party overlay presence for diagnostics
  {
    HMODULE hGameoverlay = GetModuleHandleA("gameoverlayrenderer.dll");
    if (hGameoverlay) {
      char overlayPath[MAX_PATH] = {};
      GetModuleFileNameA(hGameoverlay, overlayPath, MAX_PATH);
      HookLog("Third-party overlay detected: gameoverlayrenderer.dll (%s)", overlayPath);
    }
  }

  // NOTE: Skip D3D11 hooks for Vulkan games to prevent DXGI interference
  // Also avoid DX11 hook install in legacy D3D processes unless actual
  // D3D11/D3D10 device creation was observed (prevents DX9 interop false
  // positives when recording starts).
  //
  // Many DX11 games load d3d9.dll as a transitive dependency (for audio
  // codecs, Windows version checks, etc.) without using it for rendering.
  // The old (!d3d12DeviceCreated && !legacyD3DLoaded) fallback was too
  // conservative — it prevented DX11 hook installation in DX11 games that
  // happened to have d3d9.dll loaded.  Now we install DX11 hooks whenever
  // d3d11/d3d10 is present and D3D12 was NOT actually used (legacyD3DLoaded
  // is no longer a blocker: a true DX9-only game never hits DX11 hook paths
  // because it never calls D3D11 functions).
  {
    static int s_dx11CheckCount = 0;
    ++s_dx11CheckCount;
    bool dx11CondVulkanOk = !s_vulkanActive;
    bool dx11CondNoHookYet  = !g_DX11Hook;
    bool dx11CondDllPresent = d3d11Or10DllPresent;
    bool dx11CondDeviceOk   = (d3d11Or10DeviceCreated || !d3d12DeviceCreated);
    bool dx11CondAll        = dx11CondVulkanOk && dx11CondNoHookYet && dx11CondDllPresent && dx11CondDeviceOk;
    if (s_dx11CheckCount <= 5 || dx11CondAll) {
      HookLogImportant(
          "DX11 check #%d: vulkan=%d noHook=%d dllPresent=%d device=%d legacy=%d "
          "d3d12Created=%d => %s",
          s_dx11CheckCount, s_vulkanActive ? 1 : 0, g_DX11Hook ? 1 : 0,
          d3d11Or10DllPresent ? 1 : 0, d3d11Or10DeviceCreated ? 1 : 0,
          legacyD3DLoaded ? 1 : 0, d3d12DeviceCreated ? 1 : 0,
          dx11CondAll ? "INSTALL" : "skip");
    }
    if (dx11CondAll) {
      // The presence test above only proves the module was loaded a moment ago.
      // DX11Hook::Init resolves, inline-patches and calls into these images, so
      // take a permanent reference before committing: a transient probe load
      // (Witcher 3 startup, sessions 20260820_023643 / _031021) released
      // d3d11.dll about a second after CE saw it, which faulted CE first on the
      // unmapped export entry and then inside D3D11CreateDeviceAndSwapChain
      // while d3d11's detach tore the NVIDIA UMD adapter cache down underneath
      // it. See common/module_pin.h. Pin each present module, not the first one
      // found, so a D3D10-only process is covered too.
      const bool pinnedD3D11 = ce::module_pin::PinByName("d3d11.dll") != nullptr;
      const bool pinnedD3D10 = ce::module_pin::PinByName("d3d10.dll") != nullptr;
      const bool pinnedD3D10_1 = ce::module_pin::PinByName("d3d10_1.dll") != nullptr;
      if (!pinnedD3D11 && !pinnedD3D10 && !pinnedD3D10_1) {
        // Every candidate went away between the presence check and here, so
        // there is nothing to hook yet. Leave g_DX11Hook unset: the next
        // hook-thread tick re-evaluates instead of latching a no-op install.
        HookLogImportant(
            "DX11: d3d11/d3d10 unloaded between the presence check and the "
            "install commit - not installing, retrying on the next tick");
      } else {
        HookLog("Detected D3D10/11. Installing hooks... (D3D11/10 API called: %d, "
                "D3D12 API called: %d, LegacyD3D loaded: %d)",
                d3d11Or10DeviceCreated ? 1 : 0, d3d12DeviceCreated ? 1 : 0,
                legacyD3DLoaded ? 1 : 0);
        g_DX11Hook = new DX11Hook();
        LARGE_INTEGER _t1, _t2, _freq;
        QueryPerformanceFrequency(&_freq);
        QueryPerformanceCounter(&_t1);
        g_DX11Hook->Init();
        QueryPerformanceCounter(&_t2);
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        double _initMs = (double)(_t2.QuadPart - _t1.QuadPart) * 1000.0 / _freq.QuadPart;
        HookLog("D3D10/11 hooks installed (init=%.1f ms)", _initMs);
      }
    }
  }

  // For other APIs, skip if D3D12 was actually used (not just loaded).
  // d3d12.dll can be loaded by D3D11On12 even in non-DX12 apps.
  // We use the actual device creation flag instead of just DLL presence.
  bool dx12ActuallyUsed = WasD3D12DeviceCreated();

  // Never actively probe D3D9 after Vulkan ownership is established, or when
  // d3d9.dll is a non-system translation runtime. DX9Hook::Init creates a
  // synthetic factory/device to discover vtables; translation runtimes can
  // initialize a second global renderer from that probe. The IAT wrapper sees
  // real D3D9 objects, while the Vulkan layer owns final overlay/capture/pacing.
  // Also skip D3D9 hooks for DX11 games — d3d9.dll is often a transitive system
  // dependency (audio codecs, Windows version checks) in DX11 titles.
  const bool dx11DllLoaded = GetModuleHandleA("d3d11.dll") != nullptr;
  const bool d3d9DllLoaded = GetModuleHandleA("d3d9.dll") != nullptr;
  if (ce::vulkan_renderer_policy::ShouldBootstrapD3D9Hooks(
          s_vulkanActive, dxvkD3D9WrapperLoaded, g_DX9Hook != nullptr,
          dx12ActuallyUsed, dx11DllLoaded, d3d9DllLoaded)) {
    EarlyLog(
        "DX9 Hook Check: Installing DX9 hooks (d3d9.dll loaded, vulkanActive=%d, dx12Used=%d, dxvkD3D9=%d)",
        s_vulkanActive ? 1 : 0, dx12ActuallyUsed ? 1 : 0, dxvkD3D9WrapperLoaded ? 1 : 0);
    HookLog("Detected d3d9.dll. Installing DX9 hooks...");
    g_DX9Hook = new DX9Hook();
    LARGE_INTEGER _t1, _t2, _freq;
    QueryPerformanceFrequency(&_freq);
    QueryPerformanceCounter(&_t1);
    g_DX9Hook->Init();
    QueryPerformanceCounter(&_t2);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    double _initMs = (double)(_t2.QuadPart - _t1.QuadPart) * 1000.0 / _freq.QuadPart;
    HookLog("DX9 hooks installed (hook ptr=%p, init=%.1f ms)", (void*)g_DX9Hook, _initMs);
  } else if (!g_DX9Hook && d3d9DllLoaded) {
    static std::atomic<uint32_t> s_dx9SkipLogCount{0};
    const uint32_t skipCount = s_dx9SkipLogCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (skipCount <= 4 || (skipCount & (skipCount - 1)) == 0) {
      EarlyLog("DX9 Hook Check: Skipping DX9 hooks (vulkanActive=%d, dx12Used=%d, dx11Loaded=%d, "
               "dxvkD3D9=%d, occurrence=%lu)",
               s_vulkanActive ? 1 : 0, dx12ActuallyUsed ? 1 : 0, dx11DllLoaded ? 1 : 0,
               dxvkD3D9WrapperLoaded ? 1 : 0, static_cast<unsigned long>(skipCount));
    }
  }

  // DirectDraw titles can still load or probe D3D12 through DXGI/driver helper
  // components. That must not suppress the actual DirectDraw hook path.
  // Skip DirectDraw hooks when the Vulkan layer owns presentation. In the DXVK
  // D3D9 case the DX9 hook stays active, and synthesizing DirectDraw objects on
  // our worker thread can recurse into external overlays and crash.
  // Also skip DDraw hooks when a higher-level D3D API (d3d9, d3d8) is present,
  // because ddraw.dll is often a transitive system dependency and bootstrapping
  // DDraw (which internally creates a D3D9 device) can crash third-party overlays
  // that have already hooked Direct3DCreate9 (see DDrawHook::Init for details).
  if (!s_vulkanActive && !g_DDrawHook && GetModuleHandleA("ddraw.dll") &&
      !GetModuleHandleA("d3d9.dll") && !GetModuleHandleA("d3d8.dll")) {
    HookLog("Detected ddraw.dll. Installing DirectDraw hooks... (dx12Used=%d)",
            dx12ActuallyUsed ? 1 : 0);
    g_DDrawHook = new DDrawHook();
    LARGE_INTEGER _t1, _t2, _freq;
    QueryPerformanceFrequency(&_freq);
    QueryPerformanceCounter(&_t1);
    g_DDrawHook->Init();
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    QueryPerformanceCounter(&_t2);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    double _initMs = (double)(_t2.QuadPart - _t1.QuadPart) * 1000.0 / _freq.QuadPart;
    HookLog("DDraw hooks installed (init=%.1f ms)", _initMs);
  } else if (!s_vulkanActive && !g_DDrawHook && GetModuleHandleA("ddraw.dll")) {
    HookLog("DDraw hooks skipped (higher-level D3D API present: d3d9=%d d3d8=%d)",
            GetModuleHandleA("d3d9.dll") ? 1 : 0,
            GetModuleHandleA("d3d8.dll") ? 1 : 0);
  }

  if (!s_vulkanActive && !g_DX8Hook && !dx12ActuallyUsed && GetModuleHandleA("d3d8.dll")) {
    HookLog("Detected d3d8.dll. Installing DX8 hooks...");
    g_DX8Hook = new DX8Hook();
    LARGE_INTEGER _t1, _t2, _freq;
    QueryPerformanceFrequency(&_freq);
    QueryPerformanceCounter(&_t1);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_DX8Hook->Init();
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    QueryPerformanceCounter(&_t2);
    double _initMs = (double)(_t2.QuadPart - _t1.QuadPart) * 1000.0 / _freq.QuadPart;  // NOLINT(bugprone-narrowing-conversions)
    HookLog("DX8 hooks installed (init=%.1f ms)", _initMs);
  }

  if (!s_vulkanActive && !g_OpenGLHook && !dx12ActuallyUsed && GetModuleHandleA("opengl32.dll")) {
    HookLog("Detected opengl32.dll. Installing OpenGL hooks...");
    g_OpenGLHook = new OpenGLHook();
    LARGE_INTEGER _t1, _t2, _freq;
    QueryPerformanceFrequency(&_freq);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    QueryPerformanceCounter(&_t1);
    // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
    g_OpenGLHook->Init();
    QueryPerformanceCounter(&_t2);
    double _initMs = (double)(_t2.QuadPart - _t1.QuadPart) * 1000.0 / _freq.QuadPart;  // NOLINT(bugprone-narrowing-conversions)
    HookLog("OpenGL hooks installed (init=%.1f ms)", _initMs);
  }

  // Vulkan is handled by VK_LAYER_CE_overlay (ICD layer)
  // No hooking needed - the layer is loaded automatically by the Vulkan loader

  // FFX hooks for FSR FG detection
  // These hooks intercept ffxCreateContext/ffxDestroyContext to detect FSR FG
  // activation. Now safe with dedicated overlay queue - no race conditions with
  // game queue.
  FFXHook::Init();
  StreamlineHook::Init();

  // Install NVNGX and D3DKMT hooks for all games (injection delay prevents
  // D3D12 init crashes)
  {
    // Install NGX hooks if DLL is present
    NVNGXHook::Get().Install();

    // Install D3DKMT hooks for VRAM override (universal solution)
    // This hooks kernel-mode driver calls that games use to query VRAM
    // independently of DXGI (a common VRAM-reporting override technique)
    static bool s_D3DKMTHooksInstalled = false;
    if (!s_D3DKMTHooksInstalled) {
      if (D3DKMTHooks::Install()) {
        s_D3DKMTHooksInstalled = true;
        EarlyLog("D3DKMT hooks installed for VRAM override");
      }
    }
  }
}
