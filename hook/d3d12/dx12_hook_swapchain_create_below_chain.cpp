#include "dx12_hook_internal.h"

#include "dx12_factory_slot_policy.h"

// Swapchain creates that reach CE only BELOW a loaded overlay's factory hooks.
//
// With an overlay such as Steam loaded, CE leaves the factory vtable slots and the CreateSwapChainForHwnd
// entry to it (ShouldHookFactoryCreateSwapchainSlot, IsCreateSwapChainForHwndEntryForeignOwned): Steam hooks
// the functions the slots point to and skips any slot that leads out of dxgi (Witcher 3 20261001_045954).
// CE then sees creates at body hooks, after the overlay's handler, where the immediate caller is that
// handler for every swapchain - the game's own included. The originator is recovered from the stack.

namespace {

thread_local int t_belowForeignChainCreateDepth = 0;

void PublishDeepCreateSwapChainTrampoline(void* trampoline, void*) {
    dx12_hook_s_deepCreateSCTrampoline = reinterpret_cast<PFN_CreateSwapChain>(trampoline);
}

HMODULE CaptureEngineModule() {
    static HMODULE s_module = [] {
        HMODULE module = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(&CaptureEngineModule), &module);
        return module;
    }();
    return s_module;
}

ce::dx12_factory_slot::CreateSwapchainStackFrameKind ClassifyCreateSwapchainStackFrame(const void* frame,
                                                                                       char* pathOut,
                                                                                       size_t pathOutCount) {
    using Kind = ce::dx12_factory_slot::CreateSwapchainStackFrameKind;
    HMODULE module = nullptr;
    if (!TryGetModulePathFromCodeAddress(frame, pathOut, pathOutCount, &module) || !module) {
        return Kind::kNoImage;
    }
    if (module == CaptureEngineModule()) {
        return Kind::kCaptureEngine;
    }
    if (DXGIShared::IsAddressInsideSystemDXGI(frame)) {
        return Kind::kSystemDxgi;
    }
    if (ce::overlay_compat::IsThirdPartyOverlayModulePath(pathOut)) {
        return Kind::kThirdPartyOverlay;
    }
    return Kind::kOther;
}

}  // namespace

ScopedBelowForeignChainSwapchainCreate::ScopedBelowForeignChainSwapchainCreate() {
    ++t_belowForeignChainCreateDepth;
}

ScopedBelowForeignChainSwapchainCreate::~ScopedBelowForeignChainSwapchainCreate() {
    --t_belowForeignChainCreateDepth;
}

bool DX12_IsSwapchainCreateBelowForeignChain() {
    return t_belowForeignChainCreateDepth > 0;
}

CreateSwapchainForHwndCallerContext ResolveCreateSwapchainCallerBelowForeignChain() {
    using Kind = ce::dx12_factory_slot::CreateSwapchainStackFrameKind;
    constexpr USHORT kMaxFrames = 24;
    void* frames[kMaxFrames] = {};
    const USHORT frameCount = CaptureStackBackTrace(0, kMaxFrames, frames, nullptr);
    Kind kinds[kMaxFrames] = {};
    char firstForeignPath[MAX_PATH] = {};
    for (USHORT i = 0; i < frameCount; ++i) {
        char path[MAX_PATH] = {};
        kinds[i] = ClassifyCreateSwapchainStackFrame(frames[i], path, sizeof(path));
        if (kinds[i] == Kind::kThirdPartyOverlay && !firstForeignPath[0]) {
            strncpy_s(firstForeignPath, sizeof(firstForeignPath), path, _TRUNCATE);
        }
    }

    CreateSwapchainForHwndCallerContext context = {};
    // Suppressed by construction: below the chain an overlay frame on the stack is the chain itself.
    context.callerFromThirdPartyOverlay = false;
    const int originator = ce::dx12_factory_slot::SelectCreateSwapchainOriginatorFrame(kinds, frameCount);
    if (originator >= 0) {
        context.callerAddress = frames[originator];
        TryGetModulePathFromCodeAddress(context.callerAddress, context.callerModulePath,
                                        sizeof(context.callerModulePath));
        context.callerFromFFXFGModule = ce::overlay_compat::IsFFXFrameGenerationModulePath(context.callerModulePath);
    }

    static std::atomic<int> s_logCount{0};
    const int logCount = s_logCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 12 || (logCount % 256) == 0) {
        HookLogImportant("DX12: below-chain swapchain create originated in %s (frame %d of %u; overlay in chain=%s)",
                         context.callerModulePath[0] ? context.callerModulePath : "an unresolved frame", originator,
                         static_cast<unsigned>(frameCount), firstForeignPath[0] ? firstForeignPath : "none");
    }
    return context;
}

CreateSwapchainQueueCaptureEvidence BuildCreateSwapchainQueueCaptureEvidence(
    const void* callerAddress, bool callerFromThirdPartyOverlay, bool callerFromFFXFGModule,
    bool ffxFrameGenerationInStack, bool callerFromStreamlineFGModule, bool streamlineFrameGenerationInStack,
    const char* callerModulePath, const char* ffxModulePath) {
CreateSwapchainQueueCaptureEvidence evidence = {};
evidence.callerAddress = callerAddress;
evidence.callerFromThirdPartyOverlay = callerFromThirdPartyOverlay;
evidence.authoritativeFFXRuntimeCreator =
    ce::dx12_overlay_policy::ShouldTreatCreateSwapchainCallerAsAuthoritativeFFX(callerFromFFXFGModule,
                                                                                ffxFrameGenerationInStack);
evidence.authoritativeStreamlineRuntimeCreator = callerFromStreamlineFGModule || streamlineFrameGenerationInStack;
evidence.callerFromStreamlineFGModule = callerFromStreamlineFGModule;
evidence.streamlineFrameGenerationInStack = streamlineFrameGenerationInStack;
if (callerModulePath && *callerModulePath) {
    strncpy_s(evidence.callerModulePath, sizeof(evidence.callerModulePath), callerModulePath, _TRUNCATE);
}
const char* authoritativeFFXPath = (ffxModulePath && *ffxModulePath)
                                       ? ffxModulePath
                                       : (callerFromFFXFGModule && callerModulePath ? callerModulePath : nullptr);
if (authoritativeFFXPath && *authoritativeFFXPath) {
    strncpy_s(evidence.ffxModulePath, sizeof(evidence.ffxModulePath), authoritativeFFXPath, _TRUNCATE);
    evidence.officialAMDFFXRuntimeCreator = ce::ffx_api::IsOfficialAMDFFXRuntimeModuleName(authoritativeFFXPath);
}
return evidence;
}


CreateSwapchainForHwndCallerContext ResolveCreateSwapchainForHwndCallerContext() {
// A create that reached CE below a loaded overlay's factory hooks has no slot-detour context, and its
// immediate caller is that overlay for every swapchain: the originator comes from the stack.
if (!dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath[0] &&
    DX12_IsSwapchainCreateBelowForeignChain()) {
    return ResolveCreateSwapchainCallerBelowForeignChain();
}

CreateSwapchainForHwndCallerContext context = {};

char immediateCallerModulePath[MAX_PATH] = {};
const void* immediateCallerAddress = CE_RETURN_ADDRESS();
TryGetModulePathFromCodeAddress(immediateCallerAddress, immediateCallerModulePath,
                                sizeof(immediateCallerModulePath));

const char* effectiveCallerModulePath = ce::overlay_compat::GetEffectiveCreateSwapchainCallerModulePath(
    dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath, immediateCallerModulePath);
if (effectiveCallerModulePath && *effectiveCallerModulePath) {
    strncpy_s(context.callerModulePath, sizeof(context.callerModulePath), effectiveCallerModulePath, _TRUNCATE);
}

context.callerAddress = dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath[0]
                            ? dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerAddress
                            : immediateCallerAddress;
context.callerFromFFXFGModule = ce::overlay_compat::IsFFXFrameGenerationModulePath(context.callerModulePath);
context.callerFromThirdPartyOverlay = ce::overlay_compat::IsEffectiveCreateSwapchainCallerFromThirdPartyOverlay(
    dx12_hook_s_forwardedCreateSwapchainForHwndCallerContext.callerModulePath, immediateCallerModulePath);
return context;
}


bool InstallCreateSwapChainBelowChainHook(void* createSwapChainFn, size_t loadedOverlayCount) {
    if (!createSwapChainFn || !DXGIShared::IsAddressInsideSystemDXGI(createSwapChainFn)) {
        HookLogImportant("DX12: CreateSwapChain slot %p is not inside the system dxgi image - no body hook",
                         createSwapChainFn);
        return false;
    }
    const bool foreignEntry = ce::dx12_factory_slot::HasForeignEntryJump(createSwapChainFn);
    void* trampoline = InlineHook::InstallDeepHookPublished(
        createSwapChainFn, reinterpret_cast<void*>(DeepHookCreateSwapChain), PublishDeepCreateSwapChainTrampoline,
        nullptr, ce::dx12_factory_slot::CreateSwapChainForHwndBelowChainPatchSpan(foreignEntry, loadedOverlayCount));
    if (!trampoline) {
        HookLogImportant("DX12: CreateSwapChain body hook at %p refused (foreignEntry=%d) - its factory slot stays "
                         "CE's detour, so an overlay that has not hooked CreateSwapChain yet will skip it",
                         createSwapChainFn, foreignEntry ? 1 : 0);
        return false;
    }
    dx12_hook_s_realCreateSCAddr = createSwapChainFn;
    HookLog("DX12: Installed DEEP hook on CreateSwapChain at %p (trampoline=%p foreignEntry=%d)", createSwapChainFn,
            trampoline, foreignEntry ? 1 : 0);
    return true;
}

bool RemoveCreateSwapChainBelowChainHook() {
    if (!dx12_hook_s_realCreateSCAddr) {
        return true;
    }
    if (!InlineHook::RemoveDeepHook(dx12_hook_s_realCreateSCAddr)) {
        HookLogImportant("DX12: Retaining the CreateSwapChain body hook because CE could not prove sole ownership");
        return false;
    }
    dx12_hook_s_realCreateSCAddr = nullptr;
    dx12_hook_s_deepCreateSCTrampoline = nullptr;
    return true;
}

HRESULT STDMETHODCALLTYPE DeepHookCreateSwapChain(IDXGIFactory* pThis, IUnknown* pDevice, DXGI_SWAP_CHAIN_DESC* pDesc,
                                                  IDXGISwapChain** ppSwapChain) {
    const PFN_CreateSwapChain original = dx12_hook_s_deepCreateSCTrampoline;
    if (!original) {
        return E_FAIL;
    }
    if (HookIsShuttingDown() || DXGIShared::ShouldBypassSwapchainCreateForVulkan("DeepHookCreateSwapChain")) {
        return original(pThis, pDevice, pDesc, ppSwapChain);
    }
    if (DX12_IsInternalDXGISwapchainProbe()) {
        HookLog("DeepHookCreateSwapChain: Internal probe - passthrough without DX12 side-effects");
        return original(pThis, pDevice, pDesc, ppSwapChain);
    }
    HookLog("DeepHookCreateSwapChain: CALLED below the foreign chain (factory=%p, device=%p, swapEffect=%d)", pThis,
            pDevice, pDesc ? (int)pDesc->SwapEffect : -1);
    const CreateSwapchainForHwndCallerContext caller = ResolveCreateSwapchainCallerBelowForeignChain();
    return RunCreateSwapChainGlobalSemantics(original, caller, pThis, pDevice, pDesc, ppSwapChain);
}
