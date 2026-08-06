#include "dx12_hook_internal.h"


DX12Context GetDX12PrerenderContext(bool preferOriginalGameQueue, bool* usesOriginalGameQueue, ID3D12CommandQueue** currentQueueSnapshot) {
std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
ID3D12CommandQueue* currentQueue = g_CommandQueue.load(std::memory_order_acquire);
const bool useOriginalGameQueue = preferOriginalGameQueue && dx12_hook_g_OriginalGameQueue != nullptr;
ID3D12CommandQueue* selectedQueue = useOriginalGameQueue ? dx12_hook_g_OriginalGameQueue : currentQueue;
if (usesOriginalGameQueue) {
    *usesOriginalGameQueue = useOriginalGameQueue;
}
if (currentQueueSnapshot) {
    *currentQueueSnapshot = currentQueue;
}
if (!selectedQueue) {
    return {};
}

// Streamline can use a second D3D12 device. Derive the fence device from
// the selected queue instead of pairing origGame with a volatile runtime
// device pointer.
ID3D12Device* queueDevice = nullptr;
const HRESULT deviceHr = selectedQueue->GetDevice(IID_PPV_ARGS(&queueDevice));
if (FAILED(deviceHr) || !queueDevice) {
    static std::atomic<int> s_prerenderQueueDeviceLogs{0};
    if (s_prerenderQueueDeviceLogs.fetch_add(1, std::memory_order_relaxed) < 20) {
        HookLog("DX12: Prerender queue GetDevice failed queue=%p hr=0x%08X", selectedQueue, deviceHr);
    }
    return {};
}
DX12Context context(queueDevice, selectedQueue);
queueDevice->Release();
return context;
}


// Keep native driver limiters in sync with the DX12 device we already discover
// from queue/swapchain hooks. Ownership remains with the existing DX12 globals.


void DX12_PublishNativeLimiterDevice(ID3D12Device* device, ID3D12CommandQueue* queue, const char* source) {
if (!device)
    return;

g_ReflexLimiter.SetDevice(static_cast<IUnknown*>(device));

bool ctxUpdated = false;
bool ctxApiConflict = false;
if (auto* ctx = ce::GetHookContext()) {
    std::lock_guard<std::mutex> ctxLock(ctx->initMutex);
    if (!ctx->shuttingDown.load(std::memory_order_acquire)) {
        if (ctx->activeAPI == ce::ActiveGraphicsAPI::None) {
            ctx->activeAPI = ce::ActiveGraphicsAPI::DX12;
        }
        if (ctx->activeAPI == ce::ActiveGraphicsAPI::DX12) {
            ctx->graphicsData.dx12.device = device;
            ctx->graphicsData.dx12.commandQueue = queue;
            ctxUpdated = true;
        } else {
            ctxApiConflict = true;
        }
    }
}

static std::atomic<ID3D12Device*> s_lastPublishedDevice{nullptr};
static std::atomic<ID3D12CommandQueue*> s_lastPublishedQueue{nullptr};
static std::atomic<uint64_t> s_nativeLimiterPublishChangeCount{0};
ID3D12Device* previousDevice = s_lastPublishedDevice.exchange(device, std::memory_order_acq_rel);
ID3D12CommandQueue* previousQueue = s_lastPublishedQueue.exchange(queue, std::memory_order_acq_rel);
const bool deviceChanged = previousDevice != device;
const bool queueChanged = previousQueue != queue;
if (deviceChanged || queueChanged) {
    const uint64_t changeCount = s_nativeLimiterPublishChangeCount.fetch_add(1, std::memory_order_relaxed) + 1;
    if (deviceChanged || changeCount <= 32 || (changeCount % 512) == 0) {
        HookLogImportant(
            "DX12: Published native limiter device from %s (device=%p queue=%p ctxUpdated=%d ctxApiConflict=%d "
            "deviceChanged=%d queueChanged=%d changeCount=%llu)",
            source && source[0] ? source : "unknown", device, queue, ctxUpdated ? 1 : 0, ctxApiConflict ? 1 : 0,
            deviceChanged ? 1 : 0, queueChanged ? 1 : 0, static_cast<unsigned long long>(changeCount));
    }
}
}


void RememberOriginalQueueSwapchainIdentity(IDXGISwapChain* swapchain, const char* reason) {
if (!swapchain) {
    return;
}

IDXGISwapChain* previous = dx12_hook_g_LastProvenOriginalQueueSwapchain.load(std::memory_order_acquire);
if (previous != swapchain) {
    previous = dx12_hook_g_LastProvenOriginalQueueSwapchain.exchange(swapchain, std::memory_order_acq_rel);
    HookLogImportant("DX12: Remembered exact original-queue swapchain identity %p (previous=%p reason=%s)",
                     swapchain, previous, reason ? reason : "unspecified");
}

// The newest explicit association wins when one COM identity has served
// both runtime and native routes at different points in its lifetime.
IDXGISwapChain* expectedPostSLSwapchain = dx12_hook_g_LastSuccessfulPostSLSwapchain.load(std::memory_order_acquire);
if (expectedPostSLSwapchain == swapchain &&
    dx12_hook_g_LastSuccessfulPostSLSwapchain.compare_exchange_strong(expectedPostSLSwapchain, nullptr,
                                                            std::memory_order_acq_rel, std::memory_order_acquire)) {
    HookLogImportant("DX12: Original-queue association superseded remembered PostSL ownership for swapchain %p",
                     swapchain);
}
}


void UpdateLastKnownSwapchainHDRStateCache(DXGI_FORMAT format, bool isActualHDR, int swapChainColorSpace, bool presentationContractSupported) {
(void)format;
dx12_hook_g_LastKnownSwapchainColorSpace.store(swapChainColorSpace, std::memory_order_release);
dx12_hook_g_LastKnownSwapchainIsHDR.store(isActualHDR, std::memory_order_release);
dx12_hook_g_LastKnownSwapchainHDRStateValid.store(presentationContractSupported, std::memory_order_release);
}


bool IsReadableSwapchainPointer(const void* ptr) {
if (!ptr) {
    return false;
}

MEMORY_BASIC_INFORMATION mbi = {};
if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) {
    return false;
}
if (mbi.State != MEM_COMMIT) {
    return false;
}
if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) {
    return false;
}

return true;
}


bool IsExecutableCodePointer(const void* ptr) {
if (!ptr) {
    return false;
}

MEMORY_BASIC_INFORMATION mbi = {};
if (VirtualQuery(ptr, &mbi, sizeof(mbi)) == 0) {
    return false;
}
if (mbi.State != MEM_COMMIT) {
    return false;
}
if (mbi.Protect & (PAGE_NOACCESS | PAGE_GUARD)) {
    return false;
}

const DWORD executableProtection =
    PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY;
return (mbi.Protect & executableProtection) != 0;
}


void* ResolveLoadedOrLoadableExport(const char* moduleName, const char* functionName) {
HMODULE module = GetModuleHandleA(moduleName);
if (!module) {
    module = LoadLibraryExA(moduleName, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
}
return module ? reinterpret_cast<void*>(GetProcAddress(module, functionName)) : nullptr;
}


bool IsCrtPurecallFunctionPointer(const void* ptr) {
static void* s_ucrtPurecall = ResolveLoadedOrLoadableExport("ucrtbase.dll", "_purecall");
static void* s_msvcrtPurecall = ResolveLoadedOrLoadableExport("msvcrt.dll", "_purecall");
return ptr && (ptr == s_ucrtPurecall || ptr == s_msvcrtPurecall);
}


bool IsUsableStartupActivationSwapchainPointer(IDXGISwapChain* swapchain) {
if (!IsReadableSwapchainPointer(swapchain) || !IsReadableSwapchainPointer(reinterpret_cast<const void*>(*(void***)swapchain))) {
    return false;
}

void** vtable = *(void***)swapchain;
if (!vtable || !vtable[0] || !vtable[1] || !vtable[2] || !vtable[8]) {
    return false;
}

if (!IsExecutableCodePointer(vtable[0]) || !IsExecutableCodePointer(vtable[1]) ||
    !IsExecutableCodePointer(vtable[2]) || !IsExecutableCodePointer(vtable[8])) {
    return false;
}

if (IsCrtPurecallFunctionPointer(vtable[0]) || IsCrtPurecallFunctionPointer(vtable[1]) ||
    IsCrtPurecallFunctionPointer(vtable[2]) || IsCrtPurecallFunctionPointer(vtable[8])) {
    static std::atomic<int> s_purecallSwapchainRejectLogCount{0};
    const int logCount = s_purecallSwapchainRejectLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 100) == 0) {
        HookLogImportant(
            "DX12: Rejecting startup activation swapchain %p because its vtable resolves to CRT _purecall "
            "(qi=%p addRef=%p release=%p present=%p log=%d)",
            swapchain, vtable[0], vtable[1], vtable[2], vtable[8], logCount + 1);
    }
    return false;
}

return true;
}


void SetPostSLCallbackInstalled(bool installed, const char* reason) {
if (installed) {
    dx12_hook_g_PostSLCallbackExecutionEnabled.store(true, std::memory_order_release);
    if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != &PostSLOverlayRenderGated) {
        DXGIShared::g_PostSLOverlayRenderCallback.store(&PostSLOverlayRenderGated, std::memory_order_release);
        HookLogImportant("%s — installed gated PostSL callback", reason);
        ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kPostSLCallbackInstalled,
                                    reason ? reason : "SetPostSLCallbackInstalled", nullptr, nullptr,
                                    g_FGCompat.GetRuntimeMode(), g_FGCompat.IsFGActive(), false);
    }
    return;
}

// Any authoritative disable (protected FFX quiesce, FFX takeover, resize,
// shutdown, retirement itself) ends the make-before-break keep-alive: the
// keep-alive paths deliberately skip calling this, so a call here means a
// stronger teardown authority owns the transition now.
dx12_hook_g_PostSLExplicitOffKeepAlive.store(false, std::memory_order_release);
dx12_hook_g_PostSLWarmResumePreservationPending.store(false, std::memory_order_release);
dx12_hook_g_PostSLCallbackExecutionEnabled.store(false, std::memory_order_release);

if (DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_relaxed) != nullptr) {
    DXGIShared::g_PostSLOverlayRenderCallback.store(nullptr, std::memory_order_release);
    HookLogImportant("%s — disabled PostSL callback", reason);
    ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kPostSLCallbackRemoved,
                                reason ? reason : "SetPostSLCallbackInstalled", nullptr, nullptr,
                                g_FGCompat.GetRuntimeMode(), g_FGCompat.IsFGActive(), false);
}
}


bool TryGetModulePathFromCodeAddress(const void* codeAddress, char* modulePathOut, size_t modulePathOutCount, HMODULE* moduleHandleOut) {
if (moduleHandleOut) {
    *moduleHandleOut = nullptr;
}
if (modulePathOut && modulePathOutCount > 0) {
    modulePathOut[0] = '\0';
}
if (!codeAddress) {
    return false;
}

HMODULE callerModule = nullptr;
if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCSTR>(codeAddress), &callerModule) ||
    !callerModule) {
    return false;
}

if (moduleHandleOut) {
    *moduleHandleOut = callerModule;
}
if (modulePathOut && modulePathOutCount > 0) {
    GetModuleFileNameA(callerModule, modulePathOut, static_cast<DWORD>(modulePathOutCount));
}
return true;
}


// ===================== DX12 API call trace diagnostic =====================
// Gated by env CE_DX12_TRACE=1 or a flag file "ce_dx12_trace" next to the hook DLL. When enabled, logs
// caller-attributed D3D12 device/queue calls (CreateCommandQueue / CreateCommittedResource /
// CreateDescriptorHeap / CreateSwapChain[ForHwnd] / ExecuteCommandLists / Signal) so the overlay's --
// and any co-resident injected module's -- interaction with the app's D3D12 device can be inspected
// (queue usage, resource/descriptor footprint, per-frame submission/fence pattern). This is a
// diagnostic aid for focus/mode-switch and overlay-coexistence investigations. Zero impact when
// disabled: the extra vtable hooks are not installed and no logging runs.


bool Dx12TraceEnabled() {
static const bool s_enabled = []() -> bool {
    char buf[16] = {};
    DWORD n = GetEnvironmentVariableA("CE_DX12_TRACE", buf, sizeof(buf));
    if (n > 0 && n < sizeof(buf)) {
        if (buf[0] == '1' || _stricmp(buf, "on") == 0 || _stricmp(buf, "true") == 0 || _stricmp(buf, "yes") == 0) {
            return true;
        }
    }
    HMODULE self = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&Dx12TraceEnabled), &self) &&
        self) {
        char path[MAX_PATH] = {};
        DWORD len = GetModuleFileNameA(self, path, MAX_PATH);
        if (len > 0 && len < MAX_PATH) {
            for (DWORD i = len; i > 0; --i) {
                if (path[i - 1] == '\\' || path[i - 1] == '/') {
                    path[i] = '\0';
                    break;
                }
            }
            char flagPath[MAX_PATH] = {};
            _snprintf_s(flagPath, sizeof(flagPath), _TRUNCATE, "%sce_dx12_trace", path);
            HANDLE h = CreateFileA(flagPath, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                   OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
                return true;
            }
        }
    }
    return false;
}();
return s_enabled;
}


bool Dx12TraceIsInfraModule(const char* base) {
return _stricmp(base, "capture_hook_x86.dll") == 0 || _stricmp(base, "capture_hook_x64.dll") == 0 ||
       _stricmp(base, "d3d12.dll") == 0 || _stricmp(base, "d3d12core.dll") == 0 ||
       _stricmp(base, "dxgi.dll") == 0 || _strnicmp(base, "nvwgf2um", 8) == 0 ||
       _stricmp(base, "kernelbase.dll") == 0 || _stricmp(base, "kernel32.dll") == 0 ||
       _stricmp(base, "ntdll.dll") == 0 || _stricmp(base, "win32u.dll") == 0;
}


// Capture the call stack, identify the originating module (first non-infra frame), and log a compact
// module trail. The trail + the queue/resource pointers in `details` are the ground truth for analysis
// (e.g. correlate ExecuteCommandLists/Signal queue pointers with the queue a given module created).


void Dx12TraceLog(const char* api, const char* details) {
constexpr USHORT kMaxFrames = 24;
void* frames[kMaxFrames] = {};
const USHORT frameCount = CaptureStackBackTrace(1, kMaxFrames, frames, nullptr);
char originator[64] = "?";
bool foundOriginator = false;
char trail[256] = {};
size_t trailLen = 0;
char lastBase[64] = {};
for (USHORT i = 0; i < frameCount; ++i) {
    char modPath[MAX_PATH] = {};
    if (!TryGetModulePathFromCodeAddress(frames[i], modPath, sizeof(modPath))) {
        continue;  // trampoline / non-module code region
    }
    const char* slash = strrchr(modPath, '\\');
    const char* base = slash ? slash + 1 : modPath;
    if (!foundOriginator && !Dx12TraceIsInfraModule(base)) {
        _snprintf_s(originator, sizeof(originator), _TRUNCATE, "%s", base);
        foundOriginator = true;
    }
    if (_stricmp(base, lastBase) != 0) {  // collapse consecutive duplicate frames
        int w =
            _snprintf_s(trail + trailLen, sizeof(trail) - trailLen, _TRUNCATE, "%s%s", trailLen ? ">" : "", base);
        if (w > 0) {
            trailLen += static_cast<size_t>(w);
        }
        _snprintf_s(lastBase, sizeof(lastBase), _TRUNCATE, "%s", base);
    }
    if (trailLen + 16 >= sizeof(trail)) {
        break;
    }
}
HookLogImportant("DX12 TRACE: %s orig=%s | %s | trail=%s", api, originator, details ? details : "", trail);
}


void ProbeRealD3D12ECL(ID3D12Device* device) {
if (dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire))
    return;
if (!device)
    return;

// Create a temporary COMPUTE queue
D3D12_COMMAND_QUEUE_DESC desc = {};
desc.Type = D3D12_COMMAND_LIST_TYPE_COMPUTE;
desc.Priority = D3D12_COMMAND_QUEUE_PRIORITY_NORMAL;
desc.Flags = D3D12_COMMAND_QUEUE_FLAG_NONE;
desc.NodeMask = 0;

ID3D12CommandQueue* probeQueue = nullptr;
HRESULT hr = device->CreateCommandQueue(&desc, IID_PPV_ARGS(&probeQueue));
if (FAILED(hr) || !probeQueue) {
    HookLogImportant("DX12: ECL probe - COMPUTE queue creation failed (hr=0x%08X)", (unsigned)hr);
    return;
}

void** probeVtable = *(void***)probeQueue;
void* probeECL = probeVtable[10];
void* probeSignal = probeVtable[14];  // Signal is at vtable[14] on ID3D12CommandQueue

// Check which module owns the COMPUTE queue's ECL
HMODULE probeModule = nullptr;
GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                   (LPCSTR)probeECL, &probeModule);
char probeMod[MAX_PATH] = {};
if (probeModule)
    GetModuleFileNameA(probeModule, probeMod, MAX_PATH);

// Check which module owns the COMPUTE queue's Signal
HMODULE probeSignalModule = nullptr;
GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                   (LPCSTR)probeSignal, &probeSignalModule);
char probeSignalMod[MAX_PATH] = {};
if (probeSignalModule)
    GetModuleFileNameA(probeSignalModule, probeSignalMod, MAX_PATH);

// Compare with the current DIRECT queue's vtable[10] (our hooked version)
ID3D12CommandQueue* directQueue = dx12_hook_g_SwapchainQueue;
void* directECL = nullptr;
char directMod[MAX_PATH] = {};
if (directQueue) {
    void** directVtable = *(void***)directQueue;
    directECL = directVtable[10];
    HMODULE dMod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCSTR)directECL, &dMod);
    if (dMod)
        GetModuleFileNameA(dMod, directMod, MAX_PATH);
}

bool sameVtable = (probeVtable == (directQueue ? *(void***)directQueue : nullptr));
bool sameECL = (probeECL == directECL);
bool probeIsD3D12 = (strstr(probeMod, "d3d12") != nullptr || strstr(probeMod, "D3D12") != nullptr);
bool probeSignalIsD3D12 =
    (strstr(probeSignalMod, "d3d12") != nullptr || strstr(probeSignalMod, "D3D12") != nullptr);

HookLogImportant("DX12: ECL probe - COMPUTE ECL=%p (%s), DIRECT ECL=%p (%s), sameVtable=%d sameECL=%d isD3D12=%d",
                 probeECL, probeMod, directECL, directMod, sameVtable ? 1 : 0, sameECL ? 1 : 0,
                 probeIsD3D12 ? 1 : 0);

if (probeIsD3D12) {
    dx12_hook_g_RealD3D12ECL.store((ExecuteCommandListsPtr)probeECL, std::memory_order_release);
    HookLogImportant("DX12: Real D3D12 ECL found via COMPUTE probe: %p", probeECL);
}

// Probe the real D3D12 Signal from the COMPUTE queue's vtable
if (probeSignalIsD3D12 && !dx12_hook_g_RealD3D12Signal.load(std::memory_order_acquire)) {
    dx12_hook_g_RealD3D12Signal.store(reinterpret_cast<SignalPtr>(probeSignal), std::memory_order_release);
    HookLogImportant("DX12: Real D3D12 Signal found via COMPUTE probe: %p (%s)", probeSignal, probeSignalMod);
}

// Always check saved original — in GTA V both COMPUTE and DIRECT share
// the same vtable (sameECL=1) so our hook is on both, but
// oExecuteCommandLists still holds the real D3D12 function.
if (!dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire)) {
    ExecuteCommandListsPtr savedOrig = oExecuteCommandLists;
    if (savedOrig) {
        HMODULE origMod = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)savedOrig, &origMod);
        char origModName[MAX_PATH] = {};
        if (origMod)
            GetModuleFileNameA(origMod, origModName, MAX_PATH);
        bool origIsD3D12 = (strstr(origModName, "d3d12") != nullptr || strstr(origModName, "D3D12") != nullptr);
        HookLogImportant("DX12: ECL probe - saved oECL=%p (%s) isD3D12=%d", (void*)savedOrig, origModName,
                         origIsD3D12 ? 1 : 0);
        if (origIsD3D12) {
            dx12_hook_g_RealD3D12ECL.store(savedOrig, std::memory_order_release);
            HookLogImportant("DX12: Real D3D12 ECL found via saved original: %p", (void*)savedOrig);
        }
    }
}

// If still not found, try to follow the saved original's JMP chain
if (!dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire)) {
    ExecuteCommandListsPtr savedOrig = oExecuteCommandLists;
    if (savedOrig) {
        const uint8_t* fn = (const uint8_t*)savedOrig;
        void* target = nullptr;
        // Check for E9 rel32 (JMP rel32) — SL's hook might be a simple JMP
        if (fn[0] == 0xE9) {
            int32_t rel = *(const int32_t*)(fn + 1);
            target = (void*)(fn + 5 + rel);
        }
        // Check for FF 25 (JMP [rip+disp32]) — indirect JMP
        else if (fn[0] == 0xFF && fn[1] == 0x25) {
            int32_t disp = *(const int32_t*)(fn + 2);
            void** addr = (void**)(fn + 6 + disp);
            target = *addr;
        }
        // Check for 48 FF 25 (REX.W JMP [rip+disp32])
        else if (fn[0] == 0x48 && fn[1] == 0xFF && fn[2] == 0x25) {
            int32_t disp = *(const int32_t*)(fn + 3);
            void** addr = (void**)(fn + 7 + disp);
            target = *addr;
        }

        if (target) {
            HMODULE targetMod = nullptr;
            GetModuleHandleExA(
                GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                (LPCSTR)target, &targetMod);
            char targetModName[MAX_PATH] = {};
            if (targetMod)
                GetModuleFileNameA(targetMod, targetModName, MAX_PATH);
            bool isD3D12 = (strstr(targetModName, "d3d12") != nullptr || strstr(targetModName, "D3D12") != nullptr);
            HookLogImportant("DX12: ECL probe - followed JMP chain: target=%p (%s) isD3D12=%d", target,
                             targetModName, isD3D12 ? 1 : 0);
            if (isD3D12) {
                dx12_hook_g_RealD3D12ECL.store((ExecuteCommandListsPtr)target, std::memory_order_release);
                HookLogImportant("DX12: Real D3D12 ECL found via JMP chain: %p", target);
            }
        }
    }
}

if (!dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire)) {
    HookLogImportant(
        "DX12: ECL probe - FAILED to find real D3D12 ECL! "
        "Overlay will be disabled during SL FG to prevent crash");
}

probeQueue->Release();
}


ExecuteCommandListsPtr GetOriginalExecuteCommandLists(ID3D12CommandQueue* queue) {
if (!queue)
    return oExecuteCommandLists;

void** vtbl = *reinterpret_cast<void***>(queue);
if (!vtbl)
    return oExecuteCommandLists;

void** cachedVtable = dx12_hook_g_LastExecuteCommandListsVTable.load(std::memory_order_acquire);
if (cachedVtable == vtbl) {
    ExecuteCommandListsPtr cachedOriginal = dx12_hook_g_LastExecuteCommandListsOriginal.load(std::memory_order_acquire);
    if (cachedOriginal)
        return cachedOriginal;
}

ExecuteCommandListsPtr original = oExecuteCommandLists;
{
    std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_ExecuteCommandListsHookStateMutex);
    auto it = dx12_hook_g_ExecuteCommandListsOriginalByVTable.find(vtbl);
    if (it != dx12_hook_g_ExecuteCommandListsOriginalByVTable.end())
        original = it->second;
}

if (original) {
    dx12_hook_g_LastExecuteCommandListsOriginal.store(original, std::memory_order_release);
    dx12_hook_g_LastExecuteCommandListsVTable.store(vtbl, std::memory_order_release);
}
return original;
}


bool HasTrackedExecuteCommandListsOriginal(ID3D12CommandQueue* queue) {
if (!queue) {
    return false;
}

void** vtbl = *reinterpret_cast<void***>(queue);
if (!vtbl) {
    return false;
}

std::lock_guard<std::recursive_mutex> lock(dx12_hook_g_ExecuteCommandListsHookStateMutex);
return dx12_hook_g_ExecuteCommandListsOriginalByVTable.find(vtbl) != dx12_hook_g_ExecuteCommandListsOriginalByVTable.end();
}


bool HookHasSafePostFSRBootstrapPathImpl() {
if (!dx12_hook_g_HadFSRFGPhase) {
    return false;
}

const bool hasRealQueueBehindWrapper = dx12_hook_g_RealQueueBehindSLWrapper.load(std::memory_order_acquire) != nullptr;
const bool hasRealD3D12ECL = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire) != nullptr;
const bool hasSLWrapperQueue = dx12_hook_g_SLWrapperQueue.load(std::memory_order_acquire) != nullptr;
const bool wrapperBootstrapSafe = !ce::dx12_overlay_policy::ShouldDelayPostSLActivationUntilSafeBootstrapPath(
    dx12_hook_g_HadFSRFGPhase, hasRealQueueBehindWrapper, hasRealD3D12ECL, hasSLWrapperQueue);
if (wrapperBootstrapSafe) {
    return true;
}

ID3D12CommandQueue* swapchainQueue = nullptr;
ID3D12CommandQueue* commandQueue = nullptr;
ID3D12CommandQueue* originalGameQueue = nullptr;
bool hasTrackedSwapchainQueueSubmitPath = false;
{
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    swapchainQueue = dx12_hook_g_SwapchainQueue;
    commandQueue = g_CommandQueue.load(std::memory_order_acquire);
    originalGameQueue = dx12_hook_g_OriginalGameQueue;
    hasTrackedSwapchainQueueSubmitPath = HasTrackedExecuteCommandListsOriginal(swapchainQueue);
}
const bool hasRuntimeOwnedSwapchainQueue = swapchainQueue != nullptr && swapchainQueue != originalGameQueue;
const bool hasRealD3D12SubmitPath = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire) != nullptr;
const bool hasSwapchainQueueSubmitPath = hasTrackedSwapchainQueueSubmitPath || hasRealD3D12SubmitPath;
const bool commandQueueMatchesSwapchainQueue =
    commandQueue != nullptr && swapchainQueue != nullptr && commandQueue == swapchainQueue;
const bool streamlineHandoffOrActive = DXGIShared::IsStreamlineStartupHandoffPending() ||
                                       DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
const bool runtimeOwnedSwapchainBootstrapSafe =
    ce::dx12_overlay_policy::ShouldTreatRuntimeOwnedSwapchainQueueAsSafePostFSRBootstrap(
        dx12_hook_g_HadFSRFGPhase, hasRuntimeOwnedSwapchainQueue, streamlineHandoffOrActive, hasSwapchainQueueSubmitPath);
if (runtimeOwnedSwapchainBootstrapSafe &&
    !dx12_hook_g_SafePostFSRRuntimeOwnedSwapchainBootstrapLogged.exchange(true, std::memory_order_acq_rel)) {
    HookLogImportant(
        "DX12: Safe post-FSR bootstrap path available via runtime-owned Streamline swapchain queue "
        "(scQueue=%p cmdQ=%p origGame=%p realECL=%p wrapper=%p realBehindWrapper=%p trackedSubmit=%d "
        "cmdMatches=%d streamlineHandoffOrActive=%d)",
        swapchainQueue, commandQueue, originalGameQueue, (void*)dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire),
        dx12_hook_g_SLWrapperQueue.load(std::memory_order_acquire),
        dx12_hook_g_RealQueueBehindSLWrapper.load(std::memory_order_acquire), hasTrackedSwapchainQueueSubmitPath ? 1 : 0,
        commandQueueMatchesSwapchainQueue ? 1 : 0, streamlineHandoffOrActive ? 1 : 0);
} else if (hasRuntimeOwnedSwapchainQueue && streamlineHandoffOrActive && !runtimeOwnedSwapchainBootstrapSafe) {
    static std::atomic<int> s_runtimeOwnedBootstrapUnsafeLogCount{0};
    const int logCount = s_runtimeOwnedBootstrapUnsafeLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 10 || (logCount % 120) == 0) {
        HookLogImportant(
            "DX12: Runtime-owned Streamline swapchain queue not yet safe for post-FSR bootstrap "
            "(scQueue=%p cmdQ=%p origGame=%p realECL=%p trackedSubmit=%d cmdMatches=%d "
            "streamlineHandoffOrActive=%d log=%d)",
            swapchainQueue, commandQueue, originalGameQueue, (void*)dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire),
            hasTrackedSwapchainQueueSubmitPath ? 1 : 0, commandQueueMatchesSwapchainQueue ? 1 : 0,
            streamlineHandoffOrActive ? 1 : 0, logCount + 1);
    }
}
return runtimeOwnedSwapchainBootstrapSafe;
}


bool PrewarmPostSLOverlayForFreshStreamlineHandoff(IDXGISwapChain* swapChain, ID3D12CommandQueue* swapchainQueue, const char* context) {
if (!swapChain || !swapchainQueue) {
    return false;
}

DXGI_SWAP_CHAIN_DESC desc = {};
const HRESULT descHr = swapChain->GetDesc(&desc);
if (FAILED(descHr) || desc.BufferCount == 0 || desc.BufferCount > 8) {
    HookLogImportant(
        "DX12: PostSL handoff prewarm refused invalid swapchain description "
        "(source=%s sc=%p queue=%p hr=0x%08X buffers=%u)",
        context ? context : "unknown", swapChain, swapchainQueue, (unsigned)descHr, desc.BufferCount);
    return false;
}

ID3D12Device* queueDevice = nullptr;
const HRESULT deviceHr = swapchainQueue->GetDevice(IID_PPV_ARGS(&queueDevice));
IDXGISwapChain3* swapChain3 = nullptr;
const HRESULT sc3Hr = swapChain->QueryInterface(IID_PPV_ARGS(&swapChain3));
if (FAILED(deviceHr) || !queueDevice || FAILED(sc3Hr) || !swapChain3) {
    HookLogImportant(
        "DX12: PostSL handoff prewarm missing exact queue/swapchain prerequisites "
        "(source=%s sc=%p queue=%p deviceHr=0x%08X sc3Hr=0x%08X)",
        context ? context : "unknown", swapChain, swapchainQueue, (unsigned)deviceHr, (unsigned)sc3Hr);
    if (swapChain3) {
        swapChain3->Release();
    }
    if (queueDevice) {
        queueDevice->Release();
    }
    return false;
}

const ULONGLONG startedMs = GetTickCount64();
bool ready = false;
bool overlayInit = false;
bool syncInit = false;
ID3D12DescriptorHeap* rtvHeap = nullptr;
{
    std::lock_guard<std::recursive_mutex> overlayLock(dx12_hook_g_OverlayMutex);
    const HRESULT deviceReason = queueDevice->GetDeviceRemovedReason();
    if (SUCCEEDED(deviceReason)) {
        // The adapter owns only device/format-scoped objects. Reuse it even when the Streamline proxy uses a
        // different queue, then create the new swapchain RTVs and allocator/fence set without recording or
        // submitting an overlay draw. This completes before slDLSSGSetOptions(ON), so the first generated
        // Present cannot race a backend shutdown/rebuild.
        dx12_hook_g_PreserveOverlayAdapterAcrossResize.store(g_OverlayAdapter.IsInitialized(), std::memory_order_release);
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        dx12_hook_g_State.cachedWidth = desc.BufferDesc.Width;
        // NOLINTNEXTLINE(bugprone-narrowing-conversions) - intentional narrowing; value is range-bounded by the surrounding API/geometry contract
        dx12_hook_g_State.cachedHeight = desc.BufferDesc.Height;
        dx12_hook_g_State.format = desc.BufferDesc.Format;

        const bool backendReady =
            InitImGui(queueDevice, static_cast<int>(desc.BufferCount), desc.BufferDesc.Format, desc.OutputWindow);
        if (backendReady) {
            CreateRTVs(queueDevice, swapChain3, static_cast<int>(desc.BufferCount));
            if (dx12_hook_g_State.rtvDescHeap) {
                InitOverlaySync(queueDevice, static_cast<int>(desc.BufferCount), swapchainQueue);
            }
        }
        ready = backendReady && dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit && dx12_hook_g_State.rtvDescHeap && dx12_hook_g_State.cmdList &&
                !dx12_hook_g_State.allocators.empty();
        if (!ready && !dx12_hook_g_State.rtvDescHeap) {
            // Make the normal first-PostSL bootstrap retry the complete swapchain-scoped setup. Preserve the
            // warm adapter if initialization itself succeeded; InitImGui will reuse it on that retry.
            dx12_hook_g_State.overlayInit = false;
            dx12_hook_g_PreserveOverlayAdapterAcrossResize.store(g_OverlayAdapter.IsInitialized(), std::memory_order_release);
        }
        overlayInit = dx12_hook_g_State.overlayInit;
        syncInit = dx12_hook_g_State.syncInit;
        rtvHeap = dx12_hook_g_State.rtvDescHeap;
    } else {
        HookLogImportant(
            "DX12: PostSL handoff prewarm refused removed device "
            "(source=%s device=%p hr=0x%08X)",
            context ? context : "unknown", queueDevice, (unsigned)deviceReason);
    }
}

HookLogImportant(
    "DX12: PostSL handoff prewarm %s before DLSS enable "
    "(source=%s sc=%p queue=%p device=%p fmt=%d buffers=%u elapsed=%llums init=%d sync=%d rtv=%p)",
    ready ? "READY" : "INCOMPLETE", context ? context : "unknown", swapChain, swapchainQueue, queueDevice,
    static_cast<int>(desc.BufferDesc.Format), desc.BufferCount,
    static_cast<unsigned long long>(GetTickCount64() - startedMs), overlayInit ? 1 : 0, syncInit ? 1 : 0, rtvHeap);

swapChain3->Release();
queueDevice->Release();
return ready;
}


// --- CPU Prerender Limit Support (DX12) ---


bool IsSteamOverlayModulePath(const char* modulePath) {
if (!modulePath || !modulePath[0])
    return false;
return strstr(modulePath, "gameoverlayrenderer") != nullptr;
}


bool IsD3D12ModuleAddress(void* address) {
if (!address) {
    return false;
}

HMODULE module = nullptr;
if (!GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                        reinterpret_cast<LPCSTR>(address), &module) ||
    !module) {
    return false;
}

char modulePath[MAX_PATH] = {};
if (!GetModuleFileNameA(module, modulePath, MAX_PATH)) {
    return false;
}

return strstr(modulePath, "d3d12") != nullptr || strstr(modulePath, "D3D12") != nullptr;
}


bool ResolveCurrentProcessForeground(HWND* foregroundWindowOut, DWORD* foregroundPidOut) {
HWND foregroundWindow = GetForegroundWindow();
DWORD foregroundPid = 0;
bool processHasForeground = false;
if (foregroundWindow) {
    GetWindowThreadProcessId(foregroundWindow, &foregroundPid);
    processHasForeground = (foregroundPid == GetCurrentProcessId());
}
if (foregroundWindowOut) {
    *foregroundWindowOut = foregroundWindow;
}
if (foregroundPidOut) {
    *foregroundPidOut = foregroundPid;
}
return processHasForeground;
}


const char* DX12WaitResultName(DWORD waitResult) {
switch (waitResult) {
    case WAIT_OBJECT_0:
        return "signaled";

    case WAIT_TIMEOUT:
        return "timeout";
    case WAIT_ABANDONED:
        return "abandoned";
    case WAIT_FAILED:
        return "failed";
    default:
        return "unknown";
}
}
