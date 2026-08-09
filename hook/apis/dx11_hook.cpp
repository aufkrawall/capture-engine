#include "dx11_hook_internal.h"

// Global Deferred Release Queue for D3D11 resources
// Prevents render thread stalls during resource destruction
ce::DeferredReleaseQueue g_DeferredRelease;

static ID3D10Device* g_pd3d10Device = NULL;

static IDXGISwapChain* g_pSwapChain = NULL;

static bool g_IsDX10Device = false;

thread_local unsigned g_D3D11InternalIdentityProbeDepth = 0;

void DX11Hook_BeginInternalIdentityProbe() {
    ++g_D3D11InternalIdentityProbeDepth;
}

void DX11Hook_EndInternalIdentityProbe() {
    if (g_D3D11InternalIdentityProbeDepth != 0)
        --g_D3D11InternalIdentityProbeDepth;
}

void DX10Hook_RegisterDeviceIdentity(ID3D10Device* device, bool is10_1, const char* evidence) {
    if (!device)
        return;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(dx11_hook_g_GraphicsApiIdentityMutex);
        const auto it = dx11_hook_g_D3D10DeviceIdentities.find(device);
        changed = it == dx11_hook_g_D3D10DeviceIdentities.end() || it->second != is10_1;
        dx11_hook_g_D3D10DeviceIdentities[device] = is10_1;
    }
    if (changed) {
        HookLogImportant("[GraphicsAPI] D3D10 device identity device=%p api=%s evidence=%s", device,
                         is10_1 ? "DX10.1" : "DX10", evidence ? evidence : "unknown");
    }
}

void DX10Hook_RegisterSwapChainIdentity(IDXGISwapChain* swapChain, bool is10_1, const char* evidence) {
    if (!swapChain)
        return;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(dx11_hook_g_GraphicsApiIdentityMutex);
        const auto it = dx11_hook_g_D3D10SwapChainIdentities.find(swapChain);
        changed = it == dx11_hook_g_D3D10SwapChainIdentities.end() || it->second != is10_1;
        dx11_hook_g_D3D10SwapChainIdentities[swapChain] = is10_1;
    }
    if (changed) {
        HookLogImportant("[GraphicsAPI] D3D10 swapchain identity swapChain=%p api=%s evidence=%s", swapChain,
                         is10_1 ? "DX10.1" : "DX10", evidence ? evidence : "unknown");
    }
}

void DX11Hook_RegisterDeviceIdentity(ID3D11Device* device, const char* evidence, bool newDevice) {
    if (!device)
        return;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(dx11_hook_g_GraphicsApiIdentityMutex);
        if (newDevice) {
            unsigned previous = 0;
            changed = !dx11_hook_g_D3D11MinorUse.TryGet(device, &previous) || previous != 0;
            dx11_hook_g_D3D11MinorUse.Set(device, 0u);
        } else {
            changed = dx11_hook_g_D3D11MinorUse.Ensure(device, 0u);
        }
    }
    if (changed) {
        HookLogImportant("[GraphicsAPI] D3D11 device identity device=%p api=DX11 evidence=%s", device,
                         evidence ? evidence : "unknown");
    }
}

void DX11Hook_ReportApiUse(ID3D11Device* device, unsigned minorVersion, const char* evidence) {
    if (!device || minorVersion == 0)
        return;
    unsigned previous = 0;
    unsigned updated = 0;
    {
        std::lock_guard<std::mutex> lock(dx11_hook_g_GraphicsApiIdentityMutex);
        dx11_hook_g_D3D11MinorUse.TryGet(device, &previous);
        updated = ce::graphics_api_identity::MergeD3D11Minor(previous, minorVersion);
        dx11_hook_g_D3D11MinorUse.Set(device, updated);
    }
    if (updated != previous) {
        const std::string label = ce::graphics_api_identity::D3D11Label(updated, false);
        HookLogImportant("[GraphicsAPI] D3D11 API use device=%p api=%s evidence=%s", device, label.c_str(),
                         evidence ? evidence : "unknown");
    }
}

static int64_t g_LastSleepUs = 0;

static std::atomic<int> g_DiagSamplerAFApplied{0};

static std::atomic<int> g_DiagSamplerMipBiasApplied{0};

static std::atomic<int> g_DiagSamplerMipOverride{0};

// Re-entrancy guard: prevents mutual recursion with other Present hooks (e.g. Steam overlay)
thread_local bool g_InPresentHook = false;

static void ClearDeferredAFBootstraps11() {
    std::lock_guard<std::mutex> lock(dx11_hook_g_DeferredAFBootstrapMutex);
    dx11_hook_g_DeferredAFBootstrappedContexts.clear();
}

static thread_local uint32_t g_WrapperContextForwardDepth11 = 0;

static thread_local uint32_t g_WrapperSamplerForwardDepth = 0;

void DX11Hook_BeginWrapperContextForwarding() {
    ++g_WrapperContextForwardDepth11;
}

void DX11Hook_EndWrapperContextForwarding() {
    if (g_WrapperContextForwardDepth11 != 0) {
        --g_WrapperContextForwardDepth11;
    }
}

bool DX11Hook_IsWrapperContextForwarding() {
    return g_WrapperContextForwardDepth11 != 0;
}

void DX11Hook_BeginWrapperSamplerForwarding() {
    ++g_WrapperSamplerForwardDepth;
}

void DX11Hook_EndWrapperSamplerForwarding() {
    if (g_WrapperSamplerForwardDepth != 0)
        --g_WrapperSamplerForwardDepth;
}

bool DX11Hook_IsWrapperSamplerForwarding() {
    return g_WrapperSamplerForwardDepth != 0;
}

static void ClearReplacementSamplerCache11() {
    std::unique_lock<std::shared_mutex> lock(dx11_hook_g_SamplerCacheMutex11);
    ClearReplacementSamplerCache11Unlocked();
}

bool DX11Hook_ApplySamplerOverrides(D3D11_SAMPLER_DESC& desc, const GraphicsConfig& gfx,
                                    bool allowAnisotropicOverride) {
    bool modified = false;

    if (desc.MaxLOD <= 0.0f || desc.MinLOD >= desc.MaxLOD ||
        ce::sampler_override::IsD3D11ComparisonFilter(desc.Filter) ||
        ce::sampler_override::IsD3D11ReductionFilter(desc.Filter) || desc.AddressU == D3D11_TEXTURE_ADDRESS_BORDER ||
        desc.AddressV == D3D11_TEXTURE_ADDRESS_BORDER || desc.AddressW == D3D11_TEXTURE_ADDRESS_BORDER) {
        return false;
    }
    if (gfx.samplerOverrideMode != "aggressive") {
        if (D3D11_DECODE_MIN_FILTER(desc.Filter) != D3D11_FILTER_TYPE_LINEAR ||
            D3D11_DECODE_MAG_FILTER(desc.Filter) != D3D11_FILTER_TYPE_LINEAR) {
            return false;
        }
    }

    const std::string& af = gfx.anisotropicFiltering;
    if (af != "default" && !af.empty()) {
        if (af == "off") {
            if (ce::sampler_override::IsD3D11AnisotropicFilter(desc.Filter)) {
                desc.Filter = ce::sampler_override::IsD3D11ComparisonFilter(desc.Filter)
                                  ? D3D11_FILTER_COMPARISON_MIN_MAG_MIP_LINEAR
                                  : D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                desc.MaxAnisotropy = 1;
                modified = true;
                HookLogImportant("DX11: AF override OFF Filter=0x%X->0x%X Aniso=%u->1", desc.Filter, desc.Filter,
                                 desc.MaxAnisotropy);
            }
        } else if (allowAnisotropicOverride) {
            const UINT maxAniso = ce::sampler_override::GetConfiguredMaxAnisotropy(gfx);
            const D3D11_FILTER newFilter = ce::sampler_override::GetForcedAnisotropicFilter(desc.Filter);
            if (desc.Filter != newFilter || desc.MaxAnisotropy != maxAniso) {
                const D3D11_FILTER origFilter = desc.Filter;
                const UINT origAniso = desc.MaxAnisotropy;
                desc.Filter = newFilter;
                desc.MaxAnisotropy = maxAniso;
                modified = true;
                int idx = g_DiagSamplerAFApplied.fetch_add(1, std::memory_order_relaxed);
                if (idx < 48) {
                    HookLogImportant("DX11: AF override ON Filter=0x%X->0x%X Aniso=%u->%u (#%d)", origFilter,
                                     desc.Filter, origAniso, desc.MaxAnisotropy, idx + 1);
                }
            }
        }
    }

    const std::string& mip = gfx.mipMapping;
    const bool isAniso = ce::sampler_override::IsD3D11AnisotropicFilter(desc.Filter);
    if (mip != "default" && !isAniso) {
        if (mip == "trilinear") {
            if (desc.Filter != D3D11_FILTER_MIN_MAG_MIP_LINEAR) {
                desc.Filter = D3D11_FILTER_MIN_MAG_MIP_LINEAR;
                modified = true;
                int idx = g_DiagSamplerMipOverride.fetch_add(1, std::memory_order_relaxed);
                if (idx < 12) {
                    HookLogImportant("DX11: Mip override trilinear applied (#%d)", idx + 1);
                }
            }
        } else if (mip == "bilinear") {
            if (desc.Filter != D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT) {
                desc.Filter = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
                modified = true;
                int idx = g_DiagSamplerMipOverride.fetch_add(1, std::memory_order_relaxed);
                if (idx < 12) {
                    HookLogImportant("DX11: Mip override bilinear applied (#%d)", idx + 1);
                }
            }
        }
    }

    float userBiasVal = 0.0f;
    const bool userBiasActive = TryParseConfiguredMipBias(gfx, userBiasVal);
    const float originalBias = desc.MipLODBias;
    desc.MipLODBias = ApplyConfiguredMipBias(gfx, originalBias);
    if (desc.MipLODBias != originalBias) {
        modified = true;
        int idx = g_DiagSamplerMipBiasApplied.fetch_add(1, std::memory_order_relaxed);
        if (idx < 24) {
            HookLogImportant("DX11: Mip bias override Bias=%.2f->%.2f (#%d)", originalBias, desc.MipLODBias, idx + 1);
        }
    }

    if (gfx.sgssaa && !gfx.disableAutoMipBias && !gfx.forceMipBiasClamp) {
        float sgBias = 0.0f;
        if (GetSGSSAABias(gfx.sgssaa, gfx.msaaSamples.c_str(), sgBias)) {
            desc.MipLODBias += sgBias;
            modified = true;
            HookLogImportant("DX11: SGSSAA bias applied (%.2f, total=%.2f)", sgBias, desc.MipLODBias);
        }
    }

    if (userBiasActive && userBiasVal < 0.0f && !gfx.sgssaa && IsUnityProcess() && !gfx.forceMipBiasClamp) {
        if (desc.MipLODBias < -0.5f) {
            desc.MipLODBias = -0.5f;
            modified = true;
            HookLogImportant("DX11: Unity mip bias clamp -0.5 applied");
        }
    }

    const float finalizedBias = FinalizeMipBias(gfx, desc.MipLODBias);
    if (finalizedBias != desc.MipLODBias) {
        desc.MipLODBias = finalizedBias;
        modified = true;
        HookLogImportant("DX11: Finalized mip bias %.2f->%.2f", desc.MipLODBias, finalizedBias);
    }

    return modified;
}

void DX11Hook::Init() {
    HookLog("DX11Hook::Init()");

    // CRITICAL FIX: Check if Vulkan actually owns rendering before installing
    // D3D11 hooks.  Vulkan games using WSI-to-DXGI mapping can freeze if we
    // hook D3D11/DXGI.  The decision is evidence-based and shared with
    // CheckAndInstallHooks: mere vulkan-1.dll presence in a D3D process must
    // not suppress the D3D11 hooks.
    if (DXGIShared::IsVulkanActive()) {
        HookLog(
            "DX11: Vulkan active (evidence-based), SKIPPING D3D11 hook "
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
            dx11_hook_s_oRealD3D11CreateDeviceAndSwapChain = reinterpret_cast<PFN_D3D11CreateDeviceAndSwapChain>(pTarget);
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

            HRESULT hr = (dx11_hook_s_oRealD3D11CreateDeviceAndSwapChain ? dx11_hook_s_oRealD3D11CreateDeviceAndSwapChain
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
        int afReplaced = dx11_hook_g_DiagSamplerReplacementCreated.load(std::memory_order_relaxed);
        int afAllowed = dx11_hook_g_DiagSamplerAllowsAF.load(std::memory_order_relaxed);
        int afNoMips = dx11_hook_g_DiagSamplerSkipNoMips.load(std::memory_order_relaxed);
        int afBorder = dx11_hook_g_DiagSamplerSkipBorder.load(std::memory_order_relaxed);
        int afReduction = dx11_hook_g_DiagSamplerSkipReduction.load(std::memory_order_relaxed);
        int afComparison = dx11_hook_g_DiagSamplerSkipComparison.load(std::memory_order_relaxed);
        int afStage = dx11_hook_g_DiagSamplerSkipStage.load(std::memory_order_relaxed);
        int afNoShader = dx11_hook_g_DiagSamplerSkipNoShader.load(std::memory_order_relaxed);
        int afNoShaderMeta = dx11_hook_g_DiagSamplerSkipNoShaderMetadata.load(std::memory_order_relaxed);
        int afShaderUnused = dx11_hook_g_DiagSamplerSkipShaderUnused.load(std::memory_order_relaxed);
        int afExplicitSample = dx11_hook_g_DiagSamplerSkipExplicitSample.load(std::memory_order_relaxed);
        int afAllowLod = dx11_hook_g_DiagSamplerAllowLodSample.load(std::memory_order_relaxed);
        int afNoSRV = dx11_hook_g_DiagSamplerSkipNoSRV.load(std::memory_order_relaxed);
        int afFormat = dx11_hook_g_DiagSamplerSkipFormat.load(std::memory_order_relaxed);
        int afSingleMip = dx11_hook_g_DiagSamplerSkipSingleMip.load(std::memory_order_relaxed);
        int afNonColor = dx11_hook_g_DiagSamplerSkipNonColorResource.load(std::memory_order_relaxed);
        int afUnsafe = dx11_hook_g_DiagSamplerSkipUnsafeResource.load(std::memory_order_relaxed);
        int afRuntimeHooks = dx11_hook_g_DiagSamplerRuntimeHookInstalled.load(std::memory_order_relaxed);
        int afDrawReconcile = dx11_hook_g_DiagSamplerDrawReconcileCalls.load(std::memory_order_relaxed);
        int afReconcileSlots = dx11_hook_g_DiagSamplerReconcileSlots.load(std::memory_order_relaxed);
        int afBindDeferred = dx11_hook_g_DiagSamplerBindDeferred.load(std::memory_order_relaxed);
        int afEffectiveBindCalls = dx11_hook_g_DiagSamplerEffectiveBindCalls.load(std::memory_order_relaxed);
        int afEffectiveBinds = dx11_hook_g_DiagSamplerEffectiveBinds.load(std::memory_order_relaxed);
        int afEffectiveBindSkips = dx11_hook_g_DiagSamplerEffectiveBindSkips.load(std::memory_order_relaxed);
        int afBootstrapComplete = dx11_hook_g_DiagDeferredAFBootstrapComplete.load(std::memory_order_relaxed);
        int afBootstrapRetry = dx11_hook_g_DiagDeferredAFBootstrapRetry.load(std::memory_order_relaxed);
        int afBootstrapDisabled = dx11_hook_g_DiagDeferredAFBootstrapDisabled.load(std::memory_order_relaxed);
        int afContextVTables = dx11_hook_g_DiagD3D11ContextVTablesHooked.load(std::memory_order_relaxed);
        int afContextHookSkips = dx11_hook_g_DiagD3D11ContextHookSkips.load(std::memory_order_relaxed);
        int afDeferredContexts = dx11_hook_g_DiagCreateDeferredContext11.load(std::memory_order_relaxed);
        int afExecuteCommandLists = dx11_hook_g_DiagExecuteCommandList11.load(std::memory_order_relaxed);
        int mipBias = g_DiagSamplerMipBiasApplied.load(std::memory_order_relaxed);
        int mipOverride = g_DiagSamplerMipOverride.load(std::memory_order_relaxed);
        int prerenderFrames = dx11_hook_g_DiagPrerenderFrames.load(std::memory_order_relaxed);
        int prerenderWaits = dx11_hook_g_DiagPrerenderWaits.load(std::memory_order_relaxed);
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
        std::lock_guard<std::mutex> lock(dx11_hook_g_PrerenderMutex);
        for (auto* q : dx11_hook_g_PrerenderQueries) {
            if (q)
                q->Release();
        }
        dx11_hook_g_PrerenderQueries.clear();
        dx11_hook_g_PrerenderFrameIndex = 0;
        if (dx11_hook_g_PrerenderQueryDevice) {
            dx11_hook_g_PrerenderQueryDevice->Release();
            dx11_hook_g_PrerenderQueryDevice = nullptr;
        }
        if (dx11_hook_g_PrerenderSerialQuery10) {
            dx11_hook_g_PrerenderSerialQuery10->Release();
            dx11_hook_g_PrerenderSerialQuery10 = nullptr;
        }
        if (dx11_hook_g_PrerenderQueryDevice10) {
            dx11_hook_g_PrerenderQueryDevice10->Release();
            dx11_hook_g_PrerenderQueryDevice10 = nullptr;
        }
    }

    dx11_hook_g_DX11Capture.Cleanup();
    if (dx11_hook_g_mainRenderTargetView) {
        g_DeferredRelease.Queue(dx11_hook_g_mainRenderTargetView);
        dx11_hook_g_mainRenderTargetView = nullptr;
    }
    if (dx11_hook_g_mainRenderTargetView10) {
        g_DeferredRelease.Queue(dx11_hook_g_mainRenderTargetView10);
        dx11_hook_g_mainRenderTargetView10 = nullptr;
    }

    // Flush deferred release queue
    g_DeferredRelease.Process();
}

void DX11Hook::OnHostDisconnect() {
    // Log diagnostic summary for sampler/prerender overrides
    {
        int afApplied = g_DiagSamplerAFApplied.load(std::memory_order_relaxed);
        int afReplaced = dx11_hook_g_DiagSamplerReplacementCreated.load(std::memory_order_relaxed);
        int afAllowed = dx11_hook_g_DiagSamplerAllowsAF.load(std::memory_order_relaxed);
        int afNoMips = dx11_hook_g_DiagSamplerSkipNoMips.load(std::memory_order_relaxed);
        int afBorder = dx11_hook_g_DiagSamplerSkipBorder.load(std::memory_order_relaxed);
        int afReduction = dx11_hook_g_DiagSamplerSkipReduction.load(std::memory_order_relaxed);
        int afComparison = dx11_hook_g_DiagSamplerSkipComparison.load(std::memory_order_relaxed);
        int afStage = dx11_hook_g_DiagSamplerSkipStage.load(std::memory_order_relaxed);
        int afNoShader = dx11_hook_g_DiagSamplerSkipNoShader.load(std::memory_order_relaxed);
        int afNoShaderMeta = dx11_hook_g_DiagSamplerSkipNoShaderMetadata.load(std::memory_order_relaxed);
        int afShaderUnused = dx11_hook_g_DiagSamplerSkipShaderUnused.load(std::memory_order_relaxed);
        int afExplicitSample = dx11_hook_g_DiagSamplerSkipExplicitSample.load(std::memory_order_relaxed);
        int afAllowLod = dx11_hook_g_DiagSamplerAllowLodSample.load(std::memory_order_relaxed);
        int afNoSRV = dx11_hook_g_DiagSamplerSkipNoSRV.load(std::memory_order_relaxed);
        int afFormat = dx11_hook_g_DiagSamplerSkipFormat.load(std::memory_order_relaxed);
        int afSingleMip = dx11_hook_g_DiagSamplerSkipSingleMip.load(std::memory_order_relaxed);
        int afNonColor = dx11_hook_g_DiagSamplerSkipNonColorResource.load(std::memory_order_relaxed);
        int afUnsafe = dx11_hook_g_DiagSamplerSkipUnsafeResource.load(std::memory_order_relaxed);
        int afRuntimeHooks = dx11_hook_g_DiagSamplerRuntimeHookInstalled.load(std::memory_order_relaxed);
        int afDrawReconcile = dx11_hook_g_DiagSamplerDrawReconcileCalls.load(std::memory_order_relaxed);
        int afReconcileSlots = dx11_hook_g_DiagSamplerReconcileSlots.load(std::memory_order_relaxed);
        int afBindDeferred = dx11_hook_g_DiagSamplerBindDeferred.load(std::memory_order_relaxed);
        int afEffectiveBindCalls = dx11_hook_g_DiagSamplerEffectiveBindCalls.load(std::memory_order_relaxed);
        int afEffectiveBinds = dx11_hook_g_DiagSamplerEffectiveBinds.load(std::memory_order_relaxed);
        int afEffectiveBindSkips = dx11_hook_g_DiagSamplerEffectiveBindSkips.load(std::memory_order_relaxed);
        int afBootstrapComplete = dx11_hook_g_DiagDeferredAFBootstrapComplete.load(std::memory_order_relaxed);
        int afBootstrapRetry = dx11_hook_g_DiagDeferredAFBootstrapRetry.load(std::memory_order_relaxed);
        int afBootstrapDisabled = dx11_hook_g_DiagDeferredAFBootstrapDisabled.load(std::memory_order_relaxed);
        int afContextVTables = dx11_hook_g_DiagD3D11ContextVTablesHooked.load(std::memory_order_relaxed);
        int afContextHookSkips = dx11_hook_g_DiagD3D11ContextHookSkips.load(std::memory_order_relaxed);
        int afDeferredContexts = dx11_hook_g_DiagCreateDeferredContext11.load(std::memory_order_relaxed);
        int afExecuteCommandLists = dx11_hook_g_DiagExecuteCommandList11.load(std::memory_order_relaxed);
        int mipBias = g_DiagSamplerMipBiasApplied.load(std::memory_order_relaxed);
        int mipOverride = g_DiagSamplerMipOverride.load(std::memory_order_relaxed);
        int prerenderFrames = dx11_hook_g_DiagPrerenderFrames.load(std::memory_order_relaxed);
        int prerenderWaits = dx11_hook_g_DiagPrerenderWaits.load(std::memory_order_relaxed);
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
    dx11_hook_g_DX11Capture.Cleanup();
}
