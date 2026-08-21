#include "dx12_hook_internal.h"
#include "streamline_bridge.h"

// C Linkage Exports for cross-module calls (e.g. from C clients or
// GetProcAddress)
__attribute__((noinline)) void DX12_SetCommandQueueInternal(ID3D12CommandQueue* pQueue, bool callerFromThirdPartyOverlay, const char* callerModulePath) {
if (!pQueue)
    return;

// Safety: during FG transitions, SL may call ECL on a queue that's
// concurrently being freed.  Freed COM objects have null vtable pointers.
// Use volatile to prevent compiler from caching the vtable across calls.
auto vtblPtr = *reinterpret_cast<void* volatile const*>(pQueue);
if (!vtblPtr)
    return;

if (ShouldQuiesceCESideEffectsForProtectedOfficialFFXStartup()) {
    static std::atomic<int> s_protectedOfficialFFXSetQueueSkipLogCount{0};
    const int logCount = s_protectedOfficialFFXSetQueueSkipLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 256) == 0) {
        HookLogImportant(
            "DX12: Protected official FFX startup pending - skipping SetCommandQueue side effects "
            "(queue=%p callerOverlay=%d caller=%s count=%d)",
            pQueue, callerFromThirdPartyOverlay ? 1 : 0,
            callerModulePath && callerModulePath[0] ? callerModulePath : "unknown", logCount + 1);
    }
    return;
}

// ExecuteCommandLists may hit this many times per frame on the same queue.
// Once we've captured the active DIRECT queue, avoid the repeated GetDesc /
// lock / QueryInterface work on the hot path.
if (g_CommandQueue.load(std::memory_order_acquire) == pQueue)
    return;

ID3D12CommandQueue* primaryQ = dx12_hook_g_PrimaryGameQueue.load(std::memory_order_acquire);
ID3D12CommandQueue* currentSwapchainQueue = nullptr;
{
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    currentSwapchainQueue = dx12_hook_g_SwapchainQueue;
}

if (ce::dx12_overlay_policy::ShouldIgnoreThirdPartyOverlayQueueForGameTracking(
        callerFromThirdPartyOverlay, dx12_hook_g_OriginalGameQueue != nullptr, pQueue == primaryQ,
        pQueue == dx12_hook_g_OriginalGameQueue, pQueue == currentSwapchainQueue)) {
    static std::atomic<int> s_overlayQueueIgnoreLogCount{0};
    int logCount = s_overlayQueueIgnoreLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 256) == 0) {
        HookLogImportant(
            "DX12: SetCommandQueue ignoring foreign overlay queue %p from caller %s "
            "(primary=%p orig=%p scQ=%p current=%p)",
            pQueue, (callerModulePath && *callerModulePath) ? callerModulePath : "unknown", primaryQ,
            dx12_hook_g_OriginalGameQueue, currentSwapchainQueue, g_CommandQueue.load(std::memory_order_acquire));
    }
    return;
}

const bool recentStreamlineTeardown = dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire) > 0;
const bool streamlineFGRunning = DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire);
const bool postSLActive = dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire);
const bool postFSRNonFGRecovery = ce::dx12_overlay_policy::IsPostFSRNonFGRecovery(
    dx12_hook_g_HadFSRFGPhase, dx12_hook_g_NeedOffscreenOverlayAfterPostFSRNonFG, IsActualFrameGenerationActive(), streamlineFGRunning,
    currentSwapchainQueue != nullptr);
const bool lastWorkingQueueStillActiveDuringRecentTeardown =
    dx12_hook_g_PostSLLastWorkingQueue != nullptr &&
    GetTickCount64() < dx12_hook_g_PostSLRecentTeardownActivityUntilMs.load(std::memory_order_acquire);
if (ce::dx12_overlay_policy::ShouldIgnoreCommandQueueRegistrationAfterRecentStreamlineTeardown(
        recentStreamlineTeardown, postFSRNonFGRecovery, lastWorkingQueueStillActiveDuringRecentTeardown,
        pQueue == primaryQ, pQueue == dx12_hook_g_OriginalGameQueue, pQueue == currentSwapchainQueue,
        pQueue == dx12_hook_g_PostSLLastWorkingQueue)) {
    if (ce::dx12_overlay_policy::ShouldRefreshRecentPostSLTeardownActivity(
            recentStreamlineTeardown, dx12_hook_g_PostSLLastWorkingQueue && pQueue == dx12_hook_g_PostSLLastWorkingQueue,
            streamlineFGRunning, postSLActive)) {
        MarkPostSLRecentTeardownActivity("DX12: SetCommandQueue recent PostSL teardown activity", pQueue);
    }
    static std::atomic<int> s_recentSLTeardownSetQueueIgnoreLogCount{0};
    int logCount = s_recentSLTeardownSetQueueIgnoreLogCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 20 || (logCount % 256) == 0) {
        HookLogImportant(
            "DX12: SetCommandQueue ignoring departed queue %p during Streamline teardown / post-FSR recovery "
            "(primary=%p orig=%p scQ=%p current=%p slOffGrace=%d postSLRecent=%d postFSR=%d)",
            pQueue, primaryQ, dx12_hook_g_OriginalGameQueue, currentSwapchainQueue,
            g_CommandQueue.load(std::memory_order_acquire), dx12_hook_g_SLOffHeuristicGrace.load(std::memory_order_acquire),
            lastWorkingQueueStillActiveDuringRecentTeardown ? 1 : 0, postFSRNonFGRecovery ? 1 : 0);
    }
    return;
}

// CRITICAL FIX: Only allow DIRECT queues for overlay rendering.
// Strange Brigade and other DX12 games use Async Compute queues.
// Submitting overlay (Direct) commands to a Compute queue causes a device
// lost/crash.
D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();
if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
    // HookLog("DX12: Ignoring non-direct queue (Type=%d)", desc.Type);
    return;
}

// Set primary game queue once — the first DIRECT queue seen is always the
// game's queue (created before any FG runtime initializes).  Used to filter
// ECL counting for accurate real-vs-interpolated frame classification.
ID3D12CommandQueue* expected = nullptr;
dx12_hook_g_PrimaryGameQueue.compare_exchange_strong(expected, pQueue, std::memory_order_acq_rel);

std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
if (g_CommandQueue.load() != pQueue) {
    if (g_CommandQueue.load())
        g_CommandQueue.load()->Release();
    g_CommandQueue.store(pQueue);
    pQueue->AddRef();

    // Re-check vtable before GetDevice — another thread may have freed
    // the queue between GetDesc and here.  Volatile prevents caching.
    auto vtblRecheck = *reinterpret_cast<void* volatile const*>(pQueue);
    if (!vtblRecheck) {
        HookLogImportant("DX12: SetCommandQueue — queue %p freed during registration (vtable null after store)",
                         pQueue);
        return;
    }

    ID3D12Device* dev = nullptr;
    if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(&dev)))) {
        DX12_PublishNativeLimiterDevice(dev, pQueue, "command queue");
        // A bridged Streamline 2.x runtime needs this device, and this is the route that
        // finds it in a title whose device never came through an sl.interposer export -
        // an Agility SDK game creates it via ID3D12DeviceFactory::CreateDevice, which
        // Streamline does not interpose at all. No-op unless the bridge is active.
        ce::streamline_bridge::NotifyD3D12Device(dev);
        if (g_Device.load() != dev) {
            if (g_Device.load())
                g_Device.load()->Release();
            g_Device.store(dev);

            // Clear device-removed flag — a new device means recovery.
            dx12_hook_g_DeviceRemoved.store(false, std::memory_order_release);
            DXGIShared::g_SharedState.deviceRemovedFatal.store(false, std::memory_order_release);
            g_RenderWatchdog.SetForceMonitor(false);

            // Reset primary game queue — new device means new queues.
            dx12_hook_g_PrimaryGameQueue.store(pQueue, std::memory_order_release);
            dx12_hook_g_LastSuccessfulPostSLSwapchain.store(nullptr, std::memory_order_release);
            dx12_hook_g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
            dx12_hook_g_LastProvenOriginalQueueSwapchain.store(nullptr, std::memory_order_release);

            // Report GPU LUID for host metrics (PDH counter filtering).
            // ID3D12Device has GetAdapterLuid() directly — don't use
            // IDXGIDevice (D3D12 devices don't implement it).
            LUID adapterLuid = dev->GetAdapterLuid();
            ReportLUID(adapterLuid.LowPart, adapterLuid.HighPart);
            HookLog("DX12: Reported LUID %08x-%08x", adapterLuid.HighPart, adapterLuid.LowPart);
        } else
            dev->Release();
    }
}

// CRITICAL FIX: Hook queue vtable lazily here instead of during swapchain
// creation This prevents hangs during DXGI internal operations
DX12_HookQueueVTable(pQueue);
}


// Capture the queue that was passed to CreateSwapChain* so we can prefer it
// for overlay submission.  Only accepts DIRECT queues (same rule as
// DX12_SetCommandQueue).  Also hooks the queue vtable for ECL interception.
bool DX12_SetSwapchainQueue(ID3D12CommandQueue* pQueue, bool authoritativeStreamlineRuntimeQueue, bool authoritativeFFXRuntimeQueue, bool gameCreatedSwapchain, IDXGISwapChain* associatedSwapchain, bool authoritativeNormalSwapchainReturn) {
if (!pQueue)
    return false;

// Safety: freed COM objects have null vtable — skip
void** vtblCheck = *reinterpret_cast<void***>(pQueue);
if (!vtblCheck)
    return false;

D3D12_COMMAND_QUEUE_DESC desc = pQueue->GetDesc();
if (desc.Type != D3D12_COMMAND_LIST_TYPE_DIRECT)
    return false;

bool runtimeOwnershipJustActivated = false;

// Diagnostic: log the queue's device to detect cross-device issues
ID3D12Device* queueDev = nullptr;
if (SUCCEEDED(pQueue->GetDevice(IID_PPV_ARGS(&queueDev)))) {
    auto* curDev = g_Device.load(std::memory_order_acquire);
    if (queueDev != curDev) {
        HookLogImportant("DX12: SetSwapchainQueue — queue %p device %p DIFFERS from g_Device %p", pQueue, queueDev,
                         curDev);
    }
    DX12_PublishNativeLimiterDevice(queueDev, pQueue, "swapchain queue");
    queueDev->Release();
}

std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
dx12_hook_g_LastSwapchainQueueCaptureSwapchain.store(associatedSwapchain, std::memory_order_release);
if (associatedSwapchain && dx12_hook_g_OriginalGameQueue && pQueue != dx12_hook_g_OriginalGameQueue) {
    IDXGISwapChain* expectedOriginalSwapchain = associatedSwapchain;
    if (dx12_hook_g_LastProvenOriginalQueueSwapchain.compare_exchange_strong(
            expectedOriginalSwapchain, nullptr, std::memory_order_acq_rel, std::memory_order_acquire)) {
        HookLogImportant(
            "DX12: Non-original queue association superseded remembered native ownership for swapchain %p "
            "(queue=%p origGame=%p)",
            associatedSwapchain, pQueue, dx12_hook_g_OriginalGameQueue);
    }
}
if (dx12_hook_g_SwapchainQueue != pQueue) {
    if (dx12_hook_g_SwapchainQueue)
        dx12_hook_g_SwapchainQueue->Release();
    dx12_hook_g_SwapchainQueue = pQueue;
    dx12_hook_g_SwapchainQueue->AddRef();
    dx12_hook_g_SwapchainQueueCaptureTime = GetTickCount64();


    // Track whether an FG runtime owns this swapchain/queue
    bool runtimeOwns = (dx12_hook_g_OriginalGameQueue && pQueue != dx12_hook_g_OriginalGameQueue);

    if (authoritativeNormalSwapchainReturn) {
        runtimeOwns = false;
        if (dx12_hook_g_OriginalGameQueue != pQueue) {
            ID3D12CommandQueue* oldOriginalGameQueue = dx12_hook_g_OriginalGameQueue;
            dx12_hook_g_OriginalGameQueue = pQueue;
            dx12_hook_g_LastProvenOriginalQueueSwapchain.store(nullptr, std::memory_order_release);
            pQueue->AddRef();
            HookLogImportant(
                "DX12: Re-baselined original game queue to authoritative normal-return queue %p "
                "(was %p; the game replaced the retired Streamline presentation topology)",
                pQueue, oldOriginalGameQueue);
            if (oldOriginalGameQueue) {
                oldOriginalGameQueue->Release();
            }
        }
        const bool ownershipWasHeld = dx12_hook_g_FGRuntimeOwnsSwapchain;
        dx12_hook_g_FGRuntimeOwnsSwapchain = false;
        DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
        dx12_hook_g_FGRuntimeOwnsSwapchainSince = 0;
        ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
        dx12_hook_s_pendingLateRuntimeOwnedStartupHandoff.store(false, std::memory_order_release);
        HookLogImportant(
            "DX12: Authoritative normal swapchain return ended retired Streamline queue ownership "
            "(queue=%p origGame=%p ownershipWasHeld=%d)",
            pQueue, dx12_hook_g_OriginalGameQueue, ownershipWasHeld ? 1 : 0);
    }

    // A GAME-created swapchain (caller is neither an FG runtime nor a
    // third-party overlay) arriving while explicit native-FSR OFF/destroy
    // evidence is pending is the stronger off signal the runtime-owned
    // teardown was waiting for. Games that recreate their swapchain on a
    // FRESH queue never satisfy the origGame-return teardown end below and
    // would otherwise stay misclassified as runtime-owned, blanking the
    // overlay through FG cooldowns and re-latching FSR heuristics on a
    // plain game queue (20260611_191950 FSR->OFF).
    const bool endNativeFGTeardownOnGameSwapchainCreation =
        ce::dx12_overlay_policy::ShouldEndRuntimeOwnedNativeFGTeardownOnGameSwapchainCreation(
            gameCreatedSwapchain, dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire),
            dx12_hook_g_NativeFSRContextsDestroyedAwaitingGameSwapchain.load(std::memory_order_acquire),
            dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire),
            DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire));
    if (endNativeFGTeardownOnGameSwapchainCreation) {
        runtimeOwns = false;
        dx12_hook_g_NativeFSRContextsDestroyedAwaitingGameSwapchain.store(false, std::memory_order_release);
        dx12_hook_g_PostNativeFSROffGameSwapchainRecoveryQueue.store(pQueue, std::memory_order_release);
        if (associatedSwapchain) {
            dx12_hook_g_ExactGameSwapchainRecoverySwapchain.store(associatedSwapchain, std::memory_order_release);
        }
        // The game retired its previous present queue and created this
        // swapchain on a fresh one with game provenance. Re-baseline the
        // original-game-queue anchor so frame classification counts the
        // game's real ECLs again (a stale dead anchor classifies every
        // present as zero-ECL/interpolated and starves ProcessFrame —
        // 20260612_000936: overlay disappeared forever after FSR->off),
        // and so future FG cycles' takeover/teardown proofs compare
        // against the queue that actually presents. Games that recreate
        // on the SAME queue (Talos-style) hit the pointer-equality no-op.
        if (dx12_hook_g_OriginalGameQueue != pQueue) {
            ID3D12CommandQueue* oldOriginalGameQueue = dx12_hook_g_OriginalGameQueue;
            dx12_hook_g_OriginalGameQueue = pQueue;
            dx12_hook_g_LastProvenOriginalQueueSwapchain.store(nullptr, std::memory_order_release);
            pQueue->AddRef();
            HookLogImportant(
                "DX12: Re-baselined original game queue to game-created recovery queue %p "
                "(was %p; old queue retired by the game itself)",
                pQueue, oldOriginalGameQueue);
            if (oldOriginalGameQueue) {
                oldOriginalGameQueue->Release();
            }
        }
        const bool ownershipWasHeld = dx12_hook_g_FGRuntimeOwnsSwapchain;
        if (dx12_hook_g_FGRuntimeOwnsSwapchain) {
            dx12_hook_g_FGRuntimeOwnsSwapchain = false;
            DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
            dx12_hook_g_FGRuntimeOwnsSwapchainSince = 0;
            ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
        }
        ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
        dx12_hook_s_pendingLateRuntimeOwnedStartupHandoff.store(false, std::memory_order_release);
        ForceClearNativeFSRInternalNoCallbackComposition(
            "game-created swapchain after explicit native FSR OFF/destroy");
        g_FGCompat.SetHeuristicFSRFGActive(false);
        RequestFGDetectionHeuristicReset();
        ResetAuthoritativeFSRRealFrameOnlyStreak();
        SetNativeFSRStartupConfigureArmingPending(false,
                                                  "game-created swapchain after explicit native FSR OFF/destroy");
        ClearOfficialFFXRuntimeOwnedPresentPathAssumption(
            "game-created swapchain after explicit native FSR OFF/destroy");
        if (g_FGCompat.IsFSRFGApiActive()) {
            g_FGCompat.SetFSRFGActive(false);
            g_FGCompat.SetFSRFGMultiplier(0);
        }
        HookLogImportant(
            "DX12: Game-created swapchain after explicit native FSR OFF/destroy — ending runtime-owned "
            "native-FG teardown so the overlay resumes without FG cooldowns (queue=%p origGame=%p "
            "ownershipWasHeld=%d caller=%s)",
            pQueue, dx12_hook_g_OriginalGameQueue, ownershipWasHeld ? 1 : 0, "game");
    }

    if (runtimeOwns && !dx12_hook_g_FGRuntimeOwnsSwapchain) {
        dx12_hook_g_FGRuntimeOwnsSwapchain = true;
        DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(true, std::memory_order_release);
        dx12_hook_g_FGRuntimeOwnsSwapchainSince = GetTickCount64();
        runtimeOwnershipJustActivated = true;
        ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
        dx12_hook_s_pendingLateRuntimeOwnedStartupHandoff.store(true, std::memory_order_release);
        HookLogImportant(
            "DX12: FG runtime now owns swapchain queue %p (origGame=%p) — dedicated/cross-queue overlay work is "
            "disabled on this queue",
            pQueue, dx12_hook_g_OriginalGameQueue);
    } else if (!runtimeOwns && dx12_hook_g_FGRuntimeOwnsSwapchain) {
        const auto runtimeMode = g_FGCompat.GetRuntimeMode();
        const bool authoritativeFSRActive = g_FGCompat.IsFSRFGApiActive();
        const bool runtimeOwnedNativeFGPresentPath = HookHasRuntimeOwnedNativeFGPresentPath();
        const bool explicitNativeFSROffPending =
            dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire);
        const bool ffxPresentCallbackStalled = IsFFXPresentCallbackStalled();
        // Overlay fallback permission is a rendering transport decision, not
        // an ownership teardown signal.  Keep preserving active native FSR
        // ownership until an explicit OFF/device/swapchain transition proves
        // the runtime has really left the FG path.
        const bool preserveAuthoritativeFSRBase =
            ce::dx12_overlay_policy::ShouldSkipSeparateOverlayGpuWorkForRuntimeOwnedFrameGeneration(
                true, DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire), runtimeMode,
                authoritativeFSRActive, runtimeOwnedNativeFGPresentPath, false);
        const bool endNativeFGTeardownOnOrigGame =
            ce::dx12_overlay_policy::ShouldEndRuntimeOwnedNativeFGTeardownOnOriginalQueueReturn(
                pQueue == dx12_hook_g_OriginalGameQueue, explicitNativeFSROffPending, authoritativeFSRActive, runtimeMode,
                runtimeOwnedNativeFGPresentPath);
        const bool preserveAuthoritativeFSR = preserveAuthoritativeFSRBase && !endNativeFGTeardownOnOrigGame;
        if (preserveAuthoritativeFSR) {
            HookLogImportant(
                "DX12: Swapchain queue returned to origGame %p while authoritative/runtime-owned FSR state is "
                "still active (runtime=%s explicitNativeOff=%d nativeFGPath=%d stalled=%d) — preserving FG "
                "runtime ownership until a stronger off signal arrives",
                pQueue, ce::fg_runtime::GetRuntimeModeName(runtimeMode), explicitNativeFSROffPending ? 1 : 0,
                runtimeOwnedNativeFGPresentPath ? 1 : 0, ffxPresentCallbackStalled ? 1 : 0);
            HookLogImportant("DX12: Swapchain queue captured (queue=%p, origGame=%p, same=%d, fgOwned=%d)", pQueue,
                             dx12_hook_g_OriginalGameQueue, (pQueue == dx12_hook_g_OriginalGameQueue) ? 1 : 0,
                             dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0);
            return runtimeOwnershipJustActivated;
        }

        if (endNativeFGTeardownOnOrigGame) {
            HookLogImportant(
                "DX12: Explicit native FSR OFF plus origGame swapchain return ending runtime-owned native-FG "
                "teardown (queue=%p origGame=%p runtime=%s nativeFGPath=%d callbackStalled=%d)",
                pQueue, dx12_hook_g_OriginalGameQueue, ce::fg_runtime::GetRuntimeModeName(runtimeMode),
                runtimeOwnedNativeFGPresentPath ? 1 : 0, ffxPresentCallbackStalled ? 1 : 0);
            g_FGCompat.SetHeuristicFSRFGActive(false);
            ResetAuthoritativeFSRRealFrameOnlyStreak();
            ForceClearNativeFSRInternalNoCallbackComposition(
                "explicit native FSR OFF plus origGame swapchain return");
        }

        dx12_hook_g_FGRuntimeOwnsSwapchain = false;
        DXGIShared::g_SharedState.fgRuntimeOwnsSwapchain.store(false, std::memory_order_release);
        dx12_hook_g_FGRuntimeOwnsSwapchainSince = 0;
        ResetStaleRuntimeOwnedStreamlineNoFGRealFrameOnlyStreak();
        ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
        dx12_hook_s_pendingLateRuntimeOwnedStartupHandoff.store(false, std::memory_order_release);
        if (g_FGCompat.IsFSRFGApiActive()) {
            HookLogImportant("DX12: Swapchain returned to origGame queue %p — ending authoritative FSR FG state",
                             pQueue);
            SetNativeFSRStartupConfigureArmingPending(false, "swapchain returned to origGame");
            ClearOfficialFFXRuntimeOwnedPresentPathAssumption("swapchain returned to origGame");
            g_FGCompat.SetFSRFGActive(false);
            g_FGCompat.SetFSRFGMultiplier(0);
            ResetAuthoritativeFSRRealFrameOnlyStreak();
        }
        HookLogImportant("DX12: Swapchain returned to origGame queue %p — FG runtime ownership cleared", pQueue);
    }

    // If the swapchain queue changed and FSR is no longer active, the old explicit
    // native-FSR OFF teardown flag is stale — it referred to the previous queue's
    // runtime-owned Present path, not this new one.  Keeping it true defers overlay
    // init indefinitely when the game creates a new menu swapchain after FSR OFF.
    if (dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire) &&
        !g_FGCompat.IsFSRFGApiActive() && g_FGCompat.GetRuntimeMode() != ce::fg_runtime::RuntimeMode::kFSRFG) {
        ClearExplicitNativeFSROffPendingRuntimeOwnedTeardown();
        HookLogImportant(
            "DX12: Swapchain queue changed to %p while FSR is no longer active — cleared stale explicit native FSR "
            "OFF teardown flag (origGame=%p runtime=%s)",
            pQueue, dx12_hook_g_OriginalGameQueue, ce::fg_runtime::GetRuntimeModeName(g_FGCompat.GetRuntimeMode()));
    }

    HookLogImportant("DX12: Swapchain queue captured (queue=%p, origGame=%p, same=%d, fgOwned=%d)", pQueue,
                     dx12_hook_g_OriginalGameQueue, (pQueue == dx12_hook_g_OriginalGameQueue) ? 1 : 0,
                     dx12_hook_g_FGRuntimeOwnsSwapchain ? 1 : 0);
}

// Only hook the vtable if this is the game's original queue (or we haven't
// captured origGame yet).  FG runtimes (FSR FG) create their own queues and
// rely on tight ECL timing.  Hooking their vtable adds overhead to every ECL
// call (safety checks, heartbeat, queue tracking, lock acquisition, etc.).
// This cumulative overhead breaks FSR FG's internal fence synchronization,
// causing ffxQuery to spin-wait or WaitForSingleObject indefinitely.
// We already hook origGame's queue for watchdog/heartbeat — that's sufficient.
const bool shouldHookQueueVTable = ce::dx12_overlay_policy::ShouldHookSwapchainQueueVTableForFrameGenerationRuntime(
    dx12_hook_g_OriginalGameQueue != nullptr, pQueue == dx12_hook_g_OriginalGameQueue, authoritativeStreamlineRuntimeQueue,
    authoritativeFFXRuntimeQueue);
if (shouldHookQueueVTable) {
    if (authoritativeStreamlineRuntimeQueue && dx12_hook_g_OriginalGameQueue && pQueue != dx12_hook_g_OriginalGameQueue) {
        HookLogImportant(
            "DX12: Hooking authoritative Streamline runtime queue vtable %p (origGame=%p) to keep runtime-owned "
            "ECL tracking visible",
            pQueue, dx12_hook_g_OriginalGameQueue);
    }
    DX12_HookQueueVTable(pQueue);
} else {
    if (authoritativeFFXRuntimeQueue && authoritativeStreamlineRuntimeQueue && dx12_hook_g_OriginalGameQueue &&
        pQueue != dx12_hook_g_OriginalGameQueue) {
        HookLogImportant(
            "DX12: Skipping vtable hook for FFX-owned runtime queue %p despite stale Streamline provenance "
            "(origGame=%p) — preserving FSR timing",
            pQueue, dx12_hook_g_OriginalGameQueue);
    } else {
        HookLogImportant("DX12: Skipping vtable hook for FG runtime queue %p (origGame=%p) — preserving FSR timing",
                         pQueue, dx12_hook_g_OriginalGameQueue);
    }
}

return runtimeOwnershipJustActivated;
}


bool IsDX12Swapchain(IDXGISwapChain* pSwapChain) {
if (!pSwapChain)
    return false;

ID3D12Device* pDX12Device = nullptr;
HRESULT hr = pSwapChain->GetDevice(IID_PPV_ARGS(&pDX12Device));
if (FAILED(hr) || !pDX12Device)
    return false;

pDX12Device->Release();
return true;
}


bool InvalidatePostSLProofForFreshAuthoritativeStreamlineHandoff(const char* context, ID3D12CommandQueue* newSwapchainQueue, ID3D12CommandQueue* previousSwapchainQueue, ID3D12CommandQueue* originalGameQueue) {
ID3D12CommandQueue* lockedQueue = nullptr;
ID3D12CommandQueue* lastWorkingQueue = nullptr;
{
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    lockedQueue = dx12_hook_g_PostSLLockedQueue;
    lastWorkingQueue = dx12_hook_g_PostSLLastWorkingQueue;
}

const bool postSLConfirmedRendering = dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire);
const bool newQueueMatchesPreviousSwapchainQueue =
    newSwapchainQueue != nullptr && newSwapchainQueue == previousSwapchainQueue;
const bool invalidateConfirmed =
    ce::dx12_overlay_policy::ShouldInvalidateConfirmedPostSLForFreshAuthoritativeStreamlineHandoff(
        true, postSLConfirmedRendering, newQueueMatchesPreviousSwapchainQueue);
const bool clearLastWorking =
    ce::dx12_overlay_policy::ShouldClearPostSLQueueProofForFreshAuthoritativeStreamlineHandoff(
        true, lastWorkingQueue != nullptr, newSwapchainQueue != nullptr && lastWorkingQueue == newSwapchainQueue);
const bool clearLocked = ce::dx12_overlay_policy::ShouldClearPostSLQueueProofForFreshAuthoritativeStreamlineHandoff(
    true, lockedQueue != nullptr, newSwapchainQueue != nullptr && lockedQueue == newSwapchainQueue);

DXGIShared::g_SharedState.streamlineStartupTopLevelPresentConsumed.store(false, std::memory_order_release);
DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(true, std::memory_order_release);
dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
dx12_hook_g_PostSLSyntheticStartupWrapperProgressCount.store(0, std::memory_order_release);
dx12_hook_g_PostSLSyntheticStartupWrapperOnlyDumpRequested.store(false, std::memory_order_release);
dx12_hook_g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);

if (invalidateConfirmed) {
    const int previousStableFrames = dx12_hook_g_PostSLStableFrameCount.exchange(0, std::memory_order_acq_rel);
    dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
    dx12_hook_g_PostSLRuntimeStateStabilizationLogged.store(false, std::memory_order_release);
    dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
    dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
    HookLogImportant(
        "DX12: Fresh authoritative Streamline handoff invalidated stale PostSL confirmation "
        "(source=%s newScQueue=%p prevScQueue=%p origGame=%p locked=%p lastWorking=%p stableFrames=%d)",
        context ? context : "unknown", newSwapchainQueue, previousSwapchainQueue, originalGameQueue, lockedQueue,
        lastWorkingQueue, previousStableFrames);
}

if (invalidateConfirmed || clearLocked) {
    WaitForOverlayGpuIdle("DX12: Fresh authoritative Streamline handoff");
    ResetPostSLLifecycleForTransition("DX12: Fresh authoritative Streamline handoff", true);
}

if (clearLastWorking) {
    HookLogImportant(
        "DX12: Fresh authoritative Streamline handoff cleared stale PostSL lastWorking queue %p "
        "(newScQueue=%p prevScQueue=%p origGame=%p)",
        lastWorkingQueue, newSwapchainQueue, previousSwapchainQueue, originalGameQueue);
    SetPostSLLastWorkingQueue(nullptr);
}

bool overlayWasLive = false;
bool overlaySwapchainStateRetired = false;
if (dx12_hook_g_State.overlayInit || dx12_hook_g_State.syncInit) {
    std::lock_guard<std::recursive_mutex> overlayLock(dx12_hook_g_OverlayMutex);
    overlayWasLive = dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit && g_OverlayAdapter.IsInitialized();
    const bool preserveLiveOverlayDuringHandoff =
        ce::dx12_overlay_policy::ShouldPreserveLiveOverlayDuringRuntimeInactiveStreamlineHandoff(
            dx12_hook_s_startupOverlayCompatSettled.load(std::memory_order_acquire), dx12_hook_g_State.overlayInit && dx12_hook_g_State.syncInit,
            dx12_hook_g_FGRuntimeOwnsSwapchain, DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire),
            g_FGCompat.GetRuntimeMode(), HookHasExplicitStreamlineSetOptionsActivation(),
            dx12_hook_s_startupOverlayObservedAnyFG.load(std::memory_order_acquire), dx12_hook_g_HadFSRFGPhase,
            dx12_hook_g_OriginalGameQueue != nullptr);
    if (preserveLiveOverlayDuringHandoff) {
        dx12_hook_g_State.cachedSwapChain = nullptr;
        dx12_hook_g_State.cachedSC3 = nullptr;
        HookLogImportant(
            "DX12: Fresh authoritative Streamline no-FG handoff preserved live overlay backend "
            "(source=%s newScQueue=%p prevScQueue=%p origGame=%p)",
            context ? context : "unknown", newSwapchainQueue, previousSwapchainQueue, originalGameQueue);
    } else {
        // The adapter is device/format scoped and does not retain swapchain buffers or submit through its
        // initialization queue. Keep it warm while retiring only the old swapchain-scoped RTV/sync state.
        // The fresh post-FSR Streamline handoff can then prewarm the replacement state before DLSS is enabled,
        // rather than rebuilding the backend inside the first generated Present.
        dx12_hook_g_PreserveOverlayAdapterAcrossResize.store(g_OverlayAdapter.IsInitialized(), std::memory_order_release);
        dx12_hook_g_State.overlayInit = false;
        dx12_hook_g_State.syncInit = false;
        dx12_hook_g_ResetReinitSubmitCounter.store(true, std::memory_order_release);
        CleanupRTVs();
        overlaySwapchainStateRetired = true;
        HookLogImportant(
            "DX12: Fresh authoritative Streamline handoff invalidated PostSL swapchain resources while "
            "preserving the warm device-scoped backend (source=%s newScQueue=%p prevScQueue=%p live=%d)",
            context ? context : "unknown", newSwapchainQueue, previousSwapchainQueue, overlayWasLive ? 1 : 0);
    }
}
return overlayWasLive && overlaySwapchainStateRetired;
}


void PublishPostSLRouteRetirementForNormalSwapchainReturn(const char* reason) {
SetPostSLCallbackInstalled(false, reason);
// Publish cancellation before waiting for an already-entered callback. The
// callback compares this epoch before every GPU submission point, exits,
// and releases the render mutex without a polling delay.
dx12_hook_g_PostSLLifecycleEpoch.fetch_add(1, std::memory_order_acq_rel);
}


int FinishPostSLRouteRetirementForNormalSwapchainReturn(const char* reason) {
std::lock_guard<std::mutex> renderLock(dx12_hook_g_PostSLRenderMutex);

const int previousStableFrames = dx12_hook_g_PostSLStableFrameCount.exchange(0, std::memory_order_acq_rel);
dx12_hook_g_PostSLOverlayActive.store(false, std::memory_order_release);
dx12_hook_g_PostSLConfirmedRendering.store(false, std::memory_order_release);
dx12_hook_g_PostSLConfirmedRenderInCurrentReactivationEpoch.store(false, std::memory_order_release);
dx12_hook_g_PostSLStallCounter.store(0, std::memory_order_release);
dx12_hook_g_PostSLRuntimeStateStabilizationLogged.store(false, std::memory_order_release);
dx12_hook_g_PostSLExtendedRuntimeStateStabilizationForCurrentEpoch.store(false, std::memory_order_release);
DXGIShared::g_SharedState.postSLSyntheticStartupActivationPending.store(false, std::memory_order_release);
dx12_hook_g_PostSLSyntheticStartupActivatedButUnconfirmed.store(false, std::memory_order_release);
dx12_hook_g_PostSLSyntheticStartupTakeoverLogged.store(false, std::memory_order_release);
ResetPostSLLifecycleForTransition(reason, true);
SetPostSLLastWorkingQueue(nullptr);
ReleaseStreamlineStartupActivationSwapchain(reason);

if (dx12_hook_g_State.overlayInit || dx12_hook_g_State.syncInit) {
    std::lock_guard<std::recursive_mutex> overlayLock(dx12_hook_g_OverlayMutex);
    dx12_hook_g_PreserveOverlayAdapterAcrossResize.store(g_OverlayAdapter.IsInitialized(), std::memory_order_release);
    dx12_hook_g_State.overlayInit = false;
    dx12_hook_g_State.syncInit = false;
    dx12_hook_g_ResetReinitSubmitCounter.store(true, std::memory_order_release);
    CleanupRTVs();
}

return previousStableFrames;
}


int RetirePostSLRouteForNormalSwapchainReturn(const char* reason) {
PublishPostSLRouteRetirementForNormalSwapchainReturn(reason);
return FinishPostSLRouteRetirementForNormalSwapchainReturn(reason);
}


bool HandlePostSLRouteForNormalSwapchainReturn(const char* context, ID3D12CommandQueue* returnedQueue, IDXGISwapChain* returnedSwapchain, ID3D12CommandQueue* originalGameQueue, const CreateSwapchainQueueCaptureEvidence& captureEvidence) {
const bool originalQueueNormalSwapchainReturn =
    ce::dx12_overlay_policy::ShouldTreatOriginalQueueCreateWithStreamlineStackAsNormalReturn(
        captureEvidence.authoritativeFFXRuntimeCreator, captureEvidence.callerFromStreamlineFGModule,
        captureEvidence.streamlineFrameGenerationInStack,
        dx12_hook_g_StreamlineEnableCallsInFlight.load(std::memory_order_acquire) != 0, originalGameQueue != nullptr,
        returnedQueue == originalGameQueue);
const bool gameCreatedSwapchain =
    !captureEvidence.callerFromThirdPartyOverlay && !captureEvidence.authoritativeFFXRuntimeCreator &&
    !captureEvidence.officialAMDFFXRuntimeCreator && !captureEvidence.authoritativeStreamlineRuntimeCreator;
const bool gameSwapchainAfterExplicitDLSSOff =
    ce::dx12_overlay_policy::ShouldTreatGameSwapchainCreateAfterExplicitDLSSOffAsNormalReturn(
        gameCreatedSwapchain, dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire),
        IsActualFrameGenerationActive(), DXGIShared::g_StreamlineFGRunning.load(std::memory_order_acquire));
const bool normalSwapchainReturn = originalQueueNormalSwapchainReturn || gameSwapchainAfterExplicitDLSSOff;
if (!normalSwapchainReturn) {
    return false;
}

if (gameSwapchainAfterExplicitDLSSOff && returnedSwapchain) {
    dx12_hook_g_PostDLSSOffAuthoritativeNormalReturnSwapchain.store(returnedSwapchain, std::memory_order_release);
    HookLogImportant(
        "[OVERLAY VISIBILITY] Armed exact native swapchain takeover after authoritative DLSS OFF "
        "(swapchain=%p queue=%p)",
        returnedSwapchain, returnedQueue);
} else {
    dx12_hook_g_PostDLSSOffAuthoritativeNormalReturnSwapchain.store(nullptr, std::memory_order_release);
}

// A proven return is also an authoritative queue-topology boundary even
// when no PostSL route happens to remain armed. Seed the queue heuristic
// before the first Present so the departed Streamline queue can never be
// mistaken for the baseline of a new FSR epoch.
g_FGCompat.SetHeuristicFSRFGActive(false);
RequestFGDetectionHeuristicReset(returnedQueue);

ID3D12CommandQueue* lockedQueue = nullptr;
ID3D12CommandQueue* lastWorkingQueue = nullptr;
{
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    lockedQueue = dx12_hook_g_PostSLLockedQueue;
    lastWorkingQueue = dx12_hook_g_PostSLLastWorkingQueue;
}
const bool routeArmed = DXGIShared::g_PostSLOverlayRenderCallback.load(std::memory_order_acquire) != nullptr ||
                        dx12_hook_g_PostSLOverlayActive.load(std::memory_order_acquire) ||
                        dx12_hook_g_PostSLConfirmedRendering.load(std::memory_order_acquire) ||
                        dx12_hook_g_PostSLExplicitOffKeepAlive.load(std::memory_order_acquire) || lockedQueue != nullptr ||
                        lastWorkingQueue != nullptr;
const bool hasDistinctQueueProof =
    (lockedQueue && lockedQueue != returnedQueue) || (lastWorkingQueue && lastWorkingQueue != returnedQueue);
const char* normalReturnProof = gameSwapchainAfterExplicitDLSSOff
                                    ? "Game-created replacement swapchain validated normal return after "
                                      "explicit DLSS off"
                                    : "Original game queue validated normal swapchain return behind "
                                      "Streamline stack";
if (!ce::dx12_overlay_policy::ShouldRetirePostSLRouteForNormalSwapchainReturn(
        normalSwapchainReturn, routeArmed, hasDistinctQueueProof || gameSwapchainAfterExplicitDLSSOff)) {
    HookLogImportant("%s: %s (queue=%p locked=%p lastWorking=%p routeArmed=%d) — no stale PostSL route to retire",
                     context ? context : "CreateSwapChain", normalReturnProof, returnedQueue, lockedQueue,
                     lastWorkingQueue, routeArmed ? 1 : 0);
    return true;
}

const int previousStableFrames = RetirePostSLRouteForNormalSwapchainReturn("DX12: normal swapchain return");

HookLogImportant(
    "%s: %s — retired stale PostSL route and invalidated swapchain-scoped overlay state "
    "(queue=%p locked=%p lastWorking=%p stableFrames=%d caller=%s)",
    context ? context : "CreateSwapChain", normalReturnProof, returnedQueue, lockedQueue, lastWorkingQueue,
    previousStableFrames, captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "unknown");
return true;
}

