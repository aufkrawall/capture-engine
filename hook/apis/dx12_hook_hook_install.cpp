#include "dx12_hook_internal.h"

#include "../../common/log_meter.h"
#include "../common/dx12_factory_slot_policy.h"

namespace {

void PublishCreateSwapChainForHwndTrampoline(void* trampoline, void*) {
    dx12_hook_s_oCreateSCForHwndInline = reinterpret_cast<PFN_CreateSwapChainForHwnd>(trampoline);
}

void PublishDeepCreateSwapChainForHwndTrampoline(void* trampoline, void*) {
    dx12_hook_s_deepHookTrampoline = reinterpret_cast<PFN_CreateSwapChainForHwnd>(trampoline);
}

// Create a swapchain through `factory`'s OWN CreateSwapChainForHwnd, entering NO foreign code.
//
// `dx12_hook_oCreateSwapChainForHwndGlobal` must not be called blindly here: it is the value CE
// saved from whichever factory vtable it hooked, and a proxy-wrapped factory is a different C++
// class with a different vtable. Calling its method with a real factory `this` is type confusion.
// The slot is therefore read live, and CE's saved predecessor is substituted only when the slot
// really holds CE's own detour — i.e. when it is the same vtable CE hooked.
//
// CRASH BOUNDARY (session 20260812_201336, first launch): calling this slot as found reproduced
// the documented Steam NULL-callback crash with a new stack —
// `capture_hook!CreateTempSwapChainViaFactorySlot -> RTSSHooks64 -> gameoverlayrenderer64!
// OverlayHookD3D3 -> 0x0`, DEP execute at address 0, RAX=0. Steam's overlay dispatches through
// callback slots that stay NULL until Steam has rendered on a real game swapchain, so CE must
// never enter its handler — least of all during hook install. The second launch of the same
// build survived only because Steam happened to be initialized by then; that is a race, not a
// fix. Two rules, both provable before the call:
//   * the slot must resolve into the system DXGI image (a foreign module owning the slot itself
//     is refused outright — the caller then falls back to its historical path), and
//   * a foreign ENTRY patch on the real function is skipped with a bypass trampoline rather than
//     executed. RTSS and Steam both hook by patching function code, and this temp swapchain is a
//     hidden 2x2 dummy no overlay has any business seeing.
HRESULT CreateTempSwapChainViaFactorySlot(IDXGIFactory2* factory, IUnknown* queue, HWND hwnd,
                                          const DXGI_SWAP_CHAIN_DESC1* desc, IDXGISwapChain1** out) {
    if (!factory || !out) {
        return E_FAIL;
    }
    void** vtable = *reinterpret_cast<void***>(factory);
    if (!vtable) {
        return E_FAIL;
    }
    MEMORY_BASIC_INFORMATION vtableMemory = {};
    if (VirtualQuery(reinterpret_cast<const void*>(&vtable[15]), &vtableMemory, sizeof(vtableMemory)) == 0 ||
        vtableMemory.State != MEM_COMMIT) {
        return E_FAIL;
    }
    auto slot = reinterpret_cast<PFN_CreateSwapChainForHwnd>(vtable[15]);
    if (reinterpret_cast<void*>(slot) == reinterpret_cast<void*>(DetourCreateSwapChainForHwndGlobal)) {
        slot = dx12_hook_oCreateSwapChainForHwndGlobal;
    }
    if (!slot) {
        return E_FAIL;
    }
    if (!DXGIShared::IsAddressInsideSystemDXGI(reinterpret_cast<const void*>(slot))) {
        char slotOwner[MAX_PATH] = {};
        ce::overlay_compat::TryGetModulePathFromCodeAddress(reinterpret_cast<const void*>(slot), slotOwner,
                                                            sizeof(slotOwner));
        HookLogImportant(
            "DX12: Refusing the system-DXGI temp swapchain — CreateSwapChainForHwnd slot %p belongs to %s, not the "
            "system image; entering a foreign overlay handler during hook install is the documented NULL-callback "
            "crash. Falling back to the historical temp swapchain.",
            (void*)slot, slotOwner[0] ? slotOwner : "an unresolved module");
        return E_FAIL;
    }
    if (ce::dx12_factory_slot::HasForeignEntryJump(reinterpret_cast<const void*>(slot))) {
        void* bypass = InlineHook::CreateBypassTrampoline(reinterpret_cast<void*>(slot));
        if (!bypass) {
            HookLogImportant(
                "DX12: Refusing the system-DXGI temp swapchain — CreateSwapChainForHwnd at %p carries a foreign entry "
                "patch that CE cannot bypass; running it would enter that overlay's handler",
                (void*)slot);
            return E_FAIL;
        }
        HookLogImportant(
            "DX12: Bypassing the foreign entry patch on CreateSwapChainForHwnd at %p (trampoline=%p) so the temp "
            "swapchain creation enters no overlay handler",
            (void*)slot, bypass);
        slot = reinterpret_cast<PFN_CreateSwapChainForHwnd>(bypass);
    }
    return slot(factory, queue, hwnd, desc, nullptr, nullptr, out);
}

}  // namespace


void InstallGlobalVTableHooks() {
    // Fast-app coverage: the HookThread installs these hooks as its FIRST action (before any
    // module scans / IPC waits), so a game that creates its swapchain within the first second
    // (dx12_fg_switch_test via Steam + RTSS, session 20260812_044326) cannot slip past them.
    // DX12Hook::Init retries here when dxgi.dll was not loaded yet; only a completed install
    // latches the flag.
    static std::atomic<bool> s_installed{false};
    if (s_installed.load(std::memory_order_acquire)) {
        return;
    }
HookLog("DX12: InstallGlobalVTableHooks called");

// CRITICAL: Install global factory vtable hooks to catch swapchain creation
// even for factories created before our IAT hooks were installed.
// This ensures ALL swapchains get wrapped regardless of timing.

HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
if (!hDXGI) {
    HookLog("DX12: DXGI module not loaded, skipping factory vtable hooks");
    return;
}

// Get CreateDXGIFactory1 export to create a temp factory
typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
PFN_CreateDXGIFactory1 pCreateFactory = (PFN_CreateDXGIFactory1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
if (!pCreateFactory) {
    HookLog("DX12: CreateDXGIFactory1 not found");
    return;
}

// Create a temp factory to get its vtable
IDXGIFactory2* pFactory = nullptr;
HRESULT hr = pCreateFactory(IID_PPV_ARGS(&pFactory));
if (FAILED(hr) || !pFactory) {
    HookLog("DX12: Failed to create temp factory for vtable extraction");
    return;
}

// Get the vtable - ALL IDXGIFactory instances share this vtable
void** vtable = *(void***)pFactory;
HookLog("DX12: Factory vtable at %p", vtable);

// Save the real CreateSwapChainForHwnd address BEFORE vtable patching
void* realCreateSCForHwndAddr = vtable[15];
dx12_hook_s_realCreateSCForHwndAddr = realCreateSCForHwndAddr;
// The saved slot value belongs to this exact vtable. The temp-swapchain
// installer may only invoke it with factory objects that carry this vtable
// (see dx12_factory_slot_policy.h); a proxied factory is a different class.
dx12_hook_s_savedCreateSwapChainForHwndVtable = vtable;

// Hook CreateSwapChain (vtable[10] for IDXGIFactory)
// Hook CreateSwapChainForHwnd (vtable[15] for IDXGIFactory2)
if (VTableHook::Create(reinterpret_cast<void*>(&vtable[10]), (LPVOID)DetourCreateSwapChainGlobal,
                       (LPVOID*)&dx12_hook_oCreateSwapChainGlobal) == VTableHook::Success) {
    HookLog("DX12: Hooked global CreateSwapChain at vtable[10]");
}

if (VTableHook::Create(reinterpret_cast<void*>(&vtable[15]), (LPVOID)DetourCreateSwapChainForHwndGlobal,
                       (LPVOID*)&dx12_hook_oCreateSwapChainForHwndGlobal) == VTableHook::Success) {
    HookLog("DX12: Hooked global CreateSwapChainForHwnd at vtable[15]");
}

pFactory->Release();

// Also hook IDXGIFactory4 and IDXGIFactory6 vtables to catch games that
// QueryInterface for higher factory versions (different vtable pointers).
// CreateSwapChainForHwnd is at the same slot (15) in all factory versions
// because IDXGIFactory4 inherits from IDXGIFactory3 → IDXGIFactory2.
IDXGIFactory4* pFactory4 = nullptr;
if (SUCCEEDED(pCreateFactory(IID_PPV_ARGS(&pFactory4)))) {
    void** vtable4 = *(void***)pFactory4;
    HookLog("DX12: IDXGIFactory4 available, vtable=%p (IDXGIFactory2=%p, same=%d)", vtable4, vtable,
            (int)(vtable4 == vtable));
    if (vtable4 != vtable) {  // Different vtable pointer
        VTableHook::Create(reinterpret_cast<void*>(&vtable4[10]), (LPVOID)DetourCreateSwapChainGlobal, nullptr);
        VTableHook::Create(reinterpret_cast<void*>(&vtable4[15]), (LPVOID)DetourCreateSwapChainForHwndGlobal, nullptr);
        HookLog("DX12: Hooked IDXGIFactory4 vtable[10] and vtable[15]");
    }
    pFactory4->Release();
} else {
    HookLog("DX12: IDXGIFactory4 not available");
}

IDXGIFactory6* pFactory6 = nullptr;
if (SUCCEEDED(pCreateFactory(IID_PPV_ARGS(&pFactory6)))) {
    void** vtable6 = *(void***)pFactory6;
    HookLog("DX12: IDXGIFactory6 available, vtable=%p (IDXGIFactory2=%p, same=%d)", vtable6, vtable,
            (int)(vtable6 == vtable));
    if (vtable6 != vtable) {  // Different vtable pointer
        VTableHook::Create(reinterpret_cast<void*>(&vtable6[10]), (LPVOID)DetourCreateSwapChainGlobal, nullptr);
        VTableHook::Create(reinterpret_cast<void*>(&vtable6[15]), (LPVOID)DetourCreateSwapChainForHwndGlobal, nullptr);
        HookLog("DX12: Hooked IDXGIFactory6 vtable[10] and vtable[15]");
    }
    pFactory6->Release();
} else {
    HookLog("DX12: IDXGIFactory6 not available");
}

// Install inline hook on CreateSwapChainForHwnd in dxgi.dll.
// VTable hooks only patch a single vtable and miss calls through
// Streamline's SL proxy factory (different COM vtable). Inline hooks
// patch the actual function code and catch ALL callers.
if (realCreateSCForHwndAddr && !dx12_hook_s_oCreateSCForHwndInline) {
    void* trampoline = nullptr;
    if (InlineHook::InstallPublished(realCreateSCForHwndAddr, (void*)DetourCreateSwapChainForHwndInline,
                                     &trampoline, PublishCreateSwapChainForHwndTrampoline, nullptr)) {
        HookLog("DX12: Installed INLINE hook on CreateSwapChainForHwnd at %p", realCreateSCForHwndAddr);
    } else {
        HookLog("DX12: FAILED to install inline hook on CreateSwapChainForHwnd");
    }
}

// Install DEEP hook on CreateSwapChainForHwnd.
// When Streamline hooks CreateSwapChainForHwnd at byte 0 and uses a saved
// trampoline for internal calls (bypassing both our vtable and inline hooks),
// the deep hook patches the function body past Streamline's JMP so ALL
// callers are intercepted — including Streamline's linkSwapchainToCmdQueue.
// The full wrapper pre-releases stale swapchains AND post-tracks new ones,
// ensuring SL's shadow swapchains are tracked for subsequent releases.
if (realCreateSCForHwndAddr) {
    void* trampoline = InlineHook::InstallDeepHookPublished(
        realCreateSCForHwndAddr, (void*)DeepHookCreateSwapChainForHwnd,
        PublishDeepCreateSwapChainForHwndTrampoline, nullptr);
    if (trampoline) {
        HookLog("DX12: Installed DEEP hook on CreateSwapChainForHwnd at %p (trampoline=%p)",
                realCreateSCForHwndAddr, trampoline);
    } else {
        HookLog("DX12: Deep hook not needed or failed for CreateSwapChainForHwnd");
    }
}

HookLog("DX12: Global factory vtable hooks installed");
s_installed.store(true, std::memory_order_release);
}


// Install inline hooks on Present/Present1 via temp swapchain creation.
// Inline hooks patch the function code in memory, creating a trampoline that
// bypasses the hook entirely. This solves the re-entry problem with vtable
// hooks. presentOnly: if true, only install Present hooks (defer ResizeBuffers
// for Strange Brigade)


void HookSwapchainVTableViaTempSwapchain(bool presentOnly, bool guardedSystemRouteOnly) {
HMODULE hDXGI = GetModuleHandleA("dxgi.dll");
HMODULE hD3D12 = GetModuleHandleA("d3d12.dll");
if (!hDXGI || !hD3D12)
    return;

typedef HRESULT(WINAPI * PFN_CreateDXGIFactory1)(REFIID, void**);
typedef HRESULT(WINAPI * PFN_D3D12CreateDevice)(IUnknown*, D3D_FEATURE_LEVEL, REFIID, void**);

PFN_CreateDXGIFactory1 pCreateFactory = (PFN_CreateDXGIFactory1)GetProcAddress(hDXGI, "CreateDXGIFactory1");
PFN_D3D12CreateDevice pD3D12CreateDevice = (PFN_D3D12CreateDevice)GetProcAddress(hD3D12, "D3D12CreateDevice");
if (!pCreateFactory || !pD3D12CreateDevice)
    return;

// A loader-injected factory-proxying tool (ReShade 6.8, Special K) hooks the
// CreateDXGIFactory1 export and hands callers a proxy object. The temp
// swapchain must come from the genuine dxgi factory: the historical fallback
// below invokes the raw saved slot function, which interprets its first
// argument as a CDXGIFactory. Skip the foreign entry patch so the real export
// runs; the vtable guard below still refuses any object that is not the
// factory class the saved slot was captured from.
if (ce::dx12_factory_slot::HasForeignEntryJump(reinterpret_cast<const void*>(pCreateFactory))) {
    void* bypass = InlineHook::CreateBypassTrampoline(reinterpret_cast<void*>(pCreateFactory));
    if (bypass) {
        HookLogImportant(
            "DX12: Bypassing foreign entry patch on CreateDXGIFactory1 at %p (trampoline=%p) so the temp "
            "swapchain factory is the genuine dxgi object",
            reinterpret_cast<void*>(pCreateFactory), bypass);
        pCreateFactory = reinterpret_cast<PFN_CreateDXGIFactory1>(bypass);
    } else {
        HookLogImportant(
            "DX12: Could not bypass foreign entry patch on CreateDXGIFactory1 at %p - the temp factory may be "
            "a third-party proxy",
            reinterpret_cast<void*>(pCreateFactory));
    }
}

IDXGIFactory2* pFactory = nullptr;
if (FAILED(pCreateFactory(IID_PPV_ARGS(&pFactory))) || !pFactory)
    return;

ID3D12Device* pDevice = nullptr;
if (FAILED(pD3D12CreateDevice(nullptr, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&pDevice))) || !pDevice) {
    pFactory->Release();
    return;
}

// Hook CreateSampler on the device vtable
// All D3D12 devices share the same vtable, so this hooks ALL devices
DX12_HookDeviceVTable(pDevice);

D3D12_COMMAND_QUEUE_DESC queueDesc = {};
queueDesc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
ID3D12CommandQueue* pQueue = nullptr;
if (FAILED(pDevice->CreateCommandQueue(&queueDesc, IID_PPV_ARGS(&pQueue))) || !pQueue) {
    pDevice->Release();
    pFactory->Release();
    return;
}

// Create a minimal hidden window
WNDCLASSEXW wc = {sizeof(wc)};
wc.lpfnWndProc = DefWindowProcW;
wc.hInstance = GetModuleHandleW(nullptr);
wc.lpszClassName = L"CE_Temp";
RegisterClassExW(&wc);

HWND hwnd = CreateWindowExW(0, L"CE_Temp", L"", WS_POPUP, 0, 0, 2, 2, nullptr, nullptr, wc.hInstance, nullptr);

// Create temp swapchain
DXGI_SWAP_CHAIN_DESC1 scd = {};
scd.Width = 2;
scd.Height = 2;
scd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
scd.SampleDesc.Count = 1;
scd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
scd.BufferCount = 2;
scd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;

// Mark that we're creating a temp swapchain for hook installation.
// This prevents the CreateSwapChainForHwnd hooks from capturing the temp
// queue as g_SwapchainQueue or tracking the temp swapchain.
dx12_hook_g_CreatingTempSwapchain.store(true, std::memory_order_release);

IDXGISwapChain1* pSwapChain = nullptr;
HRESULT hr = E_FAIL;
IDXGIFactory2* pTerminalFactory = nullptr;

// The vtable this temp swapchain carries decides WHERE every CE Present hook lands, and a proxy
// `dxgi.dll` in the game directory (ReShade, SpecialK, OptiScaler) makes the obvious answer the
// wrong one: it wraps the swapchain OBJECT, so slot 8 is the proxy's own Present method. A hook
// there — entry patch or deep body patch alike — runs at the START of the proxy's Present, i.e.
// BEFORE its post-processing pass and ABOVE every overlay that patched the real dxgi!Present the
// proxy forwards to. Session 20260812_195840 is exactly that: CE's deep body hook installed and
// correctly reported itself "below the foreign chain", yet Steam still drew on top, because
// Steam's patch sits on the system function further down (`no visible jump at 00007FF95309C140`
// — CE's own entry had no foreign patch at all, and `presentAddr … is in module:
// …\Talos1\Binaries\Win64\dxgi.dll`).
//
// So the temp swapchain is created from the SYSTEM dxgi factory first, which yields the terminal
// `dxgi!CDXGISwapChain::Present`. The result is accepted only when that Present really lands
// inside the system image, so a proxy that also hooks real factory vtables cannot silently put
// CE back above it — that case falls through to the historical path unchanged.
HMODULE hSystemDXGI = DXGIShared::GetSystemDXGIModuleHandle();
const bool proxyDXGILoaded = hSystemDXGI != nullptr && hSystemDXGI != hDXGI;
if (proxyDXGILoaded) {
    char proxyPath[MAX_PATH] = {};
    GetModuleFileNameA(hDXGI, proxyPath, sizeof(proxyPath));
    auto pSystemCreateFactory = (PFN_CreateDXGIFactory1)GetProcAddress(hSystemDXGI, "CreateDXGIFactory1");
    if (pSystemCreateFactory && SUCCEEDED(pSystemCreateFactory(IID_PPV_ARGS(&pTerminalFactory))) &&
        pTerminalFactory) {
        hr = CreateTempSwapChainViaFactorySlot(pTerminalFactory, pQueue, hwnd, &scd, &pSwapChain);
        if (SUCCEEDED(hr) && pSwapChain) {
            void* terminalPresent = (*reinterpret_cast<void***>(pSwapChain))[8];
            if (DXGIShared::IsAddressInsideSystemDXGI(terminalPresent)) {
                HookLogImportant(
                    "DX12: Created temp swapchain via the SYSTEM dxgi factory (Present=%p) — Present hooks target "
                    "the terminal dxgi!CDXGISwapChain::Present, below the swapchain-wrapping proxy %s",
                    terminalPresent, proxyPath[0] ? proxyPath : "dxgi.dll");
            } else {
                HookLogImportant(
                    "DX12: System-DXGI temp swapchain still resolves Present to %p outside the system image — the "
                    "proxy %s wraps real factories too; keeping the proxy-level Present view (CE's overlay stays "
                    "above that proxy)",
                    terminalPresent, proxyPath[0] ? proxyPath : "dxgi.dll");
                pSwapChain->Release();
                pSwapChain = nullptr;
                hr = E_FAIL;
            }
        } else {
            HookLogImportant("DX12: System-DXGI temp swapchain creation failed (hr=0x%08X); using the live factory",
                             hr);
        }
    }
} else if (hSystemDXGI && guardedSystemRouteOnly) {
    // No proxy dxgi.dll is loaded, so `pFactory` above ALREADY is a genuine system factory:
    // it came from the real CreateDXGIFactory1 export, with any foreign entry patch bypassed.
    //
    // What makes this route safe to run next to a third-party overlay is the guarded creation
    // itself — it refuses a CreateSwapChainForHwnd slot owned by a foreign module and steps
    // over a foreign entry patch, so no overlay handler is entered — not the presence of a
    // proxy. Requiring a proxy left the third-party-overlay deferral with NO usable route at
    // all in the ordinary no-proxy case: Cyberpunk 20260816_045933 (Steam overlay loaded
    // before the game's first D3D12 device, swapchain created after injection) retried this
    // every service pass forever, reported hr=E_FAIL because nothing had run, and never got
    // Present hooks or an overlay.
    hr = CreateTempSwapChainViaFactorySlot(pFactory, pQueue, hwnd, &scd, &pSwapChain);
    if (SUCCEEDED(hr) && pSwapChain) {
        void* terminalPresent = (*reinterpret_cast<void***>(pSwapChain))[8];
        if (DXGIShared::IsAddressInsideSystemDXGI(terminalPresent)) {
            HookLogImportant(
                "DX12: Created temp swapchain via the guarded system dxgi factory (Present=%p) — no dxgi proxy is "
                "loaded, so this already is the terminal dxgi!CDXGISwapChain::Present",
                terminalPresent);
        } else {
            HookLogImportant(
                "DX12: Guarded temp swapchain resolves Present to %p outside the system image — refusing it rather "
                "than hooking a foreign swapchain wrapper",
                terminalPresent);
            pSwapChain->Release();
            pSwapChain = nullptr;
            hr = E_FAIL;
        }
    } else {
        static std::atomic<uint32_t> s_guardedNoProxyFailures{0};
        const uint32_t failures = s_guardedNoProxyFailures.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ce::log_meter::ShouldLogCadence(failures, 5, 120)) {
            HookLogImportant("DX12: Guarded system-dxgi temp swapchain creation failed (hr=0x%08X attempt=%u)", hr,
                             failures);
        }
    }
}

// CRITICAL: Call the ORIGINAL CreateSwapChainForHwnd to get an unwrapped
// swapchain We must use oCreateSwapChainForHwndGlobal directly to bypass our
// wrapper If the original is not available, skip vtable hook installation
if (!pSwapChain && guardedSystemRouteOnly) {
    // Called from the third-party-overlay deferral. Only the system-DXGI route
    // above is provably safe there: it refuses a slot owned by a foreign module
    // and bypasses a foreign entry patch, so it enters no overlay handler. The
    // historical path below has neither guarantee and is what recursed to death
    // through Steam's NULL dispatch slots, so it stays deferred.
    static std::atomic<bool> s_guardedOnlyRefusedLogged{false};
    if (!s_guardedOnlyRefusedLogged.exchange(true, std::memory_order_acq_rel)) {
        HookLogImportant(
            "DX12: Guarded system-DXGI temp swapchain unavailable while a third-party overlay owns the "
            "creation path — leaving Present hooks deferred rather than entering the unguarded fallback");
    }
} else if (!pSwapChain) {
    const bool factoryMatchesSavedSlotVtable =
        ce::dx12_factory_slot::ShouldInvokeSavedCreateSwapChainForHwndSlot(
            static_cast<const void*>(dx12_hook_s_savedCreateSwapChainForHwndVtable), pFactory);
    if (factoryMatchesSavedSlotVtable && dx12_hook_oCreateSwapChainForHwndGlobal) {
        // Call original directly - bypasses our wrapper
        hr = dx12_hook_oCreateSwapChainForHwndGlobal(pFactory, pQueue, hwnd, &scd, nullptr, nullptr, &pSwapChain);
        if (SUCCEEDED(hr) && pSwapChain) {
            HookLog(
                "DX12: Created temp swapchain via original "
                "CreateSwapChainForHwnd (unwrapped)");
        }
    } else if (dx12_hook_oCreateSwapChainForHwndGlobal) {
        static std::atomic<bool> s_proxyFactorySkipLogged{false};
        if (!s_proxyFactorySkipLogged.exchange(true, std::memory_order_acq_rel)) {
            HookLogImportant(
                "DX12: Skipping the raw CreateSwapChainForHwnd temp-swapchain call - factory %p is a "
                "third-party proxy (vtable=%p, saved-slot vtable=%p). Passing it to the saved slot function "
                "would corrupt dxgi factory state; the real-swapchain retry paths take over.",
                (void*)pFactory, pFactory ? (void*)*reinterpret_cast<void***>(pFactory) : nullptr,
                (void*)dx12_hook_s_savedCreateSwapChainForHwndVtable);
        }
    } else {
        HookLog(
            "DX12: oCreateSwapChainForHwndGlobal not available, skipping "
            "Present vtable hooks");
    }
}

dx12_hook_g_CreatingTempSwapchain.store(false, std::memory_order_release);

if (SUCCEEDED(hr) && pSwapChain) {
    HookLog("DX12: Installing Present inline hooks via temp swapchain");
    if (DXGIShared::InstallPresentInlineHooks(pSwapChain)) {
        HookLog("DX12: Present inline hooks installed successfully");
    } else {
        HookLog("DX12: Failed to install Present inline hooks");
    }
    pSwapChain->Release();
} else {
    HookLog("DX12: Failed to create temp swapchain (hr=0x%08X)", hr);
}

// Hook ExecuteCommandLists on the temp queue's vtable.
// All DX12 command queues share the same vtable, so this hooks ALL queues
// (including the game's pre-existing queue). When ECL fires, it calls
// DX12_SetCommandQueue which captures the game's actual queue pointer.
DX12_HookQueueVTable(pQueue);

// Cleanup
if (hwnd)
    DestroyWindow(hwnd);
UnregisterClassW(L"CE_Temp", wc.hInstance);
pQueue->Release();
pDevice->Release();
if (pTerminalFactory)
    pTerminalFactory->Release();
pFactory->Release();
}
