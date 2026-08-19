#include "dx12_hook_internal.h"

#include "../common/dx12_factory_slot_policy.h"
#include "../common/swapchain_liveness.h"
#include "../wrappers/dxgi_swapchain_wrap_internal.h"

namespace {

// Set while the E_ACCESSDENIED recovery forwards a create through the live entry chain. The foreign
// entry handler's own trampoline lands back in this deep hook at +5; that nested call must run the
// genuine function below the chain exactly once without recovery so the outer recovery observes the
// foreign chain's result and can continue.
thread_local bool s_forwardingAccessDeniedCreateThroughEntryChain = false;

// Diagnostic-only: report what CE knows about the swapchains it still tracks for a HWND when DXGI
// keeps answering E_ACCESSDENIED. The tracked pointers are raw (CE must not pin a chain it wants
// DXGI to replace) and nothing removes them when a chain dies, so some of them are corpses. They are
// therefore NEVER dereferenced here: an AddRef/Release "probe" cannot be made safe on a released COM
// object — the freed heap block stays MEM_COMMIT with a plausible vtable, so the probe resurrects it
// and its Release destroys it a second time (session 20260819_000437: exactly that pattern in the
// wrapper destructor re-ran OptiScaler's proxy destructor -> STATUS_HEAP_CORRUPTION on game close).
// Instead this reports the ledger note the wrapper destructor recorded when CE dropped its last
// reference, which is the only sound observation of the residual pin count that exists.
void LogAccessDeniedSwapchainPinDiagnostics(HWND hWnd, const char* stage) {
    static std::atomic<int> s_pinDiagnosticsLogCount{0};
    const int logCount = s_pinDiagnosticsLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount >= 8 && (logCount % 64) != 0) {
        return;
    }

    std::vector<IDXGISwapChain*> chains;
    {
        std::lock_guard<std::mutex> lock(dx12_hook_s_hwndSwapchainMutex);
        const auto it = dx12_hook_s_hwndSwapchainMap.find(hWnd);
        if (it != dx12_hook_s_hwndSwapchainMap.end()) {
            chains = it->second;
        }
    }
    if (dx12_hook_g_LastSwapChain &&
        std::find(chains.begin(), chains.end(), dx12_hook_g_LastSwapChain) == chains.end()) {
        chains.push_back(dx12_hook_g_LastSwapChain);
    }

    if (chains.empty()) {
        HookLogImportant("DeepHook: E_ACCESSDENIED pin diagnostics #%d stage=%s hwnd=%p no tracked chains",
                         logCount + 1, stage ? stage : "pre-cleanup", hWnd);
        return;
    }
    for (IDXGISwapChain* chain : chains) {
        const ce::swapchain_liveness::LivenessNote note = ce::swapchain_liveness::Query(chain);
        char residualRefs[32] = "unknown";
        if (note.known) {
            snprintf(residualRefs, sizeof(residualRefs), "%lu", note.residualRefsAtCeRelease);
        }
        HookLogImportant(
            "DeepHook: E_ACCESSDENIED pin diagnostics #%d stage=%s hwnd=%p chain=%p ceReleasedLastRef=%d "
            "residualRefsAtCeRelease=%s retained=%d (raw tracked pointer, never probed)",
            logCount + 1, stage ? stage : "pre-cleanup", hWnd, (void*)chain,
            note.ceReleasedLastOwnedReference ? 1 : 0, residualRefs,
            HasRetainedStreamlineStartupActivationSwapchain() ? 1 : 0);
    }
}

}  // namespace


void CaptureSwapchainQueueFromCreateDevice(IUnknown* pDevice, IDXGISwapChain* pSwapChain, const char* context, const CreateSwapchainQueueCaptureEvidence& captureEvidence) {
if (!pDevice || !pSwapChain)
    return;

ID3D12CommandQueue* pQueue = nullptr;
HRESULT qiHr = pDevice->QueryInterface(IID_PPV_ARGS(&pQueue));
if (SUCCEEDED(qiHr) && pQueue) {
    ID3D12CommandQueue* currentOriginalGameQueue = nullptr;
    ID3D12CommandQueue* currentSwapchainQueue = nullptr;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        currentOriginalGameQueue = dx12_hook_g_OriginalGameQueue;
        currentSwapchainQueue = dx12_hook_g_SwapchainQueue;
    }
    const bool preserveCurrentGameQueue =
        ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlaySwapchainQueueCapture(
            captureEvidence.callerFromThirdPartyOverlay, currentOriginalGameQueue != nullptr,
            pQueue == currentOriginalGameQueue);
    const bool authoritativeFFXRuntimeQueue =
        ce::dx12_overlay_policy::ShouldTreatSwapchainQueueAsAuthoritativeFFXRuntime(
            captureEvidence.authoritativeFFXRuntimeCreator, currentOriginalGameQueue != nullptr,
            pQueue == currentOriginalGameQueue);
    const bool authoritativeStreamlineRuntimeQueue =
        !authoritativeFFXRuntimeQueue &&
        ce::dx12_overlay_policy::ShouldTreatSwapchainQueueAsAuthoritativeStreamlineRuntime(
            captureEvidence.authoritativeStreamlineRuntimeCreator, currentOriginalGameQueue != nullptr,
            pQueue == currentOriginalGameQueue);
    const bool freshAuthoritativeStreamlineHandoff =
        ce::dx12_overlay_policy::ShouldArmStreamlineStartupTransitionWindowForFreshAuthoritativeRuntimeQueue(
            authoritativeStreamlineRuntimeQueue, pQueue == currentSwapchainQueue);
    const bool normalSwapchainReturn = HandlePostSLRouteForNormalSwapchainReturn(
        context, pQueue, pSwapChain, currentOriginalGameQueue, captureEvidence);

    HookLogImportant("%s: QI for queue succeeded (queue=%p)", context, pQueue);
    if (preserveCurrentGameQueue) {
        HookLogImportant(
            "%s: Ignoring foreign swapchain queue %p from third-party overlay caller %s "
            "(origGame=%p) — preserving live game queue ownership",
            context, pQueue, captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "unknown",
            currentOriginalGameQueue);
        pQueue->Release();
        return;
    }
    if (authoritativeFFXRuntimeQueue && captureEvidence.authoritativeStreamlineRuntimeCreator) {
        static std::atomic<int> s_ffxOverridesStreamlineQueueAuthorityLogCount{0};
        const int logCount = s_ffxOverridesStreamlineQueueAuthorityLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20) {
            HookLogImportant(
                "%s: Authoritative FFX ownership overrides stale Streamline runtime queue authority "
                "(queue=%p caller=%s)",
                context, pQueue, captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "stack");
        }
    }
    // A caller that is neither an FG runtime, an FFX stack, nor a
    // third-party overlay is the game itself creating its swapchain. This
    // provenance is what lets explicit native-FSR OFF/destroy teardowns end
    // on game swapchain recreation instead of waiting for an origGame queue
    // return that fresh-queue games never deliver.
    const bool gameCreatedSwapchain =
        normalSwapchainReturn ||
        (!captureEvidence.callerFromThirdPartyOverlay && !captureEvidence.authoritativeFFXRuntimeCreator &&
         !captureEvidence.officialAMDFFXRuntimeCreator && !captureEvidence.authoritativeStreamlineRuntimeCreator);
    // DX12_SetSwapchainQueue publishes the queue and this exact swapchain
    // under one lock boundary, so ProcessFrame cannot observe a mismatched
    // queue/identity pair at the transition edge.
    const bool runtimeOwnershipJustActivated =
        DX12_SetSwapchainQueue(pQueue, authoritativeStreamlineRuntimeQueue, authoritativeFFXRuntimeQueue,
                               gameCreatedSwapchain, pSwapChain, normalSwapchainReturn);
    bool capturedOnOriginalQueue = false;
    {
        std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
        capturedOnOriginalQueue = gameCreatedSwapchain && dx12_hook_g_OriginalGameQueue != nullptr &&
                                  pQueue == dx12_hook_g_OriginalGameQueue && dx12_hook_g_SwapchainQueue == pQueue &&
                                  (normalSwapchainReturn || !dx12_hook_g_FGRuntimeOwnsSwapchain);
        if (capturedOnOriginalQueue) {
            RememberOriginalQueueSwapchainIdentity(pSwapChain, "CreateSwapChain original-queue association");
        }
    }
    if (freshAuthoritativeStreamlineHandoff) {
        {
            std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
            ClearStaleNativeFGPresentOwnershipForStreamlineComebackLocked(
                false, true, context ? context : "fresh authoritative Streamline handoff");
        }
        if (ce::dx12_overlay_policy::ShouldRetainStreamlineStartupActivationSwapchain(
                IsDX12Swapchain(pSwapChain), freshAuthoritativeStreamlineHandoff,
                DXGIShared::DoesFGRuntimeOwnSwapchain())) {
            DX12_RetainStreamlineStartupActivationSwapchain(pSwapChain,
                                                            "DX12: fresh authoritative Streamline handoff");
        }
        const bool retiredLiveOverlayState = InvalidatePostSLProofForFreshAuthoritativeStreamlineHandoff(
            context, pQueue, currentSwapchainQueue, currentOriginalGameQueue);
        const bool hadSuccessfulPostSLPhase = dx12_hook_g_HadSuccessfulPostSLPhase.load(std::memory_order_acquire);
        const bool prewarmPostSL = ce::dx12_overlay_policy::ShouldPrewarmPostSLOverlayAtFreshProvenHandoff(
            freshAuthoritativeStreamlineHandoff, dx12_hook_g_HadFSRFGPhase, hadSuccessfulPostSLPhase,
            DXGIShared::DoesFGRuntimeOwnSwapchain(),
            DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire), retiredLiveOverlayState,
            IsDX12Swapchain(pSwapChain));
        dx12_hook_g_PrewarmedPostSLHandoffSwapchain.store(nullptr, std::memory_order_release);
        if (prewarmPostSL) {
            const bool prewarmReady = PrewarmPostSLOverlayForFreshStreamlineHandoff(pSwapChain, pQueue, context);
            if (prewarmReady) {
                dx12_hook_g_PrewarmedPostSLHandoffSwapchain.store(pSwapChain, std::memory_order_release);
                HookLogImportant(
                    "[OVERLAY VISIBILITY] Armed exact prewarmed PostSL handoff backend for its first Present "
                    "(swapchain=%p queue=%p hadFSR=%d priorPostSL=%d)",
                    pSwapChain, pQueue, dx12_hook_g_HadFSRFGPhase ? 1 : 0, hadSuccessfulPostSLPhase ? 1 : 0);
            }
        }
        DXGIShared::g_SharedState.streamlineStartupHandoffPending.store(true, std::memory_order_release);
        DXGIShared::ArmStreamlineStartupTransitionWindow();
        StreamlineHook::OnAuthoritativeStreamlineStartupHandoff();
        ce::fg_session::EmitFGEvent(ce::fg_session::FGEventKind::kAuthoritativeStreamlineStartupHandoff,
                                    context ? context : "DX12::AuthoritativeStreamlineStartupHandoff", pQueue,
                                    pSwapChain, ce::fg_runtime::RuntimeMode::kStreamlineNoFG, false, false);
        if (ce::dx12_overlay_policy::ShouldReprobeRealD3D12ECLOnFreshAuthoritativeStreamlineHandoff(
                freshAuthoritativeStreamlineHandoff, dx12_hook_g_HadFSRFGPhase,
                dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire) != nullptr)) {
            ID3D12Device* handoffDevice = nullptr;
            const HRESULT handoffDeviceHr = pQueue->GetDevice(IID_PPV_ARGS(&handoffDevice));
            if (SUCCEEDED(handoffDeviceHr) && handoffDevice) {
                // Retain the startup-window deferral for passive method inspection;
                // the resolver itself never creates a D3D12 queue or mutates SL.
                if (!DXGIShared::IsStreamlineStartupTransitionWindowActive()) {
                    ProbeRealD3D12ECL(handoffDevice);
                    HookLogImportant(
                        "%s: Re-probed real D3D12 ECL for fresh authoritative Streamline handoff after FSR "
                        "(queue=%p realECL=%p dev=%p)",
                        context, pQueue, (void*)dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire), handoffDevice);
                } else {
                    dx12_hook_g_ProbeRealD3D12ECLDeferred.store(true, std::memory_order_release);
                    HookLogImportant(
                        "%s: Deferred realECL reprobe for fresh authoritative Streamline handoff after FSR "
                        "(queue=%p dev=%p, startup window active)",
                        context, pQueue, handoffDevice);
                }
                handoffDevice->Release();
            } else {
                HookLogImportant(
                    "%s: Failed to get handoff device for post-FSR realECL reprobe "
                    "(queue=%p hr=0x%08X)",
                    context, pQueue, (unsigned)handoffDeviceHr);
            }
        }
        HookLogImportant(
            "%s: Armed Streamline startup transition window after authoritative runtime-owned swapchain handoff "
            "(queue=%p prevScQueue=%p origGame=%p caller=%s)",
            context, pQueue, currentSwapchainQueue, currentOriginalGameQueue,
            captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "stack");
    }
    ClearStaleStreamlineOwnershipForFSRTakeover(
        captureEvidence, currentOriginalGameQueue != nullptr && pQueue != currentOriginalGameQueue,
        runtimeOwnershipJustActivated, pQueue);
    pQueue->Release();
    return;
}

// CreateSwapChainForHwnd is shared by DX10/11/12. Avoid treating arbitrary
// DXGI callers as ID3D12CommandQueue objects when QI already proved they are not.
if (IsDX12Swapchain(pSwapChain)) {
    HookLogImportant(
        "%s: DX12 swapchain created with device=%p but ID3D12CommandQueue QI failed (hr=0x%08X) — "
        "leaving swapchain queue unchanged",
        context, pDevice, qiHr);
} else {
    HookLogImportant("%s: Non-DX12 swapchain for device=%p (queue QI hr=0x%08X) — skipping queue capture", context,
                     pDevice, qiHr);
}
}


void EnsurePresentInlineHooksForRealSwapchain(IDXGISwapChain* pSwapChain, const char* source) {
if (!pSwapChain || DXGIShared::HasPresentDetourHooks()) {
    return;
}

static std::atomic<int> s_installAttemptCount{0};
const int attempt = s_installAttemptCount.fetch_add(1, std::memory_order_relaxed) + 1;
HookLog("DX12: Installing Present inline hooks via %s swapchain #%d (swapchain=%p)", source ? source : "real",
        attempt, pSwapChain);

if (!DXGIShared::InstallPresentInlineHooks(pSwapChain)) {
    HookLog("DX12: Present inline hook installation via %s swapchain failed", source ? source : "real");
    return;
}

if (DXGIShared::HasPresentInlineHooks()) {
    HookLogImportant("DX12: Present inline hooks are active via %s swapchain", source ? source : "real");
} else if (DXGIShared::HasPresentDetourHooks()) {
    HookLogImportant("DX12: Present detour hooks are active via %s swapchain (external overlay-compatible path)",
                     source ? source : "real");
} else {
    HookLog("DX12: Present inline hook installation via %s swapchain deferred to existing external hook chain",
            source ? source : "real");
}
}


void RefreshPresentHooksForRealSwapchain(IDXGISwapChain* pSwapChain, const char* source) {
if (!pSwapChain) {
    return;
}

HookLogImportant("DX12: Refreshing Present hook path via %s swapchain %p", source ? source : "real", pSwapChain);
{
    const auto& gfx = GetActiveGraphicsConfig();
    if (HasBackbufferCountOverride(gfx.backbufferCount)) {
        DXGI_SWAP_CHAIN_DESC desc = {};
        if (SUCCEEDED(pSwapChain->GetDesc(&desc))) {
            static std::atomic<int> s_backbufferLogCount{0};
            int idx = s_backbufferLogCount.fetch_add(1, std::memory_order_relaxed);
            if (idx < 24) {
                HookLogImportant(
                    "DX12: Swapchain buffer count source=%s sc=%p actual=%u requested=%d "
                    "size=%ux%u swapEffect=%d (#%d)",
                    source ? source : "real", pSwapChain, desc.BufferCount, gfx.backbufferCount,
                    desc.BufferDesc.Width, desc.BufferDesc.Height, desc.SwapEffect, idx + 1);
            }
        }
    }
}
EnsurePresentInlineHooksForRealSwapchain(pSwapChain, source);
DXGIShared::InstallHooks(pSwapChain, /*presentOnly=*/true);
DXGIShared::RepairVTableHooksIfNeeded();
}


void StartTransitionCooldown() {
LARGE_INTEGER freq, now;
QueryPerformanceFrequency(&freq);
QueryPerformanceCounter(&now);
dx12_hook_g_OverlayCooldownUntilQpc.store(now.QuadPart + freq.QuadPart * dx12_hook_kTransitionCooldownMs / 1000,
                                std::memory_order_release);
// Discard any pending deferred Signal — the queue may change during FG switch
dx12_hook_g_deferredSignalValue.store(0, std::memory_order_release);
dx12_hook_g_deferredSignalAllocIdx.store(-1, std::memory_order_release);
dx12_hook_g_deferredSignalQueue.store(nullptr, std::memory_order_release);
HookLogImportant("DX12: Overlay transition cooldown started (%lldms)", (long long)dx12_hook_kTransitionCooldownMs);
}


bool ClearStaleNativeFGPresentOwnershipForStreamlineComebackLocked(bool explicitSetOptionsActivation, bool authoritativeStreamlineHandoff, const char* source) {
const bool streamlineStartupHandoffPending =
    DXGIShared::g_SharedState.streamlineStartupHandoffPending.load(std::memory_order_acquire);
const bool runtimeOwnedNativeFGPresentPath = HookHasRuntimeOwnedNativeFGPresentPath();
const bool nativeFSRInternalNoCallbackComposition = DX12_IsNativeFSRInternalNoCallbackCompositionActive();
const bool authoritativeFSRActive = g_FGCompat.IsFSRFGApiActive();
if (!ce::dx12_overlay_policy::ShouldClearStaleNativeFGPresentOwnershipOnStreamlineComeback(
        dx12_hook_g_HadFSRFGPhase, explicitSetOptionsActivation, authoritativeStreamlineHandoff,
        authoritativeFSRActive, dx12_hook_g_SwapchainQueue != nullptr,
        dx12_hook_g_SwapchainQueue != nullptr && dx12_hook_g_SwapchainQueue != dx12_hook_g_OriginalGameQueue, streamlineStartupHandoffPending,
        runtimeOwnedNativeFGPresentPath,
        nativeFSRInternalNoCallbackComposition)) {
    return false;
}

ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
ClearOfficialFFXRuntimeOwnedPresentPathAssumption(
    "proven Streamline takeover cleared stale native-FG Present ownership");
ForceClearNativeFSRInternalNoCallbackComposition(
    "proven Streamline takeover cleared stale native-FG Present ownership");
dx12_hook_g_NativeFSRContextsDestroyedAwaitingGameSwapchain.store(false, std::memory_order_release);
dx12_hook_g_PostNativeFSROffGameSwapchainRecoveryQueue.store(nullptr, std::memory_order_release);
dx12_hook_g_ExactGameSwapchainRecoverySwapchain.store(nullptr, std::memory_order_release);
HookLogImportant(
    "DX12: Proven Streamline takeover after FSR — cleared stale native-FG Present ownership "
    "(source=%s proof=%s explicit=%d authoritativeHandoff=%d fsrApi=%d handoffPending=%d "
    "scQueue=%p origGame=%p nativeFGPath=%d noCallback=%d)",
    source ? source : "Streamline activation",
    authoritativeStreamlineHandoff ? "authoritative-handoff" : "explicit-setoptions",
    explicitSetOptionsActivation ? 1 : 0, authoritativeStreamlineHandoff ? 1 : 0,
    authoritativeFSRActive ? 1 : 0, streamlineStartupHandoffPending ? 1 : 0, dx12_hook_g_SwapchainQueue,
    dx12_hook_g_OriginalGameQueue, runtimeOwnedNativeFGPresentPath ? 1 : 0,
    nativeFSRInternalNoCallbackComposition ? 1 : 0);
return true;
}


void MarkThirdPartyOverlaySwapchain(IDXGISwapChain* pSwapChain, const char* creatorModulePath) {
DXGIShared::DX12_RegisterThirdPartyOverlaySwapchain(pSwapChain, creatorModulePath);
}


void MarkThirdPartyOverlaySwapchain(IDXGISwapChain1* pSwapChain, const char* creatorModulePath) {
MarkThirdPartyOverlaySwapchain(static_cast<IDXGISwapChain*>(pSwapChain), creatorModulePath);
}


void ForgetSwapchainFromTracking(IDXGISwapChain* pSwapChain) {
if (!pSwapChain) {
    return;
}

DXGIShared::DX12_UnregisterThirdPartyOverlaySwapchain(pSwapChain);

std::lock_guard<std::mutex> hwndLock(dx12_hook_s_hwndSwapchainMutex);
for (auto it = dx12_hook_s_hwndSwapchainMap.begin(); it != dx12_hook_s_hwndSwapchainMap.end();) {
    auto& vec = it->second;
    vec.erase(std::remove(vec.begin(), vec.end(), pSwapChain), vec.end());
    if (vec.empty()) {
        it = dx12_hook_s_hwndSwapchainMap.erase(it);
    } else {
        ++it;
    }
}
}


// Track a swapchain's HWND association (called from ProcessFrame and deep hook).
// NO AddRef — raw pointer tracking only, because pinning the chain is exactly what makes DXGI
// refuse the replacement create. Pointers therefore go stale when the chain is destroyed; the
// recovery path treats them as identities to clean up, never as objects to call into.
void TrackSwapchainHwnd(IDXGISwapChain* pSwapChain, HWND hWnd) {
if (!hWnd || !pSwapChain)
    return;
// This address holds a LIVE chain right now, so any ledger note for it describes a different,
// already-dead object that happened to sit at the same address. Drop it rather than let the
// diagnostics attribute a dead chain's residual pin count to this one.
ce::swapchain_liveness::ForgetNote(pSwapChain);
std::lock_guard<std::mutex> lock(dx12_hook_s_hwndSwapchainMutex);
auto& vec = dx12_hook_s_hwndSwapchainMap[hWnd];
for (auto* sc : vec) {
    if (sc == pSwapChain)
        return;  // Already tracked
}
vec.push_back(pSwapChain);
}


// Deep hook wrapper for CreateSwapChainForHwnd.
// Intercepts ALL callers (including Streamline's internal trampoline calls).
// Uses REACTIVE E_ACCESSDENIED recovery: tries the call first, only intervenes
// if it fails. This avoids destroying swapchains prematurely (which caused UE5
// assertion crashes when our proactive pre-check destroyed SCs that UE5's
// deferred viewport code still referenced).
HRESULT STDMETHODCALLTYPE DeepHookCreateSwapChainForHwnd(IDXGIFactory2* pThis, IUnknown* pDevice, HWND hWnd, const DXGI_SWAP_CHAIN_DESC1* pDesc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* pFDesc, IDXGIOutput* pOut, IDXGISwapChain1** ppSC) {
if (HookIsShuttingDown()) {
    if (dx12_hook_s_deepHookTrampoline)
        return dx12_hook_s_deepHookTrampoline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
    return E_FAIL;
}

// Skip side-effects for temp swapchains created during hook installation
if (dx12_hook_g_CreatingTempSwapchain.load(std::memory_order_acquire)) {
    HookLog("DeepHook: Temp swapchain creation — passthrough (no tracking)");
    return dx12_hook_s_deepHookTrampoline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
}

// Nested below-the-chain call from the foreign entry chain CE entered for an access-denied retry:
// run the genuine function exactly once without recovery so the outer recovery observes the foreign
// chain's result and can continue its own escalation.
if (s_forwardingAccessDeniedCreateThroughEntryChain) {
    if (dx12_hook_s_deepHookTrampoline) {
        return dx12_hook_s_deepHookTrampoline(pThis, pDevice, hWnd, pDesc, pFDesc, pOut, ppSC);
    }
    return E_FAIL;
}

const CreateSwapchainForHwndCallerContext callerContext = ResolveCreateSwapchainForHwndCallerContext();
const void* callerAddress = callerContext.callerAddress;
const bool callerFromFFXFGModule = callerContext.callerFromFFXFGModule;
const bool rawCallerFromThirdPartyOverlay = callerContext.callerFromThirdPartyOverlay;
const char* callerModulePath = callerContext.callerModulePath;
char ffxStackModulePath[MAX_PATH] = {};
const bool ffxFrameGenerationInStack =
    ce::overlay_compat::HasFFXFrameGenerationModuleInStack(ffxStackModulePath, sizeof(ffxStackModulePath));
const bool callerFromStreamlineFGModule =
    callerAddress && ce::overlay_compat::IsCodeAddressFromStreamlineFrameGenerationModule(callerAddress);
const bool streamlineFrameGenerationInStack = ce::overlay_compat::HasStreamlineFrameGenerationModuleInStack();
const bool callerFromThirdPartyOverlay = ShouldTreatCreateSwapchainCallerAsThirdPartyOverlay(
    "DeepHook", rawCallerFromThirdPartyOverlay, callerFromFFXFGModule, ffxFrameGenerationInStack,
    callerFromStreamlineFGModule, streamlineFrameGenerationInStack, callerModulePath);
const auto captureEvidence = BuildCreateSwapchainQueueCaptureEvidence(
    callerAddress, callerFromThirdPartyOverlay, callerFromFFXFGModule, ffxFrameGenerationInStack,
    callerFromStreamlineFGModule, streamlineFrameGenerationInStack, callerModulePath, ffxStackModulePath);

HookLogImportant("DeepHook: CreateSwapChainForHwnd ENTER factory=%p device=%p hwnd=%p BufferCount=%u SwapEffect=%d",
                 pThis, pDevice, hWnd, pDesc ? pDesc->BufferCount : 0, pDesc ? (int)pDesc->SwapEffect : -1);

// Rate-limited route diagnostics: which module reached the below-the-chain hook directly, and what
// currently lives at the CreateSwapChainForHwnd entry. A foreign overlay re-patching the entry over
// CE's prepended inline hook is visible here as a foreign caller module plus a non-CE entry jump.
{
    static std::atomic<int> s_deepEnterRouteDiagnosticsLogCount{0};
    const int routeCount = s_deepEnterRouteDiagnosticsLogCount.fetch_add(1, std::memory_order_relaxed);
    if (routeCount < 12 || (routeCount % 256) == 0) {
        char immediateModulePath[MAX_PATH] = {};
        TryGetModulePathFromCodeAddress(CE_RETURN_ADDRESS(), immediateModulePath, sizeof(immediateModulePath));
        unsigned char entryBytes[8] = {};
        if (dx12_hook_s_realCreateSCForHwndAddr) {
            memcpy(entryBytes, dx12_hook_s_realCreateSCForHwndAddr, sizeof(entryBytes));
        }
        HookLogImportant(
            "DeepHook: route diagnostics enter#%d immediateCaller=%s entry=%p bytes=%02X %02X %02X %02X %02X %02X "
            "%02X %02X",
            routeCount + 1, immediateModulePath[0] ? immediateModulePath : "unknown",
            (void*)dx12_hook_s_realCreateSCForHwndAddr, entryBytes[0], entryBytes[1], entryBytes[2], entryBytes[3],
            entryBytes[4], entryBytes[5], entryBytes[6], entryBytes[7]);
    }
}

// Apply backbuffer count override from config
DXGI_SWAP_CHAIN_DESC1 modifiedDesc;
const DXGI_SWAP_CHAIN_DESC1* pDescToUse = pDesc;
const bool applyDescriptorOverrides = ShouldApplySwapchainDescriptorOverridesForCreate(captureEvidence);
if (pDesc && !applyDescriptorOverrides) {
    LogSkippedSwapchainDescriptorOverridesForRuntimeCreate("DeepHook", captureEvidence, pDesc->BufferCount,
                                                           pDesc->Flags, pDesc->SwapEffect);
}
if (pDesc && applyDescriptorOverrides) {
    modifiedDesc = *pDesc;
    const auto& gfx = GetActiveGraphicsConfig();
    if (HasBackbufferCountOverride(gfx.backbufferCount)) {
        UINT requested = (UINT)gfx.backbufferCount;
        bool isFlip = (modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL ||
                       modifiedDesc.SwapEffect == DXGI_SWAP_EFFECT_FLIP_DISCARD);
        if (isFlip)
            modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
        if (isFlip && requested < modifiedDesc.BufferCount) {
            modifiedDesc.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
            HookLogImportant("DeepHook: Skipping BufferCount override %u < game's %u (flip model)", requested,
                             modifiedDesc.BufferCount);
        } else if (modifiedDesc.BufferCount != requested) {
            HookLogImportant("DeepHook: Overriding BufferCount %u -> %u", modifiedDesc.BufferCount, requested);
            modifiedDesc.BufferCount = requested;
        }
    }
    pDescToUse = &modifiedDesc;
}

PrepareForAuthoritativeFFXSwapchainCreate(captureEvidence, "DeepHook");

// Try the call first — let the game/SL handle SC lifecycle naturally
HRESULT hr = dx12_hook_s_deepHookTrampoline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
HookLogImportant("DeepHook: Trampoline returned hr=0x%08X sc=%p", hr, (ppSC ? *ppSC : nullptr));

const bool protectedOfficialFFXStartupCreate =
    ShouldUseProtectedOfficialFFXStartupSwapchainCreatePath(captureEvidence);
if (SUCCEEDED(hr) && ppSC && *ppSC && !callerFromThirdPartyOverlay && !protectedOfficialFFXStartupCreate) {
    // Only start transition cooldown when a swapchain recreation actually
    // succeeded. Starting it on E_ACCESSDENIED leaves the overlay in a
    // half-transitioned state while Streamline/game keeps the old chain.
    StartTransitionCooldown();
}

// Reactive recovery: if E_ACCESSDENIED, an old SC still holds the HWND.
// DON'T force-destroy — that invalidates game-held references and causes
// delayed UE5 assertion crashes. For ordinary callers we can clean up our
// overlay refs and do a very brief retry. For runtime-managed Streamline /
// authoritative FFX takeover paths, return the error untouched so the
// runtime can manage its own swapchain state machine.
if (hr == E_ACCESSDENIED && hWnd) {
    LogAccessDeniedSwapchainPinDiagnostics(hWnd, "pre-cleanup");

    const bool streamlineModuleLoaded = IsStreamlineLoaded();
    const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
    const bool streamlineStartupHandoffPending = DXGIShared::IsStreamlineStartupHandoffPending();
    const bool passThroughForRuntimeManagedFG =
        ce::dx12_overlay_policy::ShouldPassThroughCreateSwapchainAccessDeniedForStreamline(
            streamlineModuleLoaded, streamlineFGRunning, streamlineStartupHandoffPending, callerFromFFXFGModule,
            ffxFrameGenerationInStack);
    const char* passThroughReason =
        callerFromThirdPartyOverlay
            ? "third-party overlay caller"
            : ((callerFromFFXFGModule || ffxFrameGenerationInStack) ? "authoritative FFX takeover"
                                                                    : "Streamline active");
    // The below-the-chain trampoline performs the genuine DXGI create without entering the foreign
    // overlay entry chain. A foreign overlay whose CreateSwapChainForHwnd entry handler holds the old
    // swapchain until a replacement create arrives through that handler then keeps the old chain (and
    // the HWND association DXGI checks) alive, so every trampoline retry stays E_ACCESSDENIED. Retry
    // once through the live entry chain so the foreign handlers can release the old swapchain before
    // the genuine create runs; the nested below-the-chain call is guarded above and forwards straight
    // through. Only legal when the factory still carries the system DXGI vtable the saved entry slot
    // belongs to — a ReShade-style proxy factory would crash inside dxgi (sessions 20260813_004853 /
    // 20260813_004923), so that case keeps the trampoline-only retries.
    const auto retryThroughLiveEntryChain = [&](HRESULT& retryHr) -> bool {
        const bool factoryMatchesSavedSlotVtable =
            ce::dx12_factory_slot::ShouldInvokeSavedCreateSwapChainForHwndSlot(
                reinterpret_cast<const void*>(dx12_hook_s_savedCreateSwapChainForHwndVtable), pThis);
        if (!ce::dx12_overlay_policy::ShouldRetryAccessDeniedCreateThroughLiveEntryChain(
                dx12_hook_s_realCreateSCForHwndAddr != nullptr, callerFromThirdPartyOverlay,
                HookIsShuttingDown()) ||
            !factoryMatchesSavedSlotVtable || !dx12_hook_oCreateSwapChainForHwndGlobal) {
            return false;
        }
        HookLogImportant(
            "DeepHook: retrying E_ACCESSDENIED create through the live entry chain so foreign overlay swapchain "
            "handlers can release the old HWND association (hwnd=%p factory=%p caller=%s)",
            hWnd, pThis, callerModulePath[0] ? callerModulePath : "unknown");
        ScopedForwardedCreateSwapchainForHwndInlineSideEffectGuard inlineSideEffectGuard;
        ScopedForwardedCreateSwapchainForHwndCallerContext forwardedCallerContext(callerAddress, callerModulePath);
        s_forwardingAccessDeniedCreateThroughEntryChain = true;
        auto forwardingFlagReset =
            ce::make_scope_guard([]() { s_forwardingAccessDeniedCreateThroughEntryChain = false; });
        retryHr = dx12_hook_oCreateSwapChainForHwndGlobal(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
        HookLogImportant("DeepHook: live-entry-chain retry returned hr=0x%08X sc=%p inlineHandled=%d", retryHr,
                         (ppSC && *ppSC) ? (void*)*ppSC : nullptr,
                         inlineSideEffectGuard.InlineHandledForwardedCall() ? 1 : 0);
        return true;
    };
    if (ce::dx12_overlay_policy::ChooseCreateSwapchainAccessDeniedRecovery(passThroughForRuntimeManagedFG,
                                                                           callerFromThirdPartyOverlay) ==
        ce::dx12_overlay_policy::CreateSwapchainAccessDeniedRecovery::kMinimalCEReleaseThenEscalate) {
        // See the INLINE variant: a failed runtime-managed create is fatal to the game (null swapchain
        // deref, session 20260702_092933). Minimal CE unpin (retained startup-activation swapchain) +
        // retry first; full overlay cleanup only as the last resort before returning the error.
        HookLogImportant(
            "DeepHook: E_ACCESSDENIED for HWND=%p — %s, minimal CE unpin + retry "
            "(slFG=%d startupPending=%d callerFFX=%d stackFFX=%d retained=%d module=%s)",
            hWnd, passThroughReason, streamlineFGRunning ? 1 : 0, streamlineStartupHandoffPending ? 1 : 0,
            callerFromFFXFGModule ? 1 : 0, ffxFrameGenerationInStack ? 1 : 0,
            HasRetainedStreamlineStartupActivationSwapchain() ? 1 : 0,
            callerModulePath[0] ? callerModulePath : "unknown");
        ReleaseStreamlineStartupActivationSwapchain("DeepHook: E_ACCESSDENIED runtime-managed minimal recovery");
        for (int attempt = 1; attempt <= 5 && hr == E_ACCESSDENIED; ++attempt) {
            Sleep(20);
            hr = dx12_hook_s_deepHookTrampoline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
            if (SUCCEEDED(hr)) {
                HookLogImportant("DeepHook: runtime-managed minimal-recovery retry %d succeeded hr=0x%08X sc=%p",
                                 attempt, hr, (ppSC && *ppSC) ? (void*)*ppSC : nullptr);
            }
        }
        if (hr == E_ACCESSDENIED) {
            HookLogImportant(
                "DeepHook: runtime-managed minimal recovery still E_ACCESSDENIED — escalating to full overlay "
                "cleanup for HWND=%p",
                hWnd);
            {
                std::lock_guard<std::recursive_mutex> overlayLock(dx12_hook_g_OverlayMutex);
                dx12_hook_g_LastSwapChain = nullptr;
                CleanupOverlay();
                CleanupRTVs();
                dx12_hook_g_State.overlayInit = false;
            }
            {
                std::lock_guard<std::mutex> lock(dx12_hook_s_hwndSwapchainMutex);
                dx12_hook_s_hwndSwapchainMap.erase(hWnd);
            }
            if (ppSC && *ppSC) {
                ForgetSwapchainFromTracking(static_cast<IDXGISwapChain*>(*ppSC));
            }
            LogAccessDeniedSwapchainPinDiagnostics(hWnd, "post-cleanup");
            HRESULT entryChainRetryHr = E_FAIL;
            if (retryThroughLiveEntryChain(entryChainRetryHr) && SUCCEEDED(entryChainRetryHr)) {
                hr = entryChainRetryHr;
            }
            if (hr == E_ACCESSDENIED) {
                LogAccessDeniedSwapchainPinDiagnostics(hWnd, "post-entry-retry");
            }
            for (int attempt = 1; attempt <= 10 && hr == E_ACCESSDENIED; ++attempt) {
                Sleep(20);
                hr = dx12_hook_s_deepHookTrampoline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
                if (SUCCEEDED(hr)) {
                    HookLogImportant("DeepHook: escalated full-cleanup retry %d succeeded hr=0x%08X", attempt, hr);
                }
            }
        }
        if (hr == E_ACCESSDENIED) {
            HookLogImportant(
                "DeepHook: E_ACCESSDENIED persists after CE unpin + full cleanup — returning the error to the "
                "caller (HWND=%p)",
                hWnd);
            CaptureCreateSwapchainAccessDeniedExhaustedDump(hWnd, "DeepHook minimal-recovery escalation");
        }
    } else {
        HookLogImportant(
            "DeepHook: E_ACCESSDENIED for HWND=%p — cleaning up overlay refs "
            "(slLoaded=%d slFG=%d startupPending=%d callerFFX=%d stackFFX=%d module=%s)",
            hWnd, streamlineModuleLoaded ? 1 : 0, streamlineFGRunning ? 1 : 0,
            streamlineStartupHandoffPending ? 1 : 0, callerFromFFXFGModule ? 1 : 0,
            ffxFrameGenerationInStack ? 1 : 0, callerModulePath[0] ? callerModulePath : "unknown");

        // Clean up ALL overlay resources so we don't hold stale refs that
        // prevent DXGI from releasing the HWND association.  Must match the
        // cleanup sequence in DX12_OnSwapchainResizeBegin which successfully
        // avoids E_ACCESSDENIED before ResizeBuffers.
        {
            std::lock_guard<std::recursive_mutex> overlayLock(dx12_hook_g_OverlayMutex);
            dx12_hook_g_LastSwapChain = nullptr;
            CleanupOverlay();
            CleanupRTVs();
            dx12_hook_g_State.overlayInit = false;
        }
        // The retained Streamline startup-activation swapchain is an
        // AddRef'd swapchain reference; while CE pins it, DXGI refuses a
        // new swapchain on the same HWND (session 20260613_032326: the
        // app's native recreate after DLSS->OFF failed E_ACCESSDENIED
        // through all retries and stopped its main loop).
        ReleaseStreamlineStartupActivationSwapchain("DeepHook: CreateSwapChainForHwnd E_ACCESSDENIED recovery");
        HookLogImportant("DeepHook: Released overlay + RTV refs for HWND=%p", hWnd);

        // Clear our tracking entries (raw pointers, no Release needed)
        {
            std::lock_guard<std::mutex> lock(dx12_hook_s_hwndSwapchainMutex);
            dx12_hook_s_hwndSwapchainMap.erase(hWnd);
        }
        if (ppSC && *ppSC) {
            ForgetSwapchainFromTracking(static_cast<IDXGISwapChain*>(*ppSC));
        }

        LogAccessDeniedSwapchainPinDiagnostics(hWnd, "post-cleanup");
        HRESULT entryChainRetryHr = E_FAIL;
        if (retryThroughLiveEntryChain(entryChainRetryHr) && SUCCEEDED(entryChainRetryHr)) {
            hr = entryChainRetryHr;
        }
        if (hr == E_ACCESSDENIED) {
            LogAccessDeniedSwapchainPinDiagnostics(hWnd, "post-entry-retry");
        }

        // Retry: 10 attempts × 20ms = 200ms max.  FSR FG activation may
        // need time for the game to release its own swapchain refs after
        // we've released ours.
        for (int attempt = 1; attempt <= 10 && hr == E_ACCESSDENIED; ++attempt) {
            Sleep(20);
            hr = dx12_hook_s_deepHookTrampoline(pThis, pDevice, hWnd, pDescToUse, pFDesc, pOut, ppSC);
            if (SUCCEEDED(hr)) {
                HookLogImportant("DeepHook: Retry attempt %d succeeded hr=0x%08X sc=%p", attempt, hr,
                                 (ppSC ? *ppSC : nullptr));
                break;
            }
        }
        if (FAILED(hr)) {
            HookLogImportant("DeepHook: All retries exhausted — returning E_ACCESSDENIED to caller (HWND=%p)",
                             hWnd);
            CaptureCreateSwapchainAccessDeniedExhaustedDump(hWnd, "DeepHook full-cleanup recovery");
        }
    }
}

// Post-track: record the new swapchain for future reactive recovery
if (SUCCEEDED(hr) && ppSC && *ppSC && hWnd) {
    if (callerFromThirdPartyOverlay) {
        MarkThirdPartyOverlaySwapchain(*ppSC, callerModulePath);
        HookLogImportant(
            "DeepHook: Third-party overlay caller %s created swapchain %p for HWND=%p — leaving CE queue and "
            "transition state unchanged",
            callerModulePath[0] ? callerModulePath : "unknown", *ppSC, hWnd);
        return hr;
    }
    IDXGISwapChain* newSC = static_cast<IDXGISwapChain*>(*ppSC);
    if (ShouldBypassInvisibleWindowCreateSwapchainSideEffects(hWnd, newSC, "DeepHook", hr)) {
        return hr;
    }
    if (HandleProtectedOfficialFFXStartupSwapchainCreate(captureEvidence, pDevice, newSC, "DeepHook")) {
        return hr;
    }

    TrackSwapchainHwnd(*ppSC, hWnd);
    HookLogImportant("DeepHook: Created & tracked swapchain %p for HWND=%p", *ppSC, hWnd);

    // SL (or game) just created a new swapchain. Refresh the full Present
    // hook path on it — the new swapchain may expose a different Present
    // implementation or vtable than the one we initially hooked.
    RefreshPresentHooksForRealSwapchain(newSC, "DeepHook");

    CaptureSwapchainQueueFromCreateDevice(pDevice, *ppSC, "DeepHook", captureEvidence);
} else if (FAILED(hr)) {
    HookLogImportant("DeepHook: CreateSwapChainForHwnd FAILED hr=0x%08X hwnd=%p", hr, hWnd);
}

return hr;
}
