        ReleaseResize();
        return hr;
    }

    if (api == APIType::D3D12)
        HandleDX12ResizeBegin();
    else if (api == APIType::D3D11)
        HandleDX11ResizeBegin();

    HookLog("DXGI: ResizeBuffers - calling oResizeBuffers...");
    HRESULT hr = oResizeBuffers(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
    HookLog("DXGI: ResizeBuffers - oResizeBuffers returned hr=0x%08X", hr);

    if (FAILED(hr)) {
        HookLog("DXGI: ResizeBuffers FAILED with 0x%08X", hr);
    } else {
        HookLog("DXGI: ResizeBuffers SUCCESS");
    }

    // Reset resize flags after resize completes
    if (api == APIType::D3D12) {
        HookLog("DXGI: ResizeBuffers - calling HandleDX12ResizeEnd...");
        HandleDX12ResizeEnd();
        HookLog("DXGI: ResizeBuffers - HandleDX12ResizeEnd returned");
    }

    g_SharedState.swapchainInvalid.store(false);
    g_SharedState.wrapperResizeDepth.fetch_sub(1);
    ReleaseResize();
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourResizeBuffers1(IDXGISwapChain* pSwapChain, UINT BufferCount, UINT Width, UINT Height,
                                               DXGI_FORMAT NewFormat, UINT SwapChainFlags,
                                               const UINT* pCreationNodeMask, IUnknown* const* ppPresentQueue) {
    // Apply backbuffer count override from config
    {
        const auto& cfg = GetActiveGraphicsConfig();
        if (HasBackbufferCountOverride(cfg.backbufferCount)) {
            UINT requested = static_cast<UINT>(cfg.backbufferCount);
            if (requested > 0 && requested != BufferCount) {
                DXGI_SWAP_CHAIN_DESC scDesc = {};
                bool canOverride = true;
                if (SUCCEEDED(pSwapChain->GetDesc(&scDesc))) {
                    bool isFlip = (scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                                   scDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
                    if (isFlip && requested < BufferCount) {
                        SwapChainFlags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
                        canOverride = false;
                        HookLog("DetourResizeBuffers1: Skipping BufferCount override %u < game's %u (flip model)",
                                requested, BufferCount);
                    }
                }
                if (canOverride) {
                    HookLogImportant("DetourResizeBuffers1: Overriding BufferCount %u -> %u", BufferCount, requested);
                    BufferCount = requested;
                }
            }
        }
    }

    // Vulkan passthrough
    if (IsVulkanActive()) {
        return oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask,
                               ppPresentQueue);
    }

    // AGGRESSIVE RECURSION GUARD: Steam overlay causes infinite recursion through
    // hook chain
    if (IsRecursiveResize()) {
        // Recursion detected - call original directly through vtable to bypass
        // Steam's hook
        void** vtable = *(void***)pSwapChain;
        typedef HRESULT(STDMETHODCALLTYPE * PFN_ResizeBuffers1)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT,
                                                                const UINT*, IUnknown* const*);
        PFN_ResizeBuffers1 originalResize1 = (PFN_ResizeBuffers1)vtable[39];  // ResizeBuffers1 is at index 39
        return originalResize1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask,
                               ppPresentQueue);
    }

    if (g_SharedState.wrapperResizeDepth.fetch_add(1) > 0) {
        HRESULT hr = oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags,
                                     pCreationNodeMask, ppPresentQueue);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    // Check if this is our wrapper swapchain - if so, skip resize handling
    void* pWrapperTest = nullptr;
    if (SUCCEEDED(pSwapChain->QueryInterface(IID_CWrapDXGISwapChain, &pWrapperTest))) {
        ((IUnknown*)pWrapperTest)->Release();
        HRESULT hr = oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags,
                                     pCreationNodeMask, ppPresentQueue);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    g_SharedState.swapchainInvalid.store(true);

    APIType api = DetectAPIType(pSwapChain);

    // CRITICAL FIX: Skip DX12 resize handling during initial swapchain creation
    // Some games call ResizeBuffers immediately after CreateSwapChain
    static std::atomic<int> s_initialResizeCount{0};
    if (api == APIType::D3D12 && s_initialResizeCount.fetch_add(1) == 0) {
        HookLog("DXGI: ResizeBuffers - FIRST D3D12 resize, direct vtable call");
        // Call directly through vtable to bypass any hook chain issues
        void** vtable = *(void***)pSwapChain;
        typedef HRESULT(STDMETHODCALLTYPE * PFN_ResizeBuffers)(IDXGISwapChain*, UINT, UINT, UINT, DXGI_FORMAT, UINT);
        PFN_ResizeBuffers originalResize = (PFN_ResizeBuffers)vtable[13];
        HRESULT hr = originalResize(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags);
        HookLog("DXGI: ResizeBuffers - first D3D12 resize returned hr=0x%08X", hr);
        g_SharedState.wrapperResizeDepth.fetch_sub(1);
        ReleaseResize();
        return hr;
    }

    if (api == APIType::D3D12)
        HandleDX12ResizeBegin();
    else if (api == APIType::D3D11)
        HandleDX11ResizeBegin();

    HRESULT hr = oResizeBuffers1(pSwapChain, BufferCount, Width, Height, NewFormat, SwapChainFlags, pCreationNodeMask,
                                 ppPresentQueue);

    if (FAILED(hr)) {
        HookLog("DXGI: ResizeBuffers1 FAILED with 0x%08X", hr);
    } else {
        HookLog("DXGI: ResizeBuffers1 SUCCESS");
    }

    // Reset resize flags after resize completes
    if (api == APIType::D3D12)
        HandleDX12ResizeEnd();

    g_SharedState.swapchainInvalid.store(false);
    g_SharedState.wrapperResizeDepth.fetch_sub(1);
    ReleaseResize();
    return hr;
}

HRESULT STDMETHODCALLTYPE DetourSetColorSpace1(IDXGISwapChain* pSwapChain, DXGI_COLOR_SPACE_TYPE colorSpace) {
    const PFN_SetColorSpace1 original = oSetColorSpace1Trampoline.load(std::memory_order_acquire);
    if (!original) {
        static std::atomic<int> s_missingTrampolineLogCount{0};
        if (s_missingTrampolineLogCount.fetch_add(1, std::memory_order_relaxed) < 5) {
            HookLogImportant(
                "DXGI: SetColorSpace1 detour entered without a published trampoline; failing closed sc=%p cs=%d",
                pSwapChain, static_cast<int>(colorSpace));
        }
        return DXGI_ERROR_UNSUPPORTED;
    }

    const HRESULT result = original(pSwapChain, colorSpace);
    if (SUCCEEDED(result) &&
        ce::presentation_color::ShouldRecordDetouredColorSpaceChange(s_wrapperSetColorSpaceForwardDepth)) {
        bool changed = false;
        if (RecordSwapChainColorSpace(pSwapChain, colorSpace, &changed) && changed) {
            HookLogImportant("DXGI: Swapchain presentation color space changed source=inline sc=%p cs=%d",
                             pSwapChain, static_cast<int>(colorSpace));
        }
    }
    return result;
}

HRESULT SetSwapChainColorSpaceFromWrapper(IDXGISwapChain3* callableSwapChain, IDXGISwapChain* identitySwapChain,
                                          DXGI_COLOR_SPACE_TYPE colorSpace) {
    if (!callableSwapChain) {
        return DXGI_ERROR_UNSUPPORTED;
    }

    ++s_wrapperSetColorSpaceForwardDepth;
    const auto depthGuard = ce::make_scope_guard([]() { --s_wrapperSetColorSpaceForwardDepth; });
    const HRESULT result = callableSwapChain->SetColorSpace1(colorSpace);
    if (SUCCEEDED(result)) {
        IDXGISwapChain* identity = identitySwapChain ? identitySwapChain : callableSwapChain;
        bool changed = false;
        if (RecordSwapChainColorSpace(identity, colorSpace, &changed) && changed) {
            HookLogImportant("DXGI: Swapchain presentation color space changed source=wrapper sc=%p cs=%d",
                             identity, static_cast<int>(colorSpace));
        }
    }
    return result;
}

static void PublishSetColorSpace1Trampoline(void* trampoline, void*) {
    oSetColorSpace1Trampoline.store(reinterpret_cast<PFN_SetColorSpace1>(trampoline), std::memory_order_release);
}

static bool InstallSetColorSpace1InlineHook(IDXGISwapChain* pSwapChain, const char* source) {
    if (!pSwapChain) {
        return false;
    }
    if (oSetColorSpace1Trampoline.load(std::memory_order_acquire)) {
        return true;
    }

    // Inline-hook the real DXGI implementation only. Hooking the wrapper method
    // would make the detour's trampoline call back into the wrapper and recreate
    // the unsafe wrapper/detour composition that this path is designed to avoid.
    if (IsWrappedSwapChainObject(pSwapChain)) {
        static std::atomic<int> s_wrapperTargetLogCount{0};
        if (s_wrapperTargetLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLog("DXGI: SetColorSpace1 inline tracking skipped for wrapped %s swapchain %p",
                    source ? source : "unknown", pSwapChain);
        }
        return false;
    }

    std::lock_guard<std::mutex> installLock(s_setColorSpace1HookMutex);
    if (oSetColorSpace1Trampoline.load(std::memory_order_acquire)) {
        return true;
    }

    IDXGISwapChain3* colorSpaceSwapChain = nullptr;
    if (FAILED(pSwapChain->QueryInterface(IID_PPV_ARGS(&colorSpaceSwapChain))) || !colorSpaceSwapChain) {
        static std::atomic<int> s_unsupportedLogCount{0};
        if (s_unsupportedLogCount.fetch_add(1, std::memory_order_relaxed) < 3) {
            HookLog("DXGI: SetColorSpace1 tracking unavailable for %s swapchain %p (IDXGISwapChain3 unsupported)",
                    source ? source : "unknown", pSwapChain);
        }
        return false;
    }

    void** colorSpaceVtable = *reinterpret_cast<void***>(colorSpaceSwapChain);
    void* colorSpaceAddress =
        colorSpaceVtable && IsReadableMemory(reinterpret_cast<const void*>(&colorSpaceVtable[38]), sizeof(void*)) ? colorSpaceVtable[38] : nullptr;
    colorSpaceSwapChain->Release();
    if (!colorSpaceAddress || colorSpaceAddress == reinterpret_cast<void*>(DetourSetColorSpace1)) {
        HookLogImportant("DXGI: Refusing unsafe SetColorSpace1 hook target source=%s sc=%p target=%p",
                         source ? source : "unknown", pSwapChain, colorSpaceAddress);
        return false;
    }

    void* colorSpaceTrampoline = nullptr;
    if (!InlineHook::InstallPublished(colorSpaceAddress, reinterpret_cast<void*>(DetourSetColorSpace1),
                                      &colorSpaceTrampoline, PublishSetColorSpace1Trampoline, nullptr)) {
        if (oSetColorSpace1Trampoline.load(std::memory_order_acquire)) {
            return true;
        }
        HookLogImportant(
            "DXGI: SetColorSpace1 inline tracking unavailable source=%s sc=%p target=%p; wrapper tracking remains available",
            source ? source : "unknown", pSwapChain, colorSpaceAddress);
        return false;
    }

    HookLogImportant("DXGI: SetColorSpace1 inline tracking installed source=%s target=%p trampoline=%p",
                     source ? source : "unknown", colorSpaceAddress, colorSpaceTrampoline);
    return true;
}

bool InstallHooks(IDXGISwapChain* pSwapChain, bool presentOnly) {
    // NOTE: This function should only be called for DX11/DX10 games.
    // DX12 games use wrapper-based Present interception (CWrapDXGISwapChain).
    // Calling this for DX12 can cause conflicts and stack overflow crashes
    // due to two competing Present interception mechanisms.
    // See: dx12_hook.cpp for the wrapper-based approach.

    if (!pSwapChain)
        return false;

    static std::atomic<int> s_installCount{0};
    int count = s_installCount.fetch_add(1);
    HookLog("DXGIShared::InstallHooks CALLED #%d (swapchain=%p, presentOnly=%d)", count, pSwapChain,
            presentOnly ? 1 : 0);

    InstallSetColorSpace1InlineHook(pSwapChain, "vtable-path");

    // Third-party overlays can install their own DXGI hooks and form recursive
    // Present chains with vtable patching. In that case the wrapper-based path
    // remains active and avoids hook wars.
    if (!DXGIShared::ShouldInstallSwapchainHooksWithThirdPartyOverlay(IsThirdPartyOverlayLoaded(),
                                                                      HasPresentDetourHooks())) {
        HookLog("DXGIShared::InstallHooks: External overlay detected, skipping DXGI swapchain hooks");
        return true;
    }

    if (s_hookedVTable) {
        void** newVTable = *(void***)pSwapChain;
        if (newVTable == s_hookedVTable) {
            HookLog("DXGIShared::InstallHooks: Hooks already installed on vtable %p", s_hookedVTable);
            return true;
        }
        // New swapchain with a DIFFERENT vtable — need to re-hook.
        HookLogImportant("DXGIShared::InstallHooks: NEW vtable detected (old=%p new=%p) — re-hooking", s_hookedVTable,
                         newVTable);
    }

    void** vtable = *(void***)pSwapChain;
    if (!vtable) {
        HookLog("DXGIShared::InstallHooks: Invalid vtable");
        return false;
    }

    DWORD oldProtect;
    if (!VirtualProtect(reinterpret_cast<void*>(vtable), 40 * sizeof(void*), PAGE_READWRITE, &oldProtect)) {
        HookLog("DXGIShared::InstallHooks: VirtualProtect failed");
        return false;
    }

    s_hookedVTable = vtable;

    oPresent = (PFN_Present)vtable[8];
    vtable[8] = (void*)DetourPresent;
    HookLog("DXGIShared: Hooked Present at vtable[8] (original=%p, detour=%p)", oPresent, DetourPresent);

    oPresent1 = (PFN_Present1)vtable[22];
    vtable[22] = (void*)DetourPresent1;
    HookLog("DXGIShared: Hooked Present1 at vtable[22] (original=%p, detour=%p)", oPresent1, DetourPresent1);

    if (!presentOnly) {
        oResizeBuffers = (PFN_ResizeBuffers)vtable[13];
        vtable[13] = (void*)DetourResizeBuffers;
        HookLog(
            "DXGIShared: Hooked ResizeBuffers at vtable[13] (original=%p, "
            "detour=%p)",
            oResizeBuffers, DetourResizeBuffers);

        oResizeBuffers1 = (PFN_ResizeBuffers1)vtable[39];
        vtable[39] = (void*)DetourResizeBuffers1;
        HookLog(
            "DXGIShared: Hooked ResizeBuffers1 at vtable[39] (original=%p, "
            "detour=%p)",
            oResizeBuffers1, DetourResizeBuffers1);
    }

    VirtualProtect(reinterpret_cast<void*>(vtable), 40 * sizeof(void*), oldProtect, &oldProtect);
    HookLog("DXGIShared::InstallHooks: All vtable hooks installed successfully");
    return true;
}

bool HasPresentInlineHooks() {
    return oPresentTrampoline != nullptr || oPresent1Trampoline != nullptr;
}

bool HasPresentDetourHooks() {
    return s_hookedVTable != nullptr || oPresentTrampoline != nullptr || oPresent1Trampoline != nullptr;
}

bool CanSafelyInstallExternalPresentDetourPath(bool requiresBypassTrampoline, bool bypassTrampolineAvailable) {
    return !requiresBypassTrampoline || bypassTrampolineAvailable;
}

// Lazy hook installation - installs hooks on first Present if they were
// deferred during swapchain creation
static IDXGISwapChain* s_PendingSwapChainForLazyHook = nullptr;
static std::atomic<bool> s_LazyHooksInstalled{false};

void SetPendingSwapChainForLazyHook(IDXGISwapChain* pSwapChain) {
    if (pSwapChain) {
        pSwapChain->AddRef();
    }
    if (s_PendingSwapChainForLazyHook) {
        s_PendingSwapChainForLazyHook->Release();
    }
    s_PendingSwapChainForLazyHook = pSwapChain;
    HookLog("DXGIShared: SetPendingSwapChainForLazyHook called");
}

static void InstallHooksIfPending(IDXGISwapChain* pSwapChain) {
    if (s_LazyHooksInstalled.load(std::memory_order_acquire))
        return;

    // Check if this is the pending swapchain
    if (pSwapChain == s_PendingSwapChainForLazyHook) {
        HookLog("DXGIShared: Installing hooks lazily on first Present");
        // CRITICAL: Use presentOnly=true to only hook Present/Present1
        // ResizeBuffers hooks can cause stack overflow crashes with some overlays
        InstallHooks(pSwapChain, true);
        s_LazyHooksInstalled.store(true, std::memory_order_release);
        if (s_PendingSwapChainForLazyHook) {
            s_PendingSwapChainForLazyHook->Release();
            s_PendingSwapChainForLazyHook = nullptr;
        }
    }
}

void Init() {
    g_SharedState.lastSwapchainCreation = std::chrono::steady_clock::now();
    // Early detection of NVIDIA Smooth Motion module
    g_FGCompat.CheckForNvPresent();
}

bool InstallPresentInlineHooks(IDXGISwapChain* pSwapChain) {
    if (!pSwapChain)
        return false;

    InstallSetColorSpace1InlineHook(pSwapChain, "present-bootstrap");

    void* presentAddr = GetPresentAddress(pSwapChain);
    void* present1Addr = GetPresent1Address(pSwapChain);

    if (!presentAddr) {
        HookLog("InstallPresentInlineHooks: Failed to get Present address");
        return false;
    }

    // Save original vtable[8] before any modifications. This captures the real
    // COM method (dxgi!CDXGISwapChain::Present or equivalent) from the temp
    // swapchain, before CE patches it to DetourPresent. Used later in
    // CallOriginalPresent and AttemptSteamDX12OverlayInit to ensure DXGI COM
    // method state management runs before dxgi!Present is called with Steam's
    // E9 JMP.
    {
        void** vtable = *(void***)pSwapChain;
        if (vtable && IsReadableMemory(reinterpret_cast<const void*>(&vtable[8]), sizeof(void*))) {
            if (!s_originalVtable8Present) {
                s_originalVtable8Present = (PFN_Present)vtable[8];
                // Log the saved address and compare with GetPresentAddress
                HookLogImportant(
                    "InstallPresentInlineHooks: Saved s_originalVtable8Present=%p from temp swapchain %p "
                    "(presentAddr=%p, same=%d)",
                    (void*)s_originalVtable8Present, (void*)pSwapChain, presentAddr,
                    s_originalVtable8Present == (PFN_Present)presentAddr ? 1 : 0);
                // Log which module presentAddr belongs to for debugging
                HMODULE hAddrModule = nullptr;
                if (GetModuleHandleExA(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCSTR)presentAddr, &hAddrModule)) {
                    char modulePath[MAX_PATH] = {};
                    GetModuleFileNameA(hAddrModule, modulePath, sizeof(modulePath));
                    HookLogImportant("InstallPresentInlineHooks: presentAddr=%p is in module: %s", presentAddr,
                                     modulePath[0] ? modulePath : "(unknown)");
                }
            }
        } else {
            HookLog("InstallPresentInlineHooks: Cannot read vtable[8] from temp swapchain %p", (void*)pSwapChain);
        }
    }

    static bool s_inlineHooksInstalled = false;
    if (s_inlineHooksInstalled) {
        HookLog("InstallPresentInlineHooks: Inline hooks already installed");
        return true;
    }

    // CRITICAL: Check if an external overlay has already hooked Present
    // External overlays (NVIDIA, Steam, Discord, etc.) actively re-hook Present
    // Fighting them causes a hook war that corrupts the call chain
    const uint8_t* code = (const uint8_t*)presentAddr;
    bool externalJmpDetected = false;

    if (code[0] == 0xE9) {
        // JMP rel32 detected - check where it points
        int32_t disp;
        memcpy(&disp, code + 1, 4);
        uintptr_t jumpTarget = (uintptr_t)(code + 5) + disp;

        // Check if the jump target is outside dxgi.dll
        HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
        if (hDXGI) {
            MODULEINFO dxgiInfo;
            if (GetModuleInformation(GetCurrentProcess(), hDXGI, &dxgiInfo, sizeof(dxgiInfo))) {
                uintptr_t dxgiStart = (uintptr_t)hDXGI;
                uintptr_t dxgiEnd = dxgiStart + dxgiInfo.SizeOfImage;

                if (jumpTarget < dxgiStart || jumpTarget >= dxgiEnd) {
                    externalJmpDetected = true;
                    // JMP points outside dxgi.dll - external overlay detected
                    HookLog("InstallPresentInlineHooks: External overlay detected!");
                    HookLog("InstallPresentInlineHooks: JMP at %p targets %p (outside dxgi.dll %p-%p)", presentAddr,
                            (void*)jumpTarget, (void*)dxgiStart, (void*)dxgiEnd);

                    // Check if the target module is a known overlay
                    HMODULE hTargetModule = nullptr;
                    GetModuleHandleExA(
                        GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        (LPCSTR)jumpTarget, &hTargetModule);

                    if (hTargetModule) {
                        char moduleName[MAX_PATH] = {0};
                        GetModuleFileNameA(hTargetModule, moduleName, MAX_PATH);
                        HookLog("InstallPresentInlineHooks: External overlay module: %s", moduleName);

                        // Known overlay modules that we should cooperate with
                        std::string moduleLower = moduleName;
                        std::transform(moduleLower.begin(), moduleLower.end(), moduleLower.begin(),
                                       [](unsigned char ch) { return static_cast<char>(std::tolower(ch)); });

                        if (moduleLower.find("nvidia") != std::string::npos ||
                            moduleLower.find("nvngx") != std::string::npos ||
                            moduleLower.find("steam") != std::string::npos ||
                            moduleLower.find("gameoverlay") != std::string::npos ||
                            moduleLower.find("discord") != std::string::npos ||
                            moduleLower.find("overlay") != std::string::npos) {
                            HookLog("InstallPresentInlineHooks: External hook detected - cooperating (known overlay)");
                        }
                    } else {
                        // Skip inline hooks to prevent prologue corruption (black screen fix)
                        HookLog("InstallPresentInlineHooks: Unknown external JMP - possibly stale hook");
                    }
                }
            }
        }
    }

    if (externalJmpDetected) {
#ifdef _WIN64
        constexpr bool kRequiresBypassTrampolineOnInstall = false;
#else
        constexpr bool kRequiresBypassTrampolineOnInstall = true;
#endif

        void* presentBypass = InlineHook::CreateBypassTrampoline(presentAddr);
        if (!presentBypass) {
            HookLog("InstallPresentInlineHooks: WARNING - Failed to create Present bypass trampoline");
            if (!CanSafelyInstallExternalPresentDetourPath(kRequiresBypassTrampolineOnInstall, false)) {
                HookLogImportant(
                    "InstallPresentInlineHooks: External Present hook detected but no bypass trampoline is available - "
                    "skipping DXGI Present detour path");
                return false;
            }
        }

        void* present1Bypass = nullptr;
        if (present1Addr) {
            present1Bypass = InlineHook::CreateBypassTrampoline(present1Addr);
            if (!present1Bypass &&
                !CanSafelyInstallExternalPresentDetourPath(kRequiresBypassTrampolineOnInstall, false)) {
                HookLogImportant(
                    "InstallPresentInlineHooks: External Present1 hook detected but no bypass trampoline is available "
                    "- skipping DXGI Present detour path");
                return false;
            }
        }

        HookLogImportant("InstallPresentInlineHooks: External E9 JMP detected — using vtable hook path");
        // Save the external overlay hook target (Steam's OverlayHookD3D3) so we
        // can invoke it explicitly later when SL FG routing bypasses Steam's JMP.
        // This is done BEFORE SL overwrites the JMP with its own.
        // If the JMP target is not resolved (e.g. non-E9 JMP or unknown pattern),
        // g_externalOverlayPresentHook stays NULL and Steam overlay will not
        // be explicitly invoked on the forced-bypass path — the overlay module
        // must hook a different Present entry point (e.g. vtable[8] or Present1).
        {
            void* hookTarget = ResolveE9JmpTarget(presentAddr);
            if (hookTarget) {
                g_externalOverlayPresentHook = (PFN_Present)hookTarget;
                HookLog("InstallPresentInlineHooks: External E9 JMP target = %p (saved, Steam overlay hook available)",
                        hookTarget);
            } else {
                HookLogImportant(
                    "InstallPresentInlineHooks: Could not resolve E9 JMP target at %p "
                    "(bytes: %02X %02X %02X %02X %02X) — external overlay hook not saved",
                    presentAddr, ((const uint8_t*)presentAddr)[0], ((const uint8_t*)presentAddr)[1],
                    ((const uint8_t*)presentAddr)[2], ((const uint8_t*)presentAddr)[3],
                    ((const uint8_t*)presentAddr)[4]);
            }
        }

        // External overlay (e.g. Streamline) has hooked Present with an E9 JMP.
        // DO NOT inline-hook the external detour — patching 14 bytes of the
        // external function's prologue corrupts its internal state and crashes
        // under Frame Generation (where more code paths are exercised).
        //
        // Instead, use a clean vtable hook:
        //   Game calls Present → vtable[8] → DetourPresent (our overlay) →
        //   CallOriginalPresent → oPresent (the SL-hooked function) →
        //   SL E9 JMP → SL processes FG normally → real Present
        //
        // Also create bypass trampolines from the original disk bytes so that
        // re-entrant Present calls (from SL's vtable callback) can call the real
        // DXGI Present without re-entering the external E9 JMP hook chain.

        void** vtable = *(void***)pSwapChain;
        if (!vtable) {
            HookLog("InstallPresentInlineHooks: External JMP detected but vtable is null");
            s_inlineHooksInstalled = true;
            return true;
        }

        DWORD oldProtect;
        if (!VirtualProtect(reinterpret_cast<void*>(vtable), 23 * sizeof(void*), PAGE_READWRITE, &oldProtect)) {
            HookLog("InstallPresentInlineHooks: VirtualProtect failed for vtable hook");
            s_inlineHooksInstalled = true;
            return true;
        }

        s_hookedVTable = vtable;

        // === STEAM DX12 OVERLAY PRE-INITIALIZATION ===
        //
        // Steam's OverlayHookD3D3 lazily initializes internal Present-shaped
        // callback slots during its first E9 JMP entry. CE's vtable hook
        // (setting vtable[8] = DetourPresent below) prevents this natural
        // initialization because Steam's E9 JMP never fires on the game's
        // Present calls — they go through DetourPresent instead of dxgi!Present.
        //
        // Best-effort pre-init: Call Present on the temp swapchain through the
        // REAL dxgi!Present (vtable[8] is still unhooked at this point) BEFORE
        // installing our vtable hook.  This initializes Steam's "next" handler
        // but does NOT initialize every real game-swapchain callback slot
        // (the 2x2 hidden-window temp swapchain causes Steam to skip full init).
        //
        // The actual fix for the NULL rendering callback is the VEH-protected
        // call in AttemptSteamDX12OverlayInit (see below), which patches the
        // NULL pointer at crash time and lets Steam continue.
        //
        // Thread safety: InstallPresentInlineHooks runs once on the hook thread.
        // The temp swapchain is valid and vtable page is already writable
        // (from VirtualProtect at line ~2843).
            const char* overlayModule = ce::overlay_compat::GetLoadedThirdPartyOverlayModuleName();
            if (overlayModule && IsSteamOverlayModule(overlayModule)) {
                HookLogImportant(
                    "InstallPresentInlineHooks: Pre-initializing Steam overlay on temp swapchain %p "
                    "(vtable[8]=%p = dxgi!Present, before CE vtable hook)",
                    pSwapChain, (void*)vtable[8]);

                PFN_Present realPresent = (PFN_Present)vtable[8];
                HRESULT steamInitHr = realPresent(pSwapChain, 0, 0);

                HookLogImportant(
                    "InstallPresentInlineHooks: Steam overlay pre-init on temp swapchain "
                    "returned hr=0x%08X — proceeding with vtable hook installation",
                    (unsigned)steamInitHr);
        }
        // === END STEAM PRE-INIT ===

        oPresent = (PFN_Present)vtable[8];
        vtable[8] = (void*)DetourPresent;
        HookLogImportant(
            "InstallPresentInlineHooks: VTable hook on Present (original=%p, vtable=%p) — "
            "external E9 JMP detected, using non-invasive hook for FG compat",
            oPresent, vtable);

        if (presentBypass) {
            oPresentBypass = (PFN_Present)presentBypass;
            HookLog("InstallPresentInlineHooks: Present bypass trampoline created at %p", presentBypass);
        }

        if (present1Addr) {
            oPresent1 = (PFN_Present1)vtable[22];
            vtable[22] = (void*)DetourPresent1;
            HookLog("InstallPresentInlineHooks: VTable hook on Present1 (original=%p)", oPresent1);

            if (present1Bypass) {
                oPresent1Bypass = (PFN_Present1)present1Bypass;
                HookLog("InstallPresentInlineHooks: Present1 bypass trampoline created at %p", present1Bypass);
            }
        }

        VirtualProtect(reinterpret_cast<void*>(vtable), 23 * sizeof(void*), oldProtect, &oldProtect);

        s_inlineHooksInstalled = true;
        return true;
    }
