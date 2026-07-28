static bool IsMemoryReadable(const void* ptr, size_t size) {
    MEMORY_BASIC_INFORMATION memInfo;
    if (VirtualQuery(ptr, &memInfo, sizeof(memInfo)) != sizeof(memInfo))
        return false;
    if (memInfo.State != MEM_COMMIT)
        return false;
    if (memInfo.Protect & (PAGE_NOACCESS | PAGE_GUARD))
        return false;
    return true;
}

// Scan process memory for existing IDirect3DDevice9 objects
// This is needed when we inject AFTER the game has already created its device
// and inline hooks are blocked by external overlays
static void ScanForExistingD3D9Devices() {
    auto LogScan = [](const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        EarlyLog("%s", buf);
    };

    LogScan("=== ScanForExistingD3D9Devices START ===");
    EarlyLog("DX9: Scanning for existing D3D9 devices in process memory...");

    HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
    if (!d3d9Module) {
        LogScan("d3d9.dll not loaded");
        EarlyLog("DX9: d3d9.dll not loaded, cannot scan for devices");
        return;
    }

    // Get d3d9.dll's address range
    MODULEINFO d3d9Info = {};
    if (!GetModuleInformation(GetCurrentProcess(), d3d9Module, &d3d9Info, sizeof(d3d9Info))) {
        LogScan("Failed to get d3d9.dll module info");
        EarlyLog("DX9: Failed to get d3d9.dll module info");
        return;
    }

    uintptr_t d3d9Start = (uintptr_t)d3d9Info.lpBaseOfDll;
    uintptr_t d3d9End = d3d9Start + d3d9Info.SizeOfImage;
    LogScan("d3d9.dll range: %p - %p", (void*)d3d9Start, (void*)d3d9End);
    EarlyLog("DX9: d3d9.dll range: %p - %p", (void*)d3d9Start, (void*)d3d9End);

    // Scan process memory for device objects
    // A D3D9 device object starts with a vtable pointer
    // The vtable should be within d3d9.dll's address range

    MEMORY_BASIC_INFORMATION memInfo;
    uintptr_t address = 0;
    int devicesFound = 0;
    int regionsScanned = 0;

    while (VirtualQuery((void*)address, &memInfo, sizeof(memInfo)) == sizeof(memInfo)) {
        // Only scan committed, readable, writable memory
        if (memInfo.State == MEM_COMMIT && (memInfo.Protect & PAGE_READWRITE) && !(memInfo.Protect & PAGE_GUARD)) {
            regionsScanned++;

            // Scan this memory region for vtable pointers
            uintptr_t* ptr = (uintptr_t*)memInfo.BaseAddress;
            uintptr_t* end = (uintptr_t*)((char*)memInfo.BaseAddress + memInfo.RegionSize);

            for (; ptr < end; ptr++) {
                uintptr_t vtablePtr = *ptr;

                // Check if this looks like a D3D9 device vtable pointer
                if (vtablePtr >= d3d9Start && vtablePtr < d3d9End) {
                    // Validate: Check if vtable entries are readable and within d3d9.dll
                    uintptr_t* vtable = (uintptr_t*)vtablePtr;

                    // Check if vtable memory is readable (avoid AV)
                    if (!IsMemoryReadable(vtable, 18 * sizeof(uintptr_t)))
                        continue;

                    // Check vtable[0] (QueryInterface), vtable[2] (Release),
                    // vtable[16] (Reset), vtable[17] (Present)
                    if (vtable[0] >= d3d9Start && vtable[0] < d3d9End && vtable[2] >= d3d9Start &&
                        vtable[2] < d3d9End && vtable[16] >= d3d9Start && vtable[16] < d3d9End &&
                        vtable[17] >= d3d9Start && vtable[17] < d3d9End) {
                        // This looks like a D3D9 device!
                        // The device pointer is the memory location containing the vtable ptr
                        IDirect3DDevice9* device = (IDirect3DDevice9*)ptr;

                        LogScan("Found potential D3D9 device at %p (vtable=%p)", device, (void*)vtablePtr);
                        EarlyLog("DX9: Found potential D3D9 device at %p (vtable=%p)", device, (void*)vtablePtr);

                        // If we haven't hooked anything yet, try to hook this device
                        if (!oPresent) {
                            LogScan("Attempting to install hooks on found device");
                            EarlyLog("DX9: Attempting to install hooks on found device");
                            InstallDeviceHooks(device);
                            devicesFound++;

                            if (oPresent) {
                                LogScan("Successfully hooked existing device!");
                                EarlyLog("DX9: Successfully hooked existing device!");
                                break;  // Found and hooked a device, stop scanning
                            } else {
                                LogScan("Hook installation failed");
                            }
                        }
                    }
                }
            }
        }

        address = (uintptr_t)memInfo.BaseAddress + memInfo.RegionSize;
        if (address < (uintptr_t)memInfo.BaseAddress)
            break;  // Overflow
    }

    LogScan("Scan complete: regionsScanned=%d, devicesFound=%d", regionsScanned, devicesFound);
    EarlyLog("DX9: Device scan complete, found %d device(s)", devicesFound);
}

static HRESULT STDMETHODCALLTYPE DetourCreateDevice(IDirect3D9* self, UINT Adapter, D3DDEVTYPE DeviceType,
                                                    HWND hFocusWindow, DWORD BehaviorFlags,
                                                    D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                    IDirect3DDevice9** ppReturnedDeviceInterface) {
    EarlyLog("DX9: IDirect3D9::CreateDevice called (hFocusWindow=%p)", hFocusWindow);

    if (IsDX9InternalHelperBypassActive()) {
        const HRESULT hr = oCreateDevice(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                         pPresentationParameters, ppReturnedDeviceInterface);
        if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
            DX9_RegisterInternalHelperDevice(*ppReturnedDeviceInterface);
        }
        return hr;
    }

    // VSync Override for CreateDevice
    if (pPresentationParameters) {
        const auto& gfx = GetActiveGraphicsConfig();
        g_WindowedPresent = !!pPresentationParameters->Windowed;
        UINT originalInterval = pPresentationParameters->PresentationInterval;
        UINT originalBackBufferCount = pPresentationParameters->BackBufferCount;
        UINT originalRefresh = pPresentationParameters->FullScreen_RefreshRateInHz;
        EarlyLog("DX9: CreateDevice: Requested MSAA Type=%d, Quality=%d", pPresentationParameters->MultiSampleType,
                 pPresentationParameters->MultiSampleQuality);

        VSyncOverride vsync = GetVSyncOverride();
        if (vsync.shouldOverride) {
            pPresentationParameters->PresentationInterval = (UINT)vsync.presentInterval;

            if (!pPresentationParameters->Windowed && vsync.presentInterval > 0 &&
                pPresentationParameters->FullScreen_RefreshRateInHz != 0) {
                static int logCount = 0;
                if (logCount++ < 10) {
                    HookLog("DX9: CreateDevice: Clearing FullScreen_RefreshRateInHz (was %u)",
                            pPresentationParameters->FullScreen_RefreshRateInHz);
                }
                pPresentationParameters->FullScreen_RefreshRateInHz = 0;
            }
        }

        // Backbuffer Count Override
        int count = gfx.backbufferCount;
        if (count >= 2 && count <= 6) {
            pPresentationParameters->BackBufferCount = (UINT)count - 1;
            HookLog("DX9: CreateDevice: Overriding BackBufferCount to %d", count);
        }

        HookLogImportant(
            "DX9: CreateDevice overrides vsync=%s interval %u->%u refresh %u->%u "
            "backbufferCfg=%d d3dBackBufferCount %u->%u prerender=%.2f",
            gfx.vsyncMode.c_str(), originalInterval, pPresentationParameters->PresentationInterval, originalRefresh,
            pPresentationParameters->FullScreen_RefreshRateInHz, gfx.backbufferCount, originalBackBufferCount,
            pPresentationParameters->BackBufferCount, gfx.cpuPrerenderLimit);

        // MSAA Override
        ApplyMSAAOverride(self, Adapter, DeviceType, pPresentationParameters);

        HookLogImportant("DX9: CreateDevice Flags In: 0x%X (PUREDEVICE=%d)", BehaviorFlags,
                         !!(BehaviorFlags & D3DCREATE_PUREDEVICE));
        HookLogImportant("DX9: CreateDevice flags preserved (multithreaded=%d pure=%d)",
                         !!(BehaviorFlags & D3DCREATE_MULTITHREADED), !!(BehaviorFlags & D3DCREATE_PUREDEVICE));
        if (pPresentationParameters) {
            HookLogImportant(
                "DX9: CreateDevice PP: %ux%u SwapEffect=%u Windowed=%d BackBufferFmt=%u BackBufferCount=%u",
                pPresentationParameters->BackBufferWidth, pPresentationParameters->BackBufferHeight,
                pPresentationParameters->SwapEffect, pPresentationParameters->Windowed,
                pPresentationParameters->BackBufferFormat, pPresentationParameters->BackBufferCount);
        }
    }

    static_assert(!ShouldPromoteClassicD3D9Device());
    HookLogImportant("DX9: Creating native classic D3D9 device; helper-owned shared capture will be probed later");
    HRESULT hr = oCreateDevice(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters,
                               ppReturnedDeviceInterface);

    if (SUCCEEDED(hr)) {
        if (pPresentationParameters) {
            int samples = (int)pPresentationParameters->MultiSampleType;
            if (samples > g_MaxMSAASamples.load()) {
                g_MaxMSAASamples.store(samples);
            }
            EarlyLog("DX9: CreateDevice SUCCESS: Final MSAA Type=%d, Quality=%d",
                     pPresentationParameters->MultiSampleType, pPresentationParameters->MultiSampleQuality);
        }
        if (ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
            EarlyLog("DX9: CreateDevice succeeded -> %p", *ppReturnedDeviceInterface);
            RegisterD3D9DeviceIdentity(*ppReturnedDeviceInterface, false, "IDirect3D9::CreateDevice");
            InstallDeviceHooks(*ppReturnedDeviceInterface, true);
        }
    }
    return hr;
}

// Hook: Direct3DCreate9 (Export)
typedef IDirect3D9*(WINAPI* Direct3DCreate9_t)(UINT SDKVersion);
static Direct3DCreate9_t oDirect3DCreate9 = nullptr;

static IDirect3D9* WINAPI DetourDirect3DCreate9(UINT SDKVersion) {
    EarlyLog("DX9: Direct3DCreate9 called (Intercepted)");

    // Keep the factory's public interface and internal runtime layout exactly as
    // requested by the application. Sharing is introduced only through private
    // capture resources after the native device exists.
    IDirect3D9* d3d9 = oDirect3DCreate9(SDKVersion);
    if (d3d9) {
        uintptr_t* vtable = *(uintptr_t**)d3d9;
        bool vtableValid = (vtable != nullptr) && (reinterpret_cast<uintptr_t>(vtable) >= 0x10000) &&
                           (reinterpret_cast<uintptr_t>(vtable) < 0x7FFFFFFF0000);
        if (vtable && vtableValid && !oCreateDevice) {
            if (VTableHook::Create(&vtable[16], (void*)&DetourCreateDevice, (void**)&oCreateDevice) ==
                VTableHook::Success) {
                EarlyLog("DX9: CreateDevice hook installed on native D3D9 factory");
            }
        }
        HookLogImportant("DX9: Returning native IDirect3D9 factory without Ex promotion");
    }
    return d3d9;
}

// Hook: IDirect3D9Ex::CreateDeviceEx
static HRESULT STDMETHODCALLTYPE DetourCreateDeviceEx(IDirect3D9Ex* self, UINT Adapter, D3DDEVTYPE DeviceType,
                                                      HWND hFocusWindow, DWORD BehaviorFlags,
                                                      D3DPRESENT_PARAMETERS* pPresentationParameters,
                                                      D3DDISPLAYMODEEX* pFullscreenDisplayMode,
                                                      IDirect3DDevice9Ex** ppReturnedDeviceInterface) {
    EarlyLog("DX9: CreateDeviceEx called (hFocusWindow=%p)", hFocusWindow);

    if (IsDX9InternalHelperBypassActive()) {
        const HRESULT hr = oCreateDeviceEx(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags,
                                           pPresentationParameters, pFullscreenDisplayMode, ppReturnedDeviceInterface);
        if (SUCCEEDED(hr) && ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
            DX9_RegisterInternalHelperDevice(*ppReturnedDeviceInterface);
        }
        return hr;
    }

    if (pPresentationParameters) {
        const auto& gfx = GetActiveGraphicsConfig();
        g_WindowedPresent = !!pPresentationParameters->Windowed;
        UINT originalInterval = pPresentationParameters->PresentationInterval;
        UINT originalBackBufferCount = pPresentationParameters->BackBufferCount;
        UINT originalRefresh = pPresentationParameters->FullScreen_RefreshRateInHz;
        EarlyLog("DX9: CreateDeviceEx: Requested MSAA Type=%d, Quality=%d", pPresentationParameters->MultiSampleType,
                 pPresentationParameters->MultiSampleQuality);

        VSyncOverride vsync = GetVSyncOverride();
        if (vsync.shouldOverride) {
            pPresentationParameters->PresentationInterval = (UINT)vsync.presentInterval;

            if (!pPresentationParameters->Windowed && vsync.presentInterval > 0 &&
                pPresentationParameters->FullScreen_RefreshRateInHz != 0) {
                static int logCount = 0;
                if (logCount++ < 10) {
                    HookLog(
                        "DX9: CreateDeviceEx: Clearing FullScreen_RefreshRateInHz "
                        "(was %u)",
                        pPresentationParameters->FullScreen_RefreshRateInHz);
                }
                pPresentationParameters->FullScreen_RefreshRateInHz = 0;
            }
        }

        // Backbuffer Count Override
        int count = gfx.backbufferCount;
        if (count >= 2 && count <= 6) {
            pPresentationParameters->BackBufferCount = (UINT)count - 1;
            HookLog("DX9: CreateDeviceEx: Overriding BackBufferCount to %d", count);
        }

        // Log DX9 overrides at debug level only (not important level).
        // Many games create a DX9 device for intro videos but render on DX12,
        // making this log misleading. The overrides still apply to the DX9 device.
        HookLog(
            "DX9: CreateDeviceEx overrides vsync=%s interval %u->%u refresh %u->%u "
            "backbufferCfg=%d d3dBackBufferCount %u->%u prerender=%.2f",
            gfx.vsyncMode.c_str(), originalInterval, pPresentationParameters->PresentationInterval, originalRefresh,
            pPresentationParameters->FullScreen_RefreshRateInHz, gfx.backbufferCount, originalBackBufferCount,
            pPresentationParameters->BackBufferCount, gfx.cpuPrerenderLimit);

        HookLogImportant("DX9: CreateDeviceEx flags preserved (multithreaded=%d pure=%d)",
                         !!(BehaviorFlags & D3DCREATE_MULTITHREADED), !!(BehaviorFlags & D3DCREATE_PUREDEVICE));
    }

    HRESULT hr = oCreateDeviceEx(self, Adapter, DeviceType, hFocusWindow, BehaviorFlags, pPresentationParameters,
                                 pFullscreenDisplayMode, ppReturnedDeviceInterface);
    if (SUCCEEDED(hr)) {
        if (pPresentationParameters) {
            int samples = (int)pPresentationParameters->MultiSampleType;
            if (samples > g_MaxMSAASamples.load()) {
                g_MaxMSAASamples.store(samples);
            }
            EarlyLog("DX9: CreateDeviceEx SUCCESS: Final MSAA Type=%d, Quality=%d",
                     pPresentationParameters->MultiSampleType, pPresentationParameters->MultiSampleQuality);
        }
        if (ppReturnedDeviceInterface && *ppReturnedDeviceInterface) {
            EarlyLog("DX9: CreateDeviceEx succeeded -> %p", *ppReturnedDeviceInterface);
            RegisterD3D9DeviceIdentity(*ppReturnedDeviceInterface, true, "IDirect3D9Ex::CreateDeviceEx");
            InstallDeviceHooks(*ppReturnedDeviceInterface, true);
        }
    }
    return hr;
}

// Hook: Direct3DCreate9Ex (Export)
static Direct3DCreate9Ex_t oDirect3DCreate9Ex = nullptr;

static HRESULT WINAPI DetourDirect3DCreate9Ex(UINT SDKVersion, IDirect3D9Ex** ppOut) {
    EarlyLog("DX9: Direct3DCreate9Ex called (Intercepted)");
    HRESULT hr = oDirect3DCreate9Ex(SDKVersion, ppOut);
    if (SUCCEEDED(hr) && ppOut && *ppOut) {
        uintptr_t* vtable = *(uintptr_t**)*ppOut;

        // Hook CreateDevice (16)
        if (!oCreateDevice) {
            if (VTableHook::Create(&vtable[16], (void*)&DetourCreateDevice, (void**)&oCreateDevice) ==
                VTableHook::Success) {
                EarlyLog("DX9: IDirect3D9::CreateDevice hook installed via Create9Ex");
            }
        }

        // Hook CreateDeviceEx (20)
        if (!oCreateDeviceEx) {
            if (VTableHook::Create(&vtable[20], (void*)&DetourCreateDeviceEx, (void**)&oCreateDeviceEx) ==
                VTableHook::Success) {
                EarlyLog("DX9: IDirect3D9Ex::CreateDeviceEx hook installed via Create9Ex");
            }
        }
    }
    return hr;
}

void DX9Hook::Init() {
    auto LogDirect = [](const char* fmt, ...) {
        va_list args;
        va_start(args, fmt);
        char buf[1024];
        vsnprintf(buf, sizeof(buf), fmt, args);
        va_end(args);
        EarlyLog("%s", buf);
    };

    LogDirect("=== DX9Hook::Init() START ===");

    HMODULE d3d9Module = GetModuleHandleA("d3d9.dll");
    LogDirect("d3d9.dll = %p", (void*)d3d9Module);

    if (!d3d9Module) {
        LogDirect("DX9: d3d9.dll not loaded, returning");
        return;
    }

    LogDirect("Calling InstallD3D9InlineHooks...");
    bool inlineResult = InstallD3D9InlineHooks();
    LogDirect("InstallD3D9InlineHooks returned %d", inlineResult ? 1 : 0);

    // Hook Export Functions
    // Using IAT hooking (in iat_hook.cpp) or active VTable hooking for DX9.

    // Check if Direct3DCreate9(Ex) are available for active hooking fallback
    void* pD3DCreate9 = (void*)GetProcAddress(d3d9Module, "Direct3DCreate9");
    void* pD3DCreate9Ex = (void*)GetProcAddress(d3d9Module, "Direct3DCreate9Ex");

    // Install inline hook on Direct3DCreate9 so ALL callers (main exe + any
    // middleware DLL) are intercepted, regardless of which module calls it.
    // This lets DetourDirect3DCreate9 hook CreateDevice on the returned factory.
    // Note: oDirect3DCreate9 may already be set by the IAT hook; we overwrite it
    // with the trampoline so calling it doesn't re-enter the inline hook.
    static bool s_direct3DCreate9InlineInstalled = false;
    if (pD3DCreate9 && !s_direct3DCreate9InlineInstalled) {
        void* trampoline = nullptr;
        if (InlineHook::Install(pD3DCreate9, (void*)DetourDirect3DCreate9, &trampoline)) {
            oDirect3DCreate9 = (Direct3DCreate9_t)trampoline;
            s_direct3DCreate9InlineInstalled = true;
            EarlyLog("DX9: Direct3DCreate9 inline hook installed (trampoline=%p)", trampoline);
        } else {
            EarlyLog("DX9: Direct3DCreate9 inline hook failed");
        }
    }

    // Hook CreateDevice on the plain IDirect3D9 vtable.  This is critical for
    // late injection: the game may have already called Direct3DCreate9() and holds
    // a plain IDirect3D9 whose vtable is DIFFERENT from IDirect3D9Ex.  By creating
    // a temporary IDirect3D9 and hooking its vtable, we intercept CreateDevice on
    // ALL plain IDirect3D9 instances (vtable is shared across all instances of the
    // same COM class).
    if (!oCreateDevice && pD3DCreate9) {
        // Use the trampoline (bypasses our inline hook) if available, else raw address
        typedef IDirect3D9*(WINAPI * PFN_Create9)(UINT);
        PFN_Create9 pfnCreate9 = oDirect3DCreate9 ? (PFN_Create9)oDirect3DCreate9 : (PFN_Create9)pD3DCreate9;
        IDirect3D9* dummyD3D9 = pfnCreate9(D3D_SDK_VERSION);
        if (dummyD3D9) {
            uintptr_t* vtable = *(uintptr_t**)dummyD3D9;
            bool vtableValid = (vtable != nullptr) && (reinterpret_cast<uintptr_t>(vtable) >= 0x10000) &&
                               (reinterpret_cast<uintptr_t>(vtable) < 0x7FFFFFFF0000);
            if (vtable && vtableValid) {
                VTableHook::Create(&vtable[16], (void*)&DetourCreateDevice, (void**)&oCreateDevice);
                EarlyLog("DX9: Plain IDirect3D9::CreateDevice hooked (vtable=%p)", (void*)vtable);
            }
            dummyD3D9->Release();
        } else {
            EarlyLog("DX9: Failed to create dummy IDirect3D9 for vtable hook");
        }
    }

    LogDirect("DX9Hook::Init() Passive Complete");

    // Check for test apps that force DX9 but might load other DLLs
    bool isTestApp = false;
    char modPath[MAX_PATH] = {};
    if (GetModuleFileNameA(nullptr, modPath, MAX_PATH)) {
        const char* exeName = strrchr(modPath, '\\');
        exeName = exeName ? exeName + 1 : modPath;
        if (strnicmp(exeName, "dx9_test", 8) == 0)
            isTestApp = true;
    }

    // Skip Active Hooking if a different graphics API is the primary renderer
    const char* skipReason = nullptr;
    if (GetModuleHandleA("d3d12.dll") && !isTestApp) {
        skipReason = "d3d12.dll (DX12 game)";
    } else if ((GetModuleHandleA("d3d11.dll") || GetModuleHandleA("d3d10.dll") || GetModuleHandleA("d3d10_1.dll")) &&
               !isTestApp) {
        // DX11/DX10 usually implies D3D11/D3D10 is primary, unless it's a test app
        skipReason = GetModuleHandleA("d3d11.dll") ? "d3d11.dll (DX11 game)" : "d3d10.dll (DX10 game)";
    } else if (GetModuleHandleA("vulkan-1.dll") && !isTestApp) {
        skipReason = "vulkan-1.dll (Vulkan game)";
    }

    // Note: opengl32.dll check removed. Many DX9 games load it but don't use it.
    // We want active init to ensure reliable hooking even in those cases.

    const bool inlineHooksReady = g_InlineHooksInstalled.load(std::memory_order_acquire);

    LogDirect("skipReason=%s, inlineHooksReady=%d, oPresent=%p", skipReason ? skipReason : "null",
              inlineHooksReady ? 1 : 0, (void*)oPresent);

    if (skipReason) {
        LogDirect("DX9: Skipping active init (%s, inlineHooksReady=%d)", skipReason, inlineHooksReady ? 1 : 0);
        return;
    }

    // CRITICAL: If inline hooks failed, try to find existing D3D9 devices FIRST
    // This is needed for late injection when the game already created its device
    // and another overlay has hooked d3d9.dll functions (blocking our inline hooks)
    // We must do this BEFORE creating a dummy device, because dummy device VTable
    // hooks won't affect the game's real device (each device has its own VTable copy)
    // DISABLED - scanner finds false positives and causes crashes
    // if (!inlineHooksReady && !oPresent) {
    //   LogDirect("DX9: Inline hooks failed, scanning for existing D3D9 devices...");
    //   ScanForExistingD3D9Devices();
    //
    //   if (oPresent) {
    //     LogDirect("DX9: Successfully hooked existing device via scanner!");
    //   } else {
    //     LogDirect("DX9: Scanner found no devices, will create dummy device");
    //   }
    // }

    // If we still don't have hooks, try active hooking with a dummy device
    // This is a fallback for cases where no device exists yet (early injection)
    // or the scanner failed to find the game's device
    LogDirect("Checking oPresent=%p for dummy device creation...", (void*)oPresent);

    if (!oPresent) {
        LogDirect("DX9: Creating dummy device for VTable hooks...");

        // Active Hooking: Create a dummy device to force vtable hooks
        // This is needed for "early" injection where the game hasn't created its device yet

        // 1. Create a specific window class for our dummy window
        WNDCLASSEXA wc = {0};
        wc.cbSize = sizeof(wc);
        wc.style = CS_CLASSDC;
        wc.lpfnWndProc = DefWindowProcA;
        wc.hInstance = GetModuleHandle(NULL);
        wc.lpszClassName = "DX9Hook_Dummy";
        RegisterClassExA(&wc);

        HWND hWnd = CreateWindowA("DX9Hook_Dummy", "DX9 Dummy", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL,
                                  wc.hInstance, NULL);

        LogDirect("Dummy window created: hWnd=%p", (void*)hWnd);

        if (hWnd && d3d9Module) {
            // Try Direct3DCreate9Ex first
            if (pD3DCreate9Ex) {
                LogDirect("Trying Direct3DCreate9Ex...");
                typedef HRESULT(WINAPI * Direct3DCreate9Ex_t)(UINT, IDirect3D9Ex**);
                Direct3DCreate9Ex_t create9Ex = (Direct3DCreate9Ex_t)pD3DCreate9Ex;
                IDirect3D9Ex* d3d9ex = nullptr;

                if (SUCCEEDED(create9Ex(D3D_SDK_VERSION, &d3d9ex))) {
                    LogDirect("Direct3DCreate9Ex succeeded, creating device...");
                    D3DPRESENT_PARAMETERS pp = {0};
                    pp.Windowed = TRUE;
                    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
                    pp.hDeviceWindow = hWnd;

                    IDirect3DDevice9Ex* deviceEx = nullptr;
                    if (SUCCEEDED(d3d9ex->CreateDeviceEx(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
                                                         D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, NULL, &deviceEx))) {
                        LogDirect("D3D9Ex device created, calling InstallDeviceHooks...");
                        InstallDeviceHooks(deviceEx, true);
                        LogDirect("InstallDeviceHooks returned, oPresent=%p", (void*)oPresent);
                        deviceEx->Release();
                    }
                    d3d9ex->Release();
                }
            }

            // Fallback to Direct3DCreate9 if Ex failed or wasn't tried, AND hooks are
            // not fully installed (InstallDeviceHooks checks for oPresent/oReset
            // internally)
            if ((!oPresent || !oReset) && pD3DCreate9) {
                LogDirect("Trying Direct3DCreate9 fallback...");
                typedef IDirect3D9*(WINAPI * Direct3DCreate9_t)(UINT);
                Direct3DCreate9_t create9 = (Direct3DCreate9_t)pD3DCreate9;
                IDirect3D9* d3d9 = create9(D3D_SDK_VERSION);

                if (d3d9) {
                    LogDirect("Direct3DCreate9 succeeded, creating device...");
                    D3DPRESENT_PARAMETERS pp = {0};
                    pp.Windowed = TRUE;
                    pp.SwapEffect = D3DSWAPEFFECT_DISCARD;
                    pp.hDeviceWindow = hWnd;

                    IDirect3DDevice9* device = nullptr;
                    if (SUCCEEDED(d3d9->CreateDevice(D3DADAPTER_DEFAULT, D3DDEVTYPE_HAL, hWnd,
                                                     D3DCREATE_SOFTWARE_VERTEXPROCESSING, &pp, &device))) {
                        LogDirect("D3D9 device created, calling InstallDeviceHooks...");
                        InstallDeviceHooks(device, true);
                        LogDirect("InstallDeviceHooks returned, oPresent=%p", (void*)oPresent);
                        device->Release();
                    }
                    d3d9->Release();
                }
            }
        }

        if (hWnd) {
            DestroyWindow(hWnd);
            UnregisterClassA("DX9Hook_Dummy", wc.hInstance);
        }
    }

    LogDirect("DX9Hook::Init() complete (inlineHooks=%d, oPresent=%p, oReset=%p)",
              g_InlineHooksInstalled.load() ? 1 : 0, (void*)oPresent, (void*)oReset);
}

void DX9Hook::Shutdown() {
    EarlyLog("DX9Hook::Shutdown()");
    ce::dx9_sampler_state::LogSummary();

    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    g_DX9Capture.ForceCleanup();
}

void DX9Hook::OnHostDisconnect() {
    EarlyLog("DX9Hook::OnHostDisconnect()");
    g_DX9Capture.ForceCleanup();
}
