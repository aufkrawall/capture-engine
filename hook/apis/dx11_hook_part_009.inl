
            ID3D11Query* currentQ = g_PrerenderQueries[g_PrerenderFrameIndex % g_PrerenderQueries.size()];
            ctx->End(currentQ);

            if (g_PrerenderFrameIndex >= (uint64_t)lookback) {
                ID3D11Query* waitQ = g_PrerenderQueries[(g_PrerenderFrameIndex - lookback) % g_PrerenderQueries.size()];
                int64_t waitStart = PerfLogger::GetQpcUs();
                while (ctx->GetData(waitQ, nullptr, 0, 0) == S_FALSE) {
                    SwitchToThread();
                }
                int64_t waitUs = PerfLogger::GetQpcUs() - waitStart;
                int idx = g_DiagPrerenderWaits.fetch_add(1, std::memory_order_relaxed);
                if (idx < 12) {
                    HookLogImportant("DX11: Prerender buffered wait lookback=%d frame=%llu wait=%lldus (#%d)", lookback,
                                     (unsigned long long)g_PrerenderFrameIndex, (long long)waitUs, idx + 1);
                }
            }
        }
        g_PrerenderFrameIndex++;
        g_DiagPrerenderFrames.fetch_add(1, std::memory_order_relaxed);
    }

    ctx->Release();
    dev->Release();
}

namespace DXGIShared {
void HandleDX11ProcessFrame(IDXGISwapChain* pSwapChain, bool isRealFrame) {
    // Deferred AF bootstrap: capture already-bound samplers/SRVs once so later
    // SRV changes can reconcile forced AF with resource context.
    ApplyDeferredSamplerOverrides11(pSwapChain);

    ::HandleDX11ProcessFrame(pSwapChain, isRealFrame);
}

void HandleDX11ResizeBegin() {
    CleanupDX11Resources();
}
}  // namespace DXGIShared

HRESULT STDMETHODCALLTYPE DetourCreateSamplerState(ID3D11Device* pDevice, const D3D11_SAMPLER_DESC* pSamplerDesc,
                                                   ID3D11SamplerState** ppSamplerState) {
    if (!pSamplerDesc)
        return oCreateSamplerState(pDevice, pSamplerDesc, ppSamplerState);
    if (DX11Hook_IsWrapperSamplerForwarding())
        return oCreateSamplerState(pDevice, pSamplerDesc, ppSamplerState);
    if (!g_GraphicsOverridesActive.load(std::memory_order_acquire))
        return oCreateSamplerState(pDevice, pSamplerDesc, ppSamplerState);

    bool debug = false;
    D3D11_SAMPLER_DESC desc = *pSamplerDesc;

    if (g_IPC && g_IPC->GetSharedMem() && g_IPC->GetSharedMem()->GetDebugLogging()) {
        debug = true;
    }

    const auto& gfx = GetActiveGraphicsConfig();
    // AF enablement needs SRV/resource context on Blackwell. Create-time still
    // handles AF-off, mip mapping, and mip-bias changes, but forced AF-on is
    // applied later by the bind-state reconciler.
    const bool allowAF = false;
    const bool modified = DX11Hook_ApplySamplerOverrides(desc, gfx, allowAF);

    {
        static std::atomic<int> s_createAFLog{0};
        int idx = s_createAFLog.fetch_add(1, std::memory_order_relaxed);
        if (idx < 48) {
            if (modified) {
                HookLogImportant(
                    "DX11: CreateSamplerState override origFilter=0x%X newFilter=0x%X Aniso=%u Bias=%.2f Addr=%d/%d/%d "
                    "Comp=%d MinLOD=%.1f MaxLOD=%.1f (#%d)",
                    pSamplerDesc->Filter, desc.Filter, desc.MaxAnisotropy, desc.MipLODBias, pSamplerDesc->AddressU,
                    pSamplerDesc->AddressV, pSamplerDesc->AddressW, pSamplerDesc->ComparisonFunc, pSamplerDesc->MinLOD,
                    pSamplerDesc->MaxLOD, idx + 1);
            } else if (ce::sampler_override::IsAnisotropicOverrideEnabled(gfx)) {
                HookLogImportant(
                    "DX11: CreateSamplerState deferred runtime AF Filter=0x%X Aniso=%u Addr=%d/%d/%d Comp=%d "
                    "MinLOD=%.1f MaxLOD=%.1f (#%d)",
                    pSamplerDesc->Filter, pSamplerDesc->MaxAnisotropy, pSamplerDesc->AddressU, pSamplerDesc->AddressV,
                    pSamplerDesc->AddressW, pSamplerDesc->ComparisonFunc, pSamplerDesc->MinLOD, pSamplerDesc->MaxLOD,
                    idx + 1);
            } else if (idx < 8) {
                HookLogImportant(
                    "DX11: CreateSamplerState passthrough AF disabled Filter=0x%X Aniso=%u Addr=%d/%d/%d Comp=%d "
                    "MinLOD=%.1f MaxLOD=%.1f af=%s (#%d)",
                    pSamplerDesc->Filter, pSamplerDesc->MaxAnisotropy, pSamplerDesc->AddressU, pSamplerDesc->AddressV,
                    pSamplerDesc->AddressW, pSamplerDesc->ComparisonFunc, pSamplerDesc->MinLOD, pSamplerDesc->MaxLOD,
                    gfx.anisotropicFiltering.c_str(), idx + 1);
            }
        }
    }

    HRESULT hr;
    if (modified) {
        hr = oCreateSamplerState(pDevice, &desc, ppSamplerState);
        if (FAILED(hr)) {
            if (debug) {
                EarlyLog(
                    "DX11: CreateSamplerState FAILED with modified desc "
                    "(hr=0x%08X). Filter=0x%X Bias=%.2f Aniso=%u",
                    hr, desc.Filter, desc.MipLODBias, desc.MaxAnisotropy);
            }
        }
    } else {
        hr = oCreateSamplerState(pDevice, pSamplerDesc, ppSamplerState);
    }
    return hr;
}

bool DX10Hook_ApplySamplerOverrides(D3D10_SAMPLER_DESC& desc, const GraphicsConfig& gfx) {
    if (!ce::sampler_override::IsD3D10SamplerOverrideEligible(desc, gfx)) {
        return false;
    }

    bool modified = false;
    const std::string& af = gfx.anisotropicFiltering;
    if (af != "default" && !af.empty()) {
        if (af == "off") {
            if (ce::sampler_override::IsD3D10AnisotropicFilter(desc.Filter)) {
                desc.Filter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
                desc.MaxAnisotropy = 1;
                modified = true;
            }
        } else if (ce::sampler_override::D3D10SamplerAllowsCreationTimeForcedAF(desc, gfx)) {
            const D3D10_FILTER forcedFilter = ce::sampler_override::GetForcedAnisotropicFilter(desc.Filter);
            const UINT forcedAnisotropy = ce::sampler_override::GetConfiguredMaxAnisotropy(gfx);
            if (desc.Filter != forcedFilter || desc.MaxAnisotropy != forcedAnisotropy) {
                desc.Filter = forcedFilter;
                desc.MaxAnisotropy = forcedAnisotropy;
                modified = true;
            }
        }
    }

    if (!ce::sampler_override::IsD3D10AnisotropicFilter(desc.Filter)) {
        D3D10_FILTER forcedFilter = desc.Filter;
        if (gfx.mipMapping == "trilinear") {
            forcedFilter = D3D10_FILTER_MIN_MAG_MIP_LINEAR;
        } else if (gfx.mipMapping == "bilinear") {
            forcedFilter = D3D10_FILTER_MIN_MAG_LINEAR_MIP_POINT;
        }
        if (desc.Filter != forcedFilter) {
            desc.Filter = forcedFilter;
            modified = true;
        }
    }

    float userBiasValue = 0.0f;
    const bool userBiasActive = TryParseConfiguredMipBias(gfx, userBiasValue);
    const float originalBias = desc.MipLODBias;
    desc.MipLODBias = ApplyConfiguredMipBias(gfx, originalBias);

    if (gfx.sgssaa && !gfx.disableAutoMipBias && !gfx.forceMipBiasClamp) {
        float sgssaaBias = 0.0f;
        if (GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), sgssaaBias)) {
            desc.MipLODBias += sgssaaBias;
        }
    }
    if (userBiasActive && userBiasValue < 0.0f && !gfx.sgssaa && IsUnityProcess() && !gfx.forceMipBiasClamp &&
        desc.MipLODBias < -0.5f) {
        desc.MipLODBias = -0.5f;
    }
    desc.MipLODBias = FinalizeMipBias(gfx, desc.MipLODBias);
    modified = modified || desc.MipLODBias != originalBias;
    return modified;
}

// D3D10 sampler descriptors are immutable. Apply the conservative policy once
// at creation so there is no sampler-bind or draw-time override work.
HRESULT STDMETHODCALLTYPE DetourCreateSamplerState10(ID3D10Device* pDevice, const D3D10_SAMPLER_DESC* pSamplerDesc,
                                                     ID3D10SamplerState** ppSamplerState) {
    if (!pSamplerDesc)
        return oCreateSamplerState10(pDevice, pSamplerDesc, ppSamplerState);
    if (DX11Hook_IsWrapperSamplerForwarding())
        return oCreateSamplerState10(pDevice, pSamplerDesc, ppSamplerState);

    D3D10_SAMPLER_DESC desc = *pSamplerDesc;
    const GraphicsConfig& gfx = GetActiveGraphicsConfigCached();
    const bool modified = DX10Hook_ApplySamplerOverrides(desc, gfx);

    HRESULT hr = oCreateSamplerState10(pDevice, modified ? &desc : pSamplerDesc, ppSamplerState);
    if (modified && FAILED(hr)) {
        if (ppSamplerState && *ppSamplerState) {
            (*ppSamplerState)->Release();
            *ppSamplerState = nullptr;
        }
        static std::atomic<int> s_fallbackLogCount{0};
        const int logIndex = s_fallbackLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 8) {
            HookLogImportant(
                "DX10: Modified sampler rejected; retrying original descriptor hr=0x%08X filter=0x%X aniso=%u "
                "bias=%.2f (#%d)",
                hr, desc.Filter, desc.MaxAnisotropy, desc.MipLODBias, logIndex + 1);
        }
        hr = oCreateSamplerState10(pDevice, pSamplerDesc, ppSamplerState);
    } else if (modified) {
        static std::atomic<int> s_overrideLogCount{0};
        const int logIndex = s_overrideLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logIndex < 32) {
            HookLogImportant(
                "DX10: Creation-time sampler override filter=0x%X->0x%X aniso=%u->%u bias=%.2f->%.2f "
                "policy=%s (#%d)",
                pSamplerDesc->Filter, desc.Filter, pSamplerDesc->MaxAnisotropy, desc.MaxAnisotropy,
                pSamplerDesc->MipLODBias, desc.MipLODBias, gfx.samplerOverrideMode.c_str(), logIndex + 1);
        }
    }
    return hr;
}

void DX11Hook::ProcessDeferredReleases() {
    g_DeferredRelease.Process();
}

void DX11Hook::Init() {
    HookLog("DX11Hook::Init()");

    // CRITICAL FIX: Check if Vulkan is active before installing D3D11 hooks
    // Vulkan games using WSI-to-DXGI mapping can freeze if we hook D3D11/DXGI
    HMODULE hVulkan = GetModuleHandleW(L"vulkan-1.dll");
    if (hVulkan) {
        HookLog(
            "DX11: Vulkan detected (vulkan-1.dll), SKIPPING D3D11 hook "
            "installation");
        return;
    }

    // D3D11CreateDeviceAndSwapChain hook is now handled by IAT patching in
    // iat_hook.cpp The InitializeD3D11Hooks() function sets up the IAT hook
    HMODULE hD3D11 = GetModuleHandleA("d3d11.dll");
    if (hD3D11) {
        // Initialize IAT hooks now that d3d11.dll is loaded
        // This may have been called before d3d11.dll was loaded at startup
        IATHook::InitializeD3D11Hooks();

        // Also hook the export directly using CustomHook to catch calls that bypass
        // IAT (e.g., statically bound imports, GetProcAddress)
        CustomHook::Initialize();
        void* pTarget = (void*)GetProcAddress(hD3D11, "D3D11CreateDeviceAndSwapChain");
        if (pTarget) {
            HookLog("DX11: Hook target at %p", pTarget);
            // Save the real original BEFORE HookExport overwrites the shared
            // oD3D11CreateDeviceAndSwapChain (shared with wrapper_hooks.cpp).
            s_oRealD3D11CreateDeviceAndSwapChain = reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain>(pTarget);
            bool created = CustomHook::HookExport(
                               "d3d11.dll", "D3D11CreateDeviceAndSwapChain", (void*)DetourD3D11CreateDeviceAndSwapChain,
                               (void**)&oD3D11CreateDeviceAndSwapChain) == CustomHook::Status::Success;
            HookLog("DX11: HookExport result: %s", created ? "success" : "failed");
            HookLog("DX11: D3D11CreateDeviceAndSwapChain export hook installed.");
        } else {
            HookLog("DX11: Failed to get D3D11CreateDeviceAndSwapChain address!");
        }
        HookLog("DX11: D3D11CreateDeviceAndSwapChain hook installed.");
    }

    // 2. Hook D3D10 entry points
    // D3D10 hooking is handled by IAT in iat_hook.cpp / wrapper_hooks.cpp
    HMODULE hD3D10 = GetModuleHandleA("d3d10.dll");
    if (hD3D10) {
        HookLog("DX11: D3D10 hooks should be active via IAT.");
    }

    // 3. Hook DXGI Factory entry points
    // DXGI Factory hooking is handled by IAT.
    HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
    if (hDXGI) {
        HookLog("DX11: DXGI hooks should be active via IAT.");
    }

    // 4. Scan for EXISTING swapchains (late injection scenario)
    // If the game already created the device/swapchain before we injected,
    // we need to hook the vtable of an EXISTING swapchain.
    // We do this by creating a temporary swapchain using the hooked factory,
    // which will also trigger our InstallVTableHooks.
    HookLog("DX11: Scanning for pre-existing swapchains...");

    // First, try D3D10 route (the game is D3D10)
    if (hD3D10) {
        typedef HRESULT(WINAPI * PFN_D3D10CreateDevice)(IDXGIAdapter*, D3D10_DRIVER_TYPE, HMODULE, UINT, UINT,
                                                        ID3D10Device**);
        PFN_D3D10CreateDevice pD3D10CD = (PFN_D3D10CreateDevice)GetProcAddress(hD3D10, "D3D10CreateDevice");
        if (pD3D10CD) {
            ID3D10Device* tempDevice = nullptr;
            // Use the REAL function, not our detour, to create a temp device
            HRESULT hr = pD3D10CD(NULL, D3D10_DRIVER_TYPE_HARDWARE, NULL, 0, D3D10_SDK_VERSION, &tempDevice);
            if (SUCCEEDED(hr) && tempDevice) {
                // Get DXGI factory from temp device
                IDXGIDevice* dxgiDev = nullptr;
                if (SUCCEEDED(tempDevice->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev))) {
                    IDXGIAdapter* adapter = nullptr;
                    if (SUCCEEDED(dxgiDev->GetAdapter(&adapter))) {
                        IDXGIFactory* factory = nullptr;
                        if (SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory))) {
                            // Create a temp hidden window for temp swapchain
                            HWND tempHwnd = CreateWindowExA(0, "STATIC", "TempDXGI", WS_OVERLAPPEDWINDOW, 0, 0, 100,
                                                            100, NULL, NULL, GetModuleHandle(NULL), NULL);
                            if (tempHwnd) {
                                DXGI_SWAP_CHAIN_DESC scd = {};
                                scd.BufferCount = 1;
                                scd.BufferDesc.Width = 100;
                                scd.BufferDesc.Height = 100;
                                scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
                                scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
                                scd.OutputWindow = tempHwnd;
                                scd.SampleDesc.Count = 1;
                                scd.Windowed = TRUE;
                                scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

                                IDXGISwapChain* tempSC = nullptr;
                                // This call goes through our detour and will install vtable
                                // hooks!
                                hr = factory->CreateSwapChain(tempDevice, &scd, &tempSC);
                                if (SUCCEEDED(hr) && tempSC) {
                                    HookLog(
                                        "DX11: Temp D3D10 swapchain created to install "
                                        "vtable hooks");
                                    tempSC->Release();
                                }
                                DestroyWindow(tempHwnd);
                            }
                            factory->Release();
                        }
                        adapter->Release();
                    }
                    dxgiDev->Release();
                }
                tempDevice->Release();
            }
        }
    }

    // If D3D10 isn't loaded (common for pure DX11 apps), still force install the
    // Present hook by creating a dummy D3D11 device+swapchain via the original
    // D3D11CreateDeviceAndSwapChain.
    if (hD3D11 && oD3D11CreateDeviceAndSwapChain) {
        HWND tempHwnd = CreateWindowExA(0, "STATIC", "TempD3D11", WS_OVERLAPPEDWINDOW, 0, 0, 100, 100, NULL, NULL,
                                        GetModuleHandle(NULL), NULL);
        if (tempHwnd) {
            DXGI_SWAP_CHAIN_DESC scd = {};
            scd.BufferCount = 1;
            scd.BufferDesc.Width = 100;
            scd.BufferDesc.Height = 100;
            scd.BufferDesc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
            scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            scd.OutputWindow = tempHwnd;
            scd.SampleDesc.Count = 1;
            scd.Windowed = TRUE;
            scd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

            D3D_FEATURE_LEVEL flOut = D3D_FEATURE_LEVEL_11_0;
            D3D_FEATURE_LEVEL flReq[] = {D3D_FEATURE_LEVEL_11_0};
            ID3D11Device* dev = nullptr;
            ID3D11DeviceContext* ctx = nullptr;
            IDXGISwapChain* sc = nullptr;

            HRESULT hr = (s_oRealD3D11CreateDeviceAndSwapChain ? s_oRealD3D11CreateDeviceAndSwapChain
                                                               : oD3D11CreateDeviceAndSwapChain)(
                nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, flReq, 1,
                D3D11_SDK_VERSION, &scd, &sc, &dev, &flOut, &ctx);
            if (SUCCEEDED(hr) && sc) {
                InstallVTableHooks(dev, ctx, sc);
                CWrapDXGISwapChain* wrappedSc = nullptr;
                IDXGISwapChain* realSc = nullptr;
                if (SUCCEEDED(sc->QueryInterface(IID_CWrapDXGISwapChain, (void**)&wrappedSc)) && wrappedSc) {
                    realSc = wrappedSc->GetReal();
                    wrappedSc->Release();
                }
                HookLog("DX11: Temp D3D11 install target (wrapper=%p, real=%p)", sc, realSc);
                DXGIShared::InstallHooks(realSc ? realSc : sc, true);
                HookLog("DX11: Temp D3D11 swapchain created to install vtable hooks");
            }

            if (sc)
                sc->Release();
            if (ctx)
                ctx->Release();
            if (dev)
                dev->Release();
            DestroyWindow(tempHwnd);
        }
    }
}

void DX11Hook::Shutdown() {
    HookLog("DX11Hook::Shutdown()");

    // Log diagnostic summary for sampler/prerender overrides
    {
        int afApplied = g_DiagSamplerAFApplied.load(std::memory_order_relaxed);
        int afReplaced = g_DiagSamplerReplacementCreated.load(std::memory_order_relaxed);
        int afAllowed = g_DiagSamplerAllowsAF.load(std::memory_order_relaxed);
        int afNoMips = g_DiagSamplerSkipNoMips.load(std::memory_order_relaxed);
        int afBorder = g_DiagSamplerSkipBorder.load(std::memory_order_relaxed);
        int afReduction = g_DiagSamplerSkipReduction.load(std::memory_order_relaxed);
        int afComparison = g_DiagSamplerSkipComparison.load(std::memory_order_relaxed);
        int afStage = g_DiagSamplerSkipStage.load(std::memory_order_relaxed);
        int afNoShader = g_DiagSamplerSkipNoShader.load(std::memory_order_relaxed);
        int afNoShaderMeta = g_DiagSamplerSkipNoShaderMetadata.load(std::memory_order_relaxed);
        int afShaderUnused = g_DiagSamplerSkipShaderUnused.load(std::memory_order_relaxed);
        int afExplicitSample = g_DiagSamplerSkipExplicitSample.load(std::memory_order_relaxed);
        int afAllowLod = g_DiagSamplerAllowLodSample.load(std::memory_order_relaxed);
        int afNoSRV = g_DiagSamplerSkipNoSRV.load(std::memory_order_relaxed);
        int afFormat = g_DiagSamplerSkipFormat.load(std::memory_order_relaxed);
        int afSingleMip = g_DiagSamplerSkipSingleMip.load(std::memory_order_relaxed);
        int afNonColor = g_DiagSamplerSkipNonColorResource.load(std::memory_order_relaxed);
        int afUnsafe = g_DiagSamplerSkipUnsafeResource.load(std::memory_order_relaxed);
        int afRuntimeHooks = g_DiagSamplerRuntimeHookInstalled.load(std::memory_order_relaxed);
        int afDrawReconcile = g_DiagSamplerDrawReconcileCalls.load(std::memory_order_relaxed);
        int afReconcileSlots = g_DiagSamplerReconcileSlots.load(std::memory_order_relaxed);
        int afBindDeferred = g_DiagSamplerBindDeferred.load(std::memory_order_relaxed);
        int afEffectiveBindCalls = g_DiagSamplerEffectiveBindCalls.load(std::memory_order_relaxed);
        int afEffectiveBinds = g_DiagSamplerEffectiveBinds.load(std::memory_order_relaxed);
        int afEffectiveBindSkips = g_DiagSamplerEffectiveBindSkips.load(std::memory_order_relaxed);
        int afBootstrapComplete = g_DiagDeferredAFBootstrapComplete.load(std::memory_order_relaxed);
        int afBootstrapRetry = g_DiagDeferredAFBootstrapRetry.load(std::memory_order_relaxed);
        int afBootstrapDisabled = g_DiagDeferredAFBootstrapDisabled.load(std::memory_order_relaxed);
        int afContextVTables = g_DiagD3D11ContextVTablesHooked.load(std::memory_order_relaxed);
        int afContextHookSkips = g_DiagD3D11ContextHookSkips.load(std::memory_order_relaxed);
        int afDeferredContexts = g_DiagCreateDeferredContext11.load(std::memory_order_relaxed);
        int afExecuteCommandLists = g_DiagExecuteCommandList11.load(std::memory_order_relaxed);
        int mipBias = g_DiagSamplerMipBiasApplied.load(std::memory_order_relaxed);
        int mipOverride = g_DiagSamplerMipOverride.load(std::memory_order_relaxed);
        int prerenderFrames = g_DiagPrerenderFrames.load(std::memory_order_relaxed);
        int prerenderWaits = g_DiagPrerenderWaits.load(std::memory_order_relaxed);
        HookLog(
            "DX11: Override summary: AF_allowed=%d AF_applied=%d AF_replaced=%d AF_runtimeHooks=%d "
            "AF_skip(noMips=%d border=%d reduction=%d comp=%d stage=%d "
            "noShader=%d noShaderMeta=%d shaderUnused=%d explicitSample=%d noSRV=%d fmt=%d singleMip=%d "
            "nonColor=%d unsafe=%d) AF_lodAllowed=%d bindDeferred=%d effectiveBindCalls=%d effectiveBinds=%d "
            "bindSkips=%d drawReconcile=%d reconcileSlots=%d "
            "bootstrap(complete=%d retry=%d disabled=%d) "
            "contextVTables=%d contextHookSkips=%d deferredContexts=%d executeCommandLists=%d "
            "mipBias=%d mipOverride=%d prerender(frames=%d waits=%d)",
            afAllowed, afApplied, afReplaced, afRuntimeHooks, afNoMips, afBorder, afReduction, afComparison, afStage,
            afNoShader, afNoShaderMeta, afShaderUnused, afExplicitSample, afNoSRV, afFormat, afSingleMip, afNonColor,
            afUnsafe, afAllowLod, afBindDeferred, afEffectiveBindCalls, afEffectiveBinds, afEffectiveBindSkips,
            afDrawReconcile, afReconcileSlots, afBootstrapComplete, afBootstrapRetry, afBootstrapDisabled,
            afContextVTables, afContextHookSkips, afDeferredContexts, afExecuteCommandLists, mipBias, mipOverride,
            prerenderFrames, prerenderWaits);
    }

    // Cleanup OverlayAdapter
    if (g_OverlayAdapter.IsInitialized()) {
        g_OverlayAdapter.Shutdown();
    }

    // CRITICAL FIX: Clear sampler caches to prevent unbounded memory growth
    // These caches accumulate replacement samplers over time
    ClearReplacementSamplerCache11();
    ReleaseTrackedShaderResources11();
    ClearDeferredAFBootstraps11();
    // Clean up prerender queries
    {
        std::lock_guard<std::mutex> lock(g_PrerenderMutex);
        for (auto* q : g_PrerenderQueries) {
            if (q)
                q->Release();
        }
        g_PrerenderQueries.clear();
        g_PrerenderFrameIndex = 0;
        if (g_PrerenderQueryDevice) {
            g_PrerenderQueryDevice->Release();
            g_PrerenderQueryDevice = nullptr;
        }
        if (g_PrerenderSerialQuery10) {
            g_PrerenderSerialQuery10->Release();
            g_PrerenderSerialQuery10 = nullptr;
        }
        if (g_PrerenderQueryDevice10) {
            g_PrerenderQueryDevice10->Release();
            g_PrerenderQueryDevice10 = nullptr;
        }
    }

    g_DX11Capture.Cleanup();
    if (g_mainRenderTargetView) {
        g_DeferredRelease.Queue(g_mainRenderTargetView);
        g_mainRenderTargetView = nullptr;
    }
    if (g_mainRenderTargetView10) {
        g_DeferredRelease.Queue(g_mainRenderTargetView10);
        g_mainRenderTargetView10 = nullptr;
    }

    // Flush deferred release queue
    g_DeferredRelease.Process();
}

void DX11Hook::OnHostDisconnect() {
    // Log diagnostic summary for sampler/prerender overrides
    {
        int afApplied = g_DiagSamplerAFApplied.load(std::memory_order_relaxed);
        int afReplaced = g_DiagSamplerReplacementCreated.load(std::memory_order_relaxed);
        int afAllowed = g_DiagSamplerAllowsAF.load(std::memory_order_relaxed);
        int afNoMips = g_DiagSamplerSkipNoMips.load(std::memory_order_relaxed);
        int afBorder = g_DiagSamplerSkipBorder.load(std::memory_order_relaxed);
        int afReduction = g_DiagSamplerSkipReduction.load(std::memory_order_relaxed);
        int afComparison = g_DiagSamplerSkipComparison.load(std::memory_order_relaxed);
        int afStage = g_DiagSamplerSkipStage.load(std::memory_order_relaxed);
        int afNoShader = g_DiagSamplerSkipNoShader.load(std::memory_order_relaxed);
        int afNoShaderMeta = g_DiagSamplerSkipNoShaderMetadata.load(std::memory_order_relaxed);
        int afShaderUnused = g_DiagSamplerSkipShaderUnused.load(std::memory_order_relaxed);
        int afExplicitSample = g_DiagSamplerSkipExplicitSample.load(std::memory_order_relaxed);
        int afAllowLod = g_DiagSamplerAllowLodSample.load(std::memory_order_relaxed);
        int afNoSRV = g_DiagSamplerSkipNoSRV.load(std::memory_order_relaxed);
        int afFormat = g_DiagSamplerSkipFormat.load(std::memory_order_relaxed);
        int afSingleMip = g_DiagSamplerSkipSingleMip.load(std::memory_order_relaxed);
        int afNonColor = g_DiagSamplerSkipNonColorResource.load(std::memory_order_relaxed);
        int afUnsafe = g_DiagSamplerSkipUnsafeResource.load(std::memory_order_relaxed);
        int afRuntimeHooks = g_DiagSamplerRuntimeHookInstalled.load(std::memory_order_relaxed);
        int afDrawReconcile = g_DiagSamplerDrawReconcileCalls.load(std::memory_order_relaxed);
        int afReconcileSlots = g_DiagSamplerReconcileSlots.load(std::memory_order_relaxed);
        int afBindDeferred = g_DiagSamplerBindDeferred.load(std::memory_order_relaxed);
        int afEffectiveBindCalls = g_DiagSamplerEffectiveBindCalls.load(std::memory_order_relaxed);
        int afEffectiveBinds = g_DiagSamplerEffectiveBinds.load(std::memory_order_relaxed);
        int afEffectiveBindSkips = g_DiagSamplerEffectiveBindSkips.load(std::memory_order_relaxed);
        int afBootstrapComplete = g_DiagDeferredAFBootstrapComplete.load(std::memory_order_relaxed);
        int afBootstrapRetry = g_DiagDeferredAFBootstrapRetry.load(std::memory_order_relaxed);
        int afBootstrapDisabled = g_DiagDeferredAFBootstrapDisabled.load(std::memory_order_relaxed);
        int afContextVTables = g_DiagD3D11ContextVTablesHooked.load(std::memory_order_relaxed);
        int afContextHookSkips = g_DiagD3D11ContextHookSkips.load(std::memory_order_relaxed);
        int afDeferredContexts = g_DiagCreateDeferredContext11.load(std::memory_order_relaxed);
        int afExecuteCommandLists = g_DiagExecuteCommandList11.load(std::memory_order_relaxed);
        int mipBias = g_DiagSamplerMipBiasApplied.load(std::memory_order_relaxed);
        int mipOverride = g_DiagSamplerMipOverride.load(std::memory_order_relaxed);
        int prerenderFrames = g_DiagPrerenderFrames.load(std::memory_order_relaxed);
        int prerenderWaits = g_DiagPrerenderWaits.load(std::memory_order_relaxed);
        HookLog(
            "DX11: Override summary: AF_allowed=%d AF_applied=%d AF_replaced=%d AF_runtimeHooks=%d "
            "AF_skip(noMips=%d border=%d reduction=%d comp=%d stage=%d "
            "noShader=%d noShaderMeta=%d shaderUnused=%d explicitSample=%d noSRV=%d fmt=%d singleMip=%d "
            "nonColor=%d unsafe=%d) AF_lodAllowed=%d bindDeferred=%d effectiveBindCalls=%d effectiveBinds=%d "
            "bindSkips=%d drawReconcile=%d reconcileSlots=%d "
            "bootstrap(complete=%d retry=%d disabled=%d) "
            "contextVTables=%d contextHookSkips=%d deferredContexts=%d executeCommandLists=%d "
            "mipBias=%d mipOverride=%d prerender(frames=%d waits=%d)",
            afAllowed, afApplied, afReplaced, afRuntimeHooks, afNoMips, afBorder, afReduction, afComparison, afStage,
            afNoShader, afNoShaderMeta, afShaderUnused, afExplicitSample, afNoSRV, afFormat, afSingleMip, afNonColor,
            afUnsafe, afAllowLod, afBindDeferred, afEffectiveBindCalls, afEffectiveBinds, afEffectiveBindSkips,
            afDrawReconcile, afReconcileSlots, afBootstrapComplete, afBootstrapRetry, afBootstrapDisabled,
            afContextVTables, afContextHookSkips, afDeferredContexts, afExecuteCommandLists, mipBias, mipOverride,
            prerenderFrames, prerenderWaits);
    }
    HookLog("DX11Hook::OnHostDisconnect() - ready for reconnection");
    // DX11 capture is synchronous, nothing to stop
    // Just cleanup for potential new session
    ClearReplacementSamplerCache11();
    ReleaseTrackedShaderResources11();
    ClearDeferredAFBootstraps11();
    g_DX11Capture.Cleanup();
}
