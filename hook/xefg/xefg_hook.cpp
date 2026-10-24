#include "xefg_hook.h"

#include <atomic>
#include <mutex>
#include <unordered_map>

#include "common/logging/log_meter.h"
#include "hook/d3d12/dx12_runtime_swapchain.h"
#include "hook/fg/fg_detection.h"
#include "hook/fg/fg_presenter.h"
#include "hook/hooking/inline_hook.h"
#include "hook/metrics/perf_logger.h"
#include "hook/overlay/overlay_compat.h"
#include "hook/pacing/fps_limiter.h"
#include "hook/present/dxgi_shared.h"
#include "hook/present/present_interposer_cadence.h"
#include "hook/runtime/hook_clock.h"
#include "hook/runtime/hook_common.h"
#include "xefg_api.h"
#include "xefg_hook_internal.h"

namespace XeFGHook {
namespace {

using namespace ce::xefg_api;
using ce::fg_presenter::Presenter;

struct ContextState {
    ID3D12Device* device = nullptr;
    HWND window = nullptr;
    IDXGISwapChain* native = nullptr;  // identity only: XeFG owns its native swapchain
    void* proxy = nullptr;             // identity only: the game owns the proxy
    bool initialized = false;
    Result initResult = kSuccess;
    uint32_t initFlags = 0;
    uint32_t maxInterpolatedFrames = 0;
    bool enabled = false;  // the SDK's default: generation off until SetEnabled(1)
    uint32_t interpolatedFrames = 1;
    int32_t uiComposition = kUiCompositionDisabled;
    void* latencyContext = nullptr;
};

std::mutex g_contextsMutex;
std::unordered_map<Handle, ContextState> g_contexts;
// The context whose swapchain presents (only one XeFG proxy swapchain exists at a time).
std::atomic<Handle> g_presentingContext{nullptr};

std::atomic<uint64_t> g_applicationFrames{0};
std::atomic<uint32_t> g_lastPresentId{0};
std::atomic<bool> g_moduleHooked{false};

std::atomic<void*> g_createContext{nullptr};
std::atomic<void*> g_initFromSwapChain{nullptr};
std::atomic<void*> g_initFromSwapChainDesc{nullptr};
std::atomic<void*> g_getSwapChainPtr{nullptr};
std::atomic<void*> g_setEnabled{nullptr};
std::atomic<void*> g_setNumInterpolatedFrames{nullptr};
std::atomic<void*> g_setUiCompositionState{nullptr};
std::atomic<void*> g_setPresentId{nullptr};
std::atomic<void*> g_getLastPresentStatus{nullptr};
std::atomic<void*> g_destroy{nullptr};
std::atomic<void*> g_setLatencyReduction{nullptr};

template <typename Fn>
Fn Original(const std::atomic<void*>& slot) {
    return reinterpret_cast<Fn>(slot.load(std::memory_order_acquire));
}

// g_FGCompat sees the presenting context's state: XeFG presents the displayed frames once its native swapchain is
// registered, and generates only while enabled.
void PublishStateLocked() {
    const Handle presenting = g_presentingContext.load(std::memory_order_acquire);
    const auto it = presenting ? g_contexts.find(presenting) : g_contexts.end();
    const bool enabled = it != g_contexts.end() && it->second.initialized && it->second.enabled;
    const uint32_t frames = it != g_contexts.end() ? it->second.interpolatedFrames : 1;
    g_FGCompat.SetXeFGState(ce::fg_presenter::HasRuntimeOwnedOutputPresenter(), enabled, static_cast<int>(frames));
}

void PublishState() {
    std::lock_guard<std::mutex> lock(g_contextsMutex);
    PublishStateLocked();
}

const char* ResultName(Result result) {
    switch (result) {
        case 0:
            return "success";
        case -1:
            return "unsupported-device";
        case -2:
            return "unsupported-driver";
        case -3:
            return "uninitialized";
        case -4:
            return "invalid-argument";
        case -5:
            return "device-out-of-memory";
        case -6:
            return "device";
        default:
            return result > 0 ? "warning" : "error";
    }
}

Result Detour_D3D12CreateContext(ID3D12Device* device, Handle* outHandle) {
    const auto original = Original<PFN_D3D12CreateContext>(g_createContext);
    if (!original) {
        return -3;
    }
    const Result result = original(device, outHandle);
    const Handle handle = outHandle ? *outHandle : nullptr;
    if (result >= kSuccess && handle) {
        std::lock_guard<std::mutex> lock(g_contextsMutex);
        g_contexts[handle] = ContextState{};
        g_contexts[handle].device = device;
    }
    HookLogImportant("XeFG: CreateContext device=%p -> context=%p result=%d (%s) tid=0x%04lX", device, handle, result,
                     ResultName(result), static_cast<unsigned long>(GetCurrentThreadId()));
    return result;
}

void NoteInitResult(Handle handle, HWND window, Result result, IDXGISwapChain* native, const void* initParams,
                    const char* entry, long applicationReferences) {
    const auto* prefix = static_cast<const D3D12InitParamsPrefix*>(initParams);
    const uint32_t initFlags = prefix ? prefix->initFlags : 0;
    const uint32_t maxInterpolated = prefix ? prefix->maxInterpolatedFrames : 0;
    {
        std::lock_guard<std::mutex> lock(g_contextsMutex);
        ContextState& context = g_contexts[handle];
        context.window = window;
        context.initResult = result;
        context.initFlags = initFlags;
        context.maxInterpolatedFrames = maxInterpolated;
        context.initialized = result >= kSuccess;
        if (native) {
            context.native = native;
        }
        if (context.initialized) {
            g_presentingContext.store(handle, std::memory_order_release);
        }
        PublishStateLocked();
    }
    HookLogImportant(
        "XeFG: %s context=%p hwnd=%p result=%d (%s) native=%p appSwapchainRef=%ld initFlags=0x%X uiNotPremultiplied=%d "
        "maxInterpolatedFrames=%u%s",
        entry, handle, window, result, ResultName(result), native, applicationReferences, initFlags,
        (initFlags & kInitFlagUiTextureNotPremultiplied) ? 1 : 0, maxInterpolated,
        result >= kSuccess && !native
            ? " - CE saw no native swapchain create inside it; its presents are attributed by module identity"
        : result < kSuccess ? " - frame generation is off for this swapchain"
                            : "");
}

Result Detour_D3D12InitFromSwapChain(Handle handle, ID3D12CommandQueue* queue, const void* initParams) {
    const auto original = Original<PFN_D3D12InitFromSwapChain>(g_initFromSwapChain);
    if (!original) {
        return -3;
    }
    const auto* prefix = static_cast<const D3D12InitParamsPrefix*>(initParams);
    IDXGISwapChain* application = prefix ? prefix->applicationSwapChain : nullptr;
    HWND window = nullptr;
    long applicationReferences = -1;
    if (application) {
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(application->GetDesc(&desc))) {
            window = desc.OutputWindow;
        }
        // XeFG releases this swapchain and requires that release to be the last reference: CE lets go of its
        // own first, and logs what is left (a transient AddRef/Release pair that returns to the same count).
        DX12_ReleaseSwapchainForRuntimeTakeover(application, "xefgSwapChainD3D12InitFromSwapChain");
        applicationReferences = static_cast<long>(application->AddRef()) - 1;
        application->Release();
    }
    ce::fg_presenter::ScopedRuntimeSwapchainCreate scope(Presenter::kXeFGSwapchain, window,
                                                         reinterpret_cast<uintptr_t>(handle));
    const Result result = original(handle, queue, initParams);
    NoteInitResult(handle, window, result, scope.CreatedSwapchain(), initParams, "InitFromSwapChain",
                   applicationReferences);
    return result;
}

Result Detour_D3D12InitFromSwapChainDesc(Handle handle, HWND window, const DXGI_SWAP_CHAIN_DESC1* desc,
                                         const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fullscreenDesc,
                                         ID3D12CommandQueue* queue, IDXGIFactory2* factory, const void* initParams) {
    const auto original = Original<PFN_D3D12InitFromSwapChainDesc>(g_initFromSwapChainDesc);
    if (!original) {
        return -3;
    }
    ce::fg_presenter::ScopedRuntimeSwapchainCreate scope(Presenter::kXeFGSwapchain, window,
                                                         reinterpret_cast<uintptr_t>(handle));
    const Result result = original(handle, window, desc, fullscreenDesc, queue, factory, initParams);
    NoteInitResult(handle, window, result, scope.CreatedSwapchain(), initParams, "InitFromSwapChainDesc", -1);
    if (desc) {
        HookLog("XeFG: InitFromSwapChainDesc requested %ux%u format=%d buffers=%u flags=0x%X queue=%p windowed=%d",
                desc->Width, desc->Height, static_cast<int>(desc->Format), desc->BufferCount, desc->Flags, queue,
                fullscreenDesc ? (fullscreenDesc->Windowed ? 1 : 0) : 1);
    }
    return result;
}

Result Detour_D3D12GetSwapChainPtr(Handle handle, REFIID riid, void** swapchain) {
    const auto original = Original<PFN_D3D12GetSwapChainPtr>(g_getSwapChainPtr);
    if (!original) {
        return -3;
    }
    const Result result = original(handle, riid, swapchain);
    void* proxy = result >= kSuccess && swapchain ? *swapchain : nullptr;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_contextsMutex);
        ContextState& context = g_contexts[handle];
        changed = proxy && context.proxy != proxy;
        if (proxy) {
            context.proxy = proxy;
        }
    }
    if (changed) {
        HookLogImportant("XeFG: context=%p hands the application proxy swapchain %p", handle, proxy);
    }
    return result;
}

Result Detour_SetEnabled(Handle handle, uint32_t enabled) {
    const auto original = Original<PFN_SetEnabled>(g_setEnabled);
    if (!original) {
        return -3;
    }
    const Result result = original(handle, enabled);
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(g_contextsMutex);
        ContextState& context = g_contexts[handle];
        if (result >= kSuccess) {
            changed = context.enabled != (enabled != 0);
            context.enabled = enabled != 0;
        }
        PublishStateLocked();
    }
    static ce::log_meter::ChangeGate s_gate;
    if (const auto verdict = s_gate.Observe(ce::log_meter::FieldKey(handle, enabled, result)); verdict || changed) {
        HookLogImportant("XeFG: SetEnabled(%u) context=%p result=%d (%s)%s", enabled, handle, result,
                         ResultName(result), ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
    return result;
}

Result Detour_SetNumInterpolatedFrames(Handle handle, uint32_t frames) {
    const auto original = Original<PFN_SetNumInterpolatedFrames>(g_setNumInterpolatedFrames);
    if (!original) {
        return -3;
    }
    const Result result = original(handle, frames);
    uint32_t applied = 1;
    {
        std::lock_guard<std::mutex> lock(g_contextsMutex);
        ContextState& context = g_contexts[handle];
        if (result >= kSuccess && frames >= 1) {
            // UINT32_MAX requests the maximum the context supports.
            context.interpolatedFrames =
                frames == UINT32_MAX && context.maxInterpolatedFrames >= 1 ? context.maxInterpolatedFrames : frames;
        }
        applied = context.interpolatedFrames;
        PublishStateLocked();
    }
    static ce::log_meter::ChangeGate s_gate;
    if (const auto verdict = s_gate.Observe(ce::log_meter::FieldKey(handle, frames, result))) {
        HookLogImportant("XeFG: SetNumInterpolatedFrames(%u) context=%p result=%d (%s) -> %ux%s", frames, handle,
                         result, ResultName(result), applied + 1,
                         ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
    return result;
}

Result Detour_SetUiCompositionState(Handle handle, int32_t state) {
    const auto original = Original<PFN_SetUiCompositionState>(g_setUiCompositionState);
    if (!original) {
        return -3;
    }
    const Result result = original(handle, state);
    if (result >= kSuccess) {
        std::lock_guard<std::mutex> lock(g_contextsMutex);
        g_contexts[handle].uiComposition = state;
    }
    static ce::log_meter::ChangeGate s_gate;
    if (const auto verdict = s_gate.Observe(ce::log_meter::FieldKey(handle, state, result))) {
        HookLogImportant("XeFG: SetUiCompositionState(%s) context=%p result=%d (%s)%s",
                         state == kUiCompositionEnabled ? "enabled" : "disabled", handle, result, ResultName(result),
                         ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
    return result;
}

Result Detour_SetPresentId(Handle handle, uint32_t presentId) {
    const auto original = Original<PFN_SetPresentId>(g_setPresentId);
    if (!original) {
        return -3;
    }
    PaceApplicationFrame(handle);
    NoteApplicationFrame(handle, presentId);
    return original(handle, presentId);
}

Result Detour_GetLastPresentStatus(Handle handle, PresentStatus* status) {
    const auto original = Original<PFN_GetLastPresentStatus>(g_getLastPresentStatus);
    if (!original) {
        return -3;
    }
    const Result result = original(handle, status);
    if (result >= kSuccess && status) {
        static ce::log_meter::ChangeGate s_gate;
        if (const auto verdict = s_gate.Observe(
                ce::log_meter::FieldKey(status->framesPresented, status->frameGenResult, status->isFrameGenEnabled))) {
            HookLog("XeFG: last present queued %u frame(s) frameGenResult=%d (%s) generation=%u context=%p%s",
                    status->framesPresented, status->frameGenResult, ResultName(status->frameGenResult),
                    status->isFrameGenEnabled, handle, ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
    }
    return result;
}

Result Detour_SetLatencyReduction(Handle handle, void* latencyContext) {
    const auto original = Original<PFN_SetLatencyReduction>(g_setLatencyReduction);
    if (!original) {
        return -3;
    }
    const Result result = original(handle, latencyContext);
    {
        std::lock_guard<std::mutex> lock(g_contextsMutex);
        g_contexts[handle].latencyContext = latencyContext;
    }
    HookLogImportant("XeFG: SetLatencyReduction context=%p xell=%p result=%d (%s)", handle, latencyContext, result,
                     ResultName(result));
    return result;
}

Result Detour_Destroy(Handle handle) {
    const auto original = Original<PFN_Destroy>(g_destroy);
    if (!original) {
        return -3;
    }
    // Destroy is not thread-safe against the context's presents (XeFG SDK), so nothing presents now. CE lets go of
    // the native swapchain's buffers before XeFG releases it, so the window is free for the next swapchain.
    IDXGISwapChain* native = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_contextsMutex);
        const auto it = g_contexts.find(handle);
        native = it != g_contexts.end() ? it->second.native : nullptr;
    }
    if (native) {
        DX12_ReleaseSwapchainForRuntimeTakeover(native, "xefgSwapChainDestroy");
    }
    const Result result = original(handle);
    ce::fg_presenter::RetireOwner(reinterpret_cast<uintptr_t>(handle), "xefgSwapChainDestroy");
    {
        std::lock_guard<std::mutex> lock(g_contextsMutex);
        g_contexts.erase(handle);
        Handle expected = handle;
        g_presentingContext.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
        PublishStateLocked();
    }
    ce::fg_presenter::ResetRuntimeCadence();
    HookLogImportant("XeFG: Destroy context=%p native=%p result=%d (%s)", handle, native, result, ResultName(result));
    return result;
}

struct ExportHook {
    const char* name;
    void* detour;
    std::atomic<void*>* original;
    bool required;
};

void PublishTrampoline(void* trampoline, void* context) {
    static_cast<std::atomic<void*>*>(context)->store(trampoline, std::memory_order_release);
}

std::mutex g_installMutex;
HookedRuntimeImage g_image;

void InstallXeFGExports(HMODULE module, const char* modulePath) {
    std::lock_guard<std::mutex> lock(g_installMutex);
    if (!module || g_image.IsHooked(module)) {
        return;
    }
    if (g_image.IsOwnedByAnotherImage(module)) {
        static ce::log_meter::ChangeGate s_gate;
        if (const auto verdict = s_gate.Observe(ce::log_meter::FieldKey(module))) {
            HookLogImportant("XeFG: second image %s at %p left unobserved - the loaded image owns CE's detours%s",
                             modulePath ? modulePath : "libxess_fg.dll", module,
                             ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
        return;
    }
    if (const size_t forgotten = g_image.ForgetUnloaded()) {
        HookLogImportant("XeFG: %s mapped after an unload at %p - forgot %zu detour record(s) of the unloaded image",
                         modulePath ? modulePath : "libxess_fg.dll", module, forgotten);
    }
    g_image.Begin(module);
    const ExportHook exports[] = {
        {"xefgSwapChainD3D12CreateContext", reinterpret_cast<void*>(&Detour_D3D12CreateContext), &g_createContext, true},
        {"xefgSwapChainD3D12InitFromSwapChain", reinterpret_cast<void*>(&Detour_D3D12InitFromSwapChain),
         &g_initFromSwapChain, true},
        {"xefgSwapChainD3D12InitFromSwapChainDesc", reinterpret_cast<void*>(&Detour_D3D12InitFromSwapChainDesc),
         &g_initFromSwapChainDesc, true},
        {"xefgSwapChainD3D12GetSwapChainPtr", reinterpret_cast<void*>(&Detour_D3D12GetSwapChainPtr), &g_getSwapChainPtr,
         true},
        {"xefgSwapChainSetEnabled", reinterpret_cast<void*>(&Detour_SetEnabled), &g_setEnabled, true},
        // XeFG 1.x has no multi-frame generation and no UI composition switch.
        {"xefgSwapChainSetNumInterpolatedFrames", reinterpret_cast<void*>(&Detour_SetNumInterpolatedFrames),
         &g_setNumInterpolatedFrames, false},
        {"xefgSwapChainSetUiCompositionState", reinterpret_cast<void*>(&Detour_SetUiCompositionState),
         &g_setUiCompositionState, false},
        {"xefgSwapChainSetPresentId", reinterpret_cast<void*>(&Detour_SetPresentId), &g_setPresentId, true},
        {"xefgSwapChainGetLastPresentStatus", reinterpret_cast<void*>(&Detour_GetLastPresentStatus),
         &g_getLastPresentStatus, false},
        {"xefgSwapChainDestroy", reinterpret_cast<void*>(&Detour_Destroy), &g_destroy, true},
        {"xefgSwapChainSetLatencyReduction", reinterpret_cast<void*>(&Detour_SetLatencyReduction),
         &g_setLatencyReduction, false},
    };
    constexpr size_t kCount = sizeof(exports) / sizeof(exports[0]);
    InlineHook::PublishedHookSpec specs[kCount] = {};
    void* trampolines[kCount] = {};
    size_t queued = 0;
    size_t specIndexForExport[kCount] = {};
    for (size_t i = 0; i < kCount; ++i) {
        void* target = reinterpret_cast<void*>(GetProcAddress(module, exports[i].name));
        specIndexForExport[i] = kCount;
        if (!target) {
            if (exports[i].required) {
                HookLogImportant("XeFG: %s missing from %s - not observed", exports[i].name,
                                 modulePath ? modulePath : "libxess_fg.dll");
            }
            continue;
        }
        InlineHook::PublishedHookSpec& spec = specs[queued];
        spec.target = target;
        spec.detour = exports[i].detour;
        spec.outTrampoline = &trampolines[queued];
        spec.publisher = PublishTrampoline;
        spec.publisherContext = exports[i].original;
        // An export another tool patched first stays exactly as it wrote it (inline_hook_policy.h).
        spec.foreignEntry = InlineHook::ForeignEntryPolicy::kLeaveToOwner;
        specIndexForExport[i] = queued++;
    }
    const size_t installed = InlineHook::InstallPublishedBatch(specs, queued);
    void* targets[kCount] = {};
    for (size_t i = 0; i < queued; ++i) {
        targets[i] = specs[i].target;
    }
    g_image.NoteTargets(targets, queued);
    size_t left = 0;
    for (size_t i = 0; i < kCount; ++i) {
        const size_t index = specIndexForExport[i];
        if (index == kCount) {
            continue;
        }
        const InlineHook::PublishedHookSpec& spec = specs[index];
        if (spec.installed) {
            HookLog("XeFG: detoured %s at %p (trampoline=%p)", exports[i].name, spec.target, trampolines[index]);
        } else if (spec.leftToForeignOwner) {
            ++left;
            HookLogImportant("XeFG: left %s at %p to the component that patched it first - CE does not observe it",
                             exports[i].name, spec.target);
        } else {
            HookLogImportant("XeFG: could not detour %s at %p", exports[i].name, spec.target);
        }
    }
    g_moduleHooked.store(installed > 0, std::memory_order_release);
    HookLogImportant("XeFG: %zu of %zu exports of %s observed (left to another owner=%zu)", installed, queued,
                     modulePath ? modulePath : "libxess_fg.dll", left);
}

}  // namespace

// The FPS limiter's game-thread site under XeFG: the application's frame, right before XeFG's proxy Present takes it
// (SetPresentId precedes that Present by contract, also inside OptiScaler's own present hook). Pacing here limits the
// rendered rate and leaves output spacing to XeFG's presenter thread; XeFG's outputs pass CE's limiter while this site
// owns the cadence (ce::runtime_input_site_policy), as under FSR FG. The proxy Present is not CE's to pair an
// ApplyPostPresent() with (OptiScaler patches it), so the whole wait happens here. A game's own XeLL frame cap
// (xellSleep at the frame's start) runs in series with it: the lower cap paces, the other never waits.
void PaceApplicationFrame(Handle handle) {
    if (HookIsShuttingDown() || handle != g_presentingContext.load(std::memory_order_acquire)) {
        return;
    }
    static ce::log_meter::ChangeGate s_gate;
    if (const auto verdict = s_gate.Observe(ce::log_meter::FieldKey(handle))) {
        HookLogImportant("XeFG: SetPresentId paces the application frame (FPS limiter input site) context=%p tid=0x%04lX%s",
                         handle, static_cast<unsigned long>(GetCurrentThreadId()),
                         ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
    }
    g_SharedFpsLimiter.Apply(false, ce::fps_limiter_policy::PresentSite::kRuntimeInputPresent);
}

void NoteApplicationFrame(Handle handle, uint32_t presentId) {
    g_lastPresentId.store(presentId, std::memory_order_relaxed);
    g_applicationFrames.fetch_add(1, std::memory_order_relaxed);
    if (handle != g_presentingContext.load(std::memory_order_acquire)) {
        return;
    }
    DXGIShared::NoteRuntimeInputApplicationFrame();
    ce::present_interposer::CadenceVerdict verdict;
    if (ce::fg_presenter::NoteRuntimeApplicationFrame(PerfLogger::GetQpcUs(), &verdict)) {
        g_FGCompat.NoteXeFGCadence(verdict);
        static std::atomic<uint32_t> s_windows{0};
        const uint32_t window = s_windows.fetch_add(1, std::memory_order_relaxed) + 1;
        if (ce::log_meter::ShouldLogCadence(window, 5, 60)) {
            HookLogImportant("XeFG: output cadence window #%u - application=%.1f fps output=%.1f fps generating=%d "
                             "multiplier=%d presentId=%u",
                             window, verdict.applicationFps, verdict.outputFps, verdict.generating ? 1 : 0,
                             verdict.multiplier, presentId);
        }
    }
}

void OnModuleLoaded(HMODULE module, const char* moduleNameOrPath) {
    if (!module || !moduleNameOrPath) {
        return;
    }
    if (ce::overlay_compat::IsXeFGFrameGenerationModulePath(moduleNameOrPath)) {
        InstallXeFGExports(module, moduleNameOrPath);
    } else if (ce::overlay_compat::IsXeLLModulePath(moduleNameOrPath)) {
        InstallXeLLExports(module, moduleNameOrPath);
    }
}

void OnModuleUnloaded(const void* moduleBase, const char* moduleBaseName) {
    if (g_image.NoteUnloaded(moduleBase)) {
        g_moduleHooked.store(false, std::memory_order_release);
        HookLog("XeFG: %s at %p unloaded - its detours are forgotten before the next install",
                moduleBaseName ? moduleBaseName : "libxess_fg.dll", moduleBase);
    } else if (NoteXeLLModuleUnloaded(moduleBase)) {
        HookLog("XeLL: %s at %p unloaded - its detours are forgotten before the next install",
                moduleBaseName ? moduleBaseName : "libxell.dll", moduleBase);
    }
}

void InstallForLoadedModules() {
    if (HMODULE xefg = GetModuleHandleW(L"libxess_fg.dll")) {
        InstallXeFGExports(xefg, "libxess_fg.dll");
    }
    if (HMODULE xell = GetModuleHandleW(L"libxell.dll")) {
        InstallXeLLExports(xell, "libxell.dll");
    }
}


void NotePresenterSwapchainRegistered(IDXGISwapChain* swapchain) {
    PublishState();
    HookLog("XeFG: native swapchain %p registered as XeFG's (presenting context=%p)", swapchain,
            g_presentingContext.load(std::memory_order_acquire));
}

Status GetStatus() {
    Status status;
    status.moduleHooked = g_moduleHooked.load(std::memory_order_acquire);
    status.presentId = g_lastPresentId.load(std::memory_order_relaxed);
    status.applicationFrames = g_applicationFrames.load(std::memory_order_relaxed);
    std::lock_guard<std::mutex> lock(g_contextsMutex);
    const Handle presenting = g_presentingContext.load(std::memory_order_acquire);
    status.contextAlive = !g_contexts.empty();
    const auto it = presenting ? g_contexts.find(presenting) : g_contexts.end();
    if (it != g_contexts.end()) {
        status.initialized = it->second.initialized;
        status.enabled = it->second.enabled;
        status.interpolatedFrames = it->second.interpolatedFrames;
    }
    return status;
}

}  // namespace XeFGHook
