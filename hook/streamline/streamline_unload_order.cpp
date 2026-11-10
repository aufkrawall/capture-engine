#include "streamline_unload_order.h"

#include <dxgi.h>

#include <atomic>
#include <mutex>

#include "common/logging/log_meter.h"
#include "hook/metrics/perf_logger.h"
#include "hook/present/backbuffer_reference_trace.h"
#include "hook/present/native_swapchain_release_observer.h"
#include "hook/present/swapchain_lifetime_witness.h"
#include "hook/runtime/hook_common.h"
#include "streamline_unload_order_policy.h"

namespace ce::streamline_unload_order {
namespace {

namespace native_release = ce::native_swapchain_release_observer;
namespace witness = ce::swapchain_lifetime_witness;

// The policy and the held-chain facts around it. The release observer, the destroy hook and the presents call in
// from other threads; the lock guards only these few words, and nothing is forwarded or logged under it.
std::mutex g_Mutex;
UnloadOrderPolicy g_Policy;
uint64_t g_HeldGeneration = 0;
int64_t g_HeldAtUs = 0;

// Lock-free views for the hot paths: the held chain (null while nothing is held) and the DLSS-G chain.
std::atomic<IDXGISwapChain*> g_HeldChain{nullptr};
std::atomic<IDXGISwapChain*> g_LiveChain{nullptr};
std::atomic<uint64_t> g_LiveGeneration{0};

std::atomic<void (*)()> g_ForwardHeldUnload{nullptr};
std::atomic<bool (*)()> g_DestroyHookInstalled{nullptr};
log_meter::ChangeGate g_NotDeferredLog;

bool ChainAlive(IDXGISwapChain* chain, uint64_t generation) {
    if (!chain) {
        return false;
    }
    const auto report = witness::Describe(chain);
    return report.state == witness::ChainState::kAlive && report.generation == generation;
}

void Disarm() {
    native_release::Watch(native_release::Subscriber::kUnloadOrder, nullptr);
    g_HeldChain.store(nullptr, std::memory_order_release);
}

void ForwardHeldUnload() {
    if (const auto forward = g_ForwardHeldUnload.load(std::memory_order_acquire)) {
        forward();
    }
}

// DXGI's Release of the held chain returned. After the plugin's destroy dispatch, the Streamline module's Release on
// the dispatching thread is the proxy dropping the native chain: Streamline's order is restored, so the unload goes.
void OnNativeRelease(const native_release::ReleaseReport& report) {
    ReleaseOutcome outcome;
    int64_t heldUs = 0;
    uint64_t generation = 0;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        outcome = g_Policy.OnNativeReleaseReturned(reinterpret_cast<ChainId>(report.chain),
                                                   IsStreamlineModuleName(report.callerImage), report.threadId,
                                                   report.remaining);
        if (!outcome.forwardUnload) {
            return;
        }
        heldUs = report.returnQpcUs - g_HeldAtUs;
        generation = g_HeldGeneration;
        Disarm();
    }
    HookLogImportant("[SL UNLOAD ORDER] forwarded reason=%s residualRefs=%u sc=%p lifetime=%llu caller=%s+0x%llX "
                     "tid=0x%04lX heldUs=%lld",
                     ForwardReasonName(ForwardReason::kProxyReleased), outcome.residualRefs, report.chain,
                     static_cast<unsigned long long>(generation), report.callerImage,
                     static_cast<unsigned long long>(report.callerOffset), report.threadId,
                     static_cast<long long>(heldUs));
    if (outcome.residualRefs > 0) {
        // A reference outlived Streamline's own destroy dispatch: a defect on the other branch. Name its holders.
        BackBufferReferenceTrace_Log(report.chain, "unload-order residual references", false);
    }
    ForwardHeldUnload();
}

}  // namespace

void SetCallbacks(const Callbacks& callbacks) {
    g_ForwardHeldUnload.store(callbacks.forwardHeldUnload, std::memory_order_release);
    g_DestroyHookInstalled.store(callbacks.destroyHookInstalled, std::memory_order_release);
}

void NoteDlssgChain(IDXGISwapChain* chain) {
    if (!chain) {
        return;
    }
    // The chain is live here: its create just returned. The witness gives the address a lifetime, so a later chain
    // at the same address is not mistaken for this one.
    DXGI_SWAP_CHAIN_DESC desc{};
    if (SUCCEEDED(chain->GetDesc(&desc)) && desc.OutputWindow) {
        witness::Watch(chain, desc.OutputWindow);
    }
    g_LiveGeneration.store(witness::Describe(chain).generation, std::memory_order_release);
    g_LiveChain.store(chain, std::memory_order_release);
    native_release::SetHandler(native_release::Subscriber::kUnloadOrder, &OnNativeRelease);
    native_release::Install(chain);
}

SetLoadedAction OnSetFeatureLoaded(bool loaded) {
    if (HookIsShuttingDown()) {
        return SetLoadedAction::kForward;
    }
    SetLoadedAction action = SetLoadedAction::kForward;
    IDXGISwapChain* heldChain = nullptr;
    uint64_t heldGeneration = 0;
    bool goneUnreported = false;
    bool deferred = false;
    bool cancelled = false;
    RequestOutcome request;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        // A held chain that died without a release report: its unload goes first, then this request as made.
        if (g_Policy.Pending()) {
            heldChain = g_HeldChain.load(std::memory_order_acquire);
            heldGeneration = g_HeldGeneration;
            if (!ChainAlive(heldChain, heldGeneration) && g_Policy.OnChainGone()) {
                goneUnreported = true;
                Disarm();
            }
        }
        if (loaded) {
            if (g_Policy.OnLoadRequested() == LoadAction::kCancelled) {
                cancelled = true;
                action = SetLoadedAction::kAnswerOk;
                Disarm();
            }
        } else {
            Capabilities capabilities;
            capabilities.observerInstalled = native_release::IsInstalled();
            const auto destroyHookProbe = g_DestroyHookInstalled.load(std::memory_order_acquire);
            capabilities.destroyHookInstalled = destroyHookProbe && destroyHookProbe();
            IDXGISwapChain* const live = g_LiveChain.load(std::memory_order_acquire);
            const uint64_t liveGeneration = g_LiveGeneration.load(std::memory_order_acquire);
            capabilities.liveChain = ChainAlive(live, liveGeneration);
            request = g_Policy.OnUnloadRequested(capabilities,
                                                 capabilities.liveChain ? reinterpret_cast<ChainId>(live) : 0);
            if (request.action == RequestAction::kHold) {
                action = SetLoadedAction::kAnswerOk;
                if (g_HeldChain.load(std::memory_order_acquire) != live) {
                    deferred = true;
                    heldChain = live;
                    heldGeneration = liveGeneration;
                    g_HeldGeneration = liveGeneration;
                    g_HeldAtUs = PerfLogger::GetQpcUs();
                    g_HeldChain.store(live, std::memory_order_release);
                    native_release::Watch(native_release::Subscriber::kUnloadOrder, live);
                }
            } else if (request.reason == ForwardReason::kTitleKeptChain) {
                Disarm();
            }
        }
    }
    if (goneUnreported) {
        HookLogImportant("[SL UNLOAD ORDER] forwarded reason=%s sc=%p lifetime=%llu",
                         ForwardReasonName(ForwardReason::kChainGone), heldChain,
                         static_cast<unsigned long long>(heldGeneration));
        ForwardHeldUnload();
    }
    if (cancelled) {
        HookLogImportant("[SL UNLOAD ORDER] cancelled sc=%p lifetime=%llu (the title loaded DLSS-G again)", heldChain,
                         static_cast<unsigned long long>(heldGeneration));
    } else if (deferred) {
        HookLogImportant("[SL UNLOAD ORDER] deferred sc=%p lifetime=%llu tid=0x%04lX", heldChain,
                         static_cast<unsigned long long>(heldGeneration), GetCurrentThreadId());
    } else if (!loaded && request.action == RequestAction::kForward && request.reason != ForwardReason::kNone) {
        const bool kept = request.reason == ForwardReason::kTitleKeptChain;
        if (const auto verdict = g_NotDeferredLog.Observe(static_cast<uint32_t>(request.reason))) {
            HookLogImportant("[SL UNLOAD ORDER] %s reason=%s%s", kept ? "forwarded" : "not-deferred",
                             ForwardReasonName(request.reason),
                             log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
    }
    return action;
}

bool AnswerNotLoaded() {
    if (!g_HeldChain.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::mutex> lock(g_Mutex);
    return g_Policy.AnswersNotLoaded();
}

void NoteDestroyReturned(IDXGISwapChain* chain) {
    if (!chain) {
        return;
    }
    if (g_HeldChain.load(std::memory_order_acquire) == chain) {
        std::lock_guard<std::mutex> lock(g_Mutex);
        g_Policy.OnDestroyReturned(reinterpret_cast<ChainId>(chain), GetCurrentThreadId());
    }
    // The DLSS-G chain is going: it is no longer a chain a later unload has to wait for.
    IDXGISwapChain* expected = chain;
    g_LiveChain.compare_exchange_strong(expected, nullptr, std::memory_order_acq_rel);
}

void NoteNativePresent(IDXGISwapChain* chain) {
    if (!chain || g_HeldChain.load(std::memory_order_acquire) != chain) {
        return;
    }
    std::lock_guard<std::mutex> lock(g_Mutex);
    g_Policy.OnPresent(reinterpret_cast<ChainId>(chain));
}

void OnInterposerUnloaded() {
    IDXGISwapChain* held = nullptr;
    uint64_t generation = 0;
    bool dropped = false;
    {
        std::lock_guard<std::mutex> lock(g_Mutex);
        held = g_HeldChain.load(std::memory_order_acquire);
        generation = g_HeldGeneration;
        dropped = g_Policy.Drop();
        Disarm();
    }
    g_LiveChain.store(nullptr, std::memory_order_release);
    if (dropped) {
        HookLogImportant("[SL UNLOAD ORDER] dropped sc=%p lifetime=%llu reason=interposer-unloaded", held,
                         static_cast<unsigned long long>(generation));
    }
}

}  // namespace ce::streamline_unload_order
