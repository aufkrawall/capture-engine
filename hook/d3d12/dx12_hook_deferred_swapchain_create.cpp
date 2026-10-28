#include "dx12_hook_internal.h"
#include "hook/fg/fg_presenter.h"

#include "hook/present/deferred_swapchain_create_ledger.h"

// Create-time queue ownership for DX12 swapchains created on a hidden window,
// replayed on their first visible Present. The policy and the evidence behind it
// are in deferred_swapchain_create_ledger.h.

namespace {

struct DeferredCreate {
    CreateSwapchainQueueCaptureEvidence evidence;
    // The visible create path of the same call site would have captured the
    // queue (the global CreateSwapChain paths only do so under Streamline).
    bool captureQueue = false;
    const char* context = nullptr;
};

using Ledger = ce::deferred_swapchain_create::Ledger<DeferredCreate>;

std::mutex g_ParkedCreatesMutex;
Ledger g_ParkedCreates;
// Mirrors g_ParkedCreates.Count() so the per-Present check is lock-free.
std::atomic<size_t> g_ParkedCreateCount{0};

// A parked record only exists to be replayed by its swapchain's first visible Present. Once its window is
// destroyed that Present cannot come, and the record would keep the swapchain's command queue alive.
bool ParkedWindowGone(const void* window) {
    return window && !IsWindow(static_cast<HWND>(const_cast<void*>(window)));
}

// Presents between two sweeps for dead windows while nothing matches the presenting swapchain.
constexpr uint32_t kDeadWindowSweepIntervalPresents = 256;

void ReleaseQueues(void** queues, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (queues[i]) {
            static_cast<ID3D12CommandQueue*>(queues[i])->Release();
        }
    }
}

}  // namespace

void ParkInvisibleWindowCreateSwapchain(IDXGISwapChain* swapchain, HWND hWnd, IUnknown* createDevice,
                                        const CreateSwapchainQueueCaptureEvidence& captureEvidence,
                                        bool createCapturesQueue, const char* context) {
    ID3D12CommandQueue* queue = nullptr;
    if (!swapchain || !createDevice || FAILED(createDevice->QueryInterface(IID_PPV_ARGS(&queue))) || !queue) {
        return;  // not a DX12 create: nothing CE would own
    }
    DeferredCreate deferred;
    deferred.evidence = captureEvidence;
    deferred.captureQueue = createCapturesQueue;
    deferred.context = context;
    void* displaced = nullptr;
    void* deadWindowQueues[ce::deferred_swapchain_create::kLedgerCapacity] = {};
    size_t deadWindowCount = 0;
    size_t parked = 0;
    {
        std::lock_guard<std::mutex> lock(g_ParkedCreatesMutex);
        deadWindowCount = g_ParkedCreates.ForgetWhereWindowGone(&ParkedWindowGone, deadWindowQueues);
        displaced = g_ParkedCreates.Park(swapchain, queue, hWnd, deferred);
        parked = g_ParkedCreates.Count();
        g_ParkedCreateCount.store(parked, std::memory_order_release);
    }
    ReleaseQueues(&displaced, 1);
    ReleaseQueues(deadWindowQueues, deadWindowCount);
    HookLogImportant(
        "%s: Parked create-time queue ownership for hidden-window swapchain %p (HWND=%p queue=%p captureQueue=%d "
        "caller=%s parked=%zu) — replayed on its first visible Present",
        context && context[0] ? context : "CreateSwapChain", swapchain, hWnd, queue, createCapturesQueue ? 1 : 0,
        captureEvidence.callerModulePath[0] ? captureEvidence.callerModulePath : "stack", parked);
}

void SupersedeParkedCreateSwapchains(IDXGISwapChain* created, HWND hWnd, const char* context) {
    if (!created || g_ParkedCreateCount.load(std::memory_order_acquire) == 0) {
        return;
    }
    void* released[ce::deferred_swapchain_create::kLedgerCapacity] = {};
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_ParkedCreatesMutex);
        count = g_ParkedCreates.Supersede(created, hWnd, released);
        g_ParkedCreateCount.store(g_ParkedCreates.Count(), std::memory_order_release);
    }
    ReleaseQueues(released, count);
    if (count) {
        HookLogImportant(
            "%s: Swapchain create %p for HWND=%p superseded %zu parked hidden-window create(s) for that window or "
            "address",
            context && context[0] ? context : "CreateSwapChain", created, hWnd, count);
    }
}

void PromoteParkedCreateSwapchainOnVisiblePresent(IDXGISwapChain* swapchain) {
    if (!swapchain || g_ParkedCreateCount.load(std::memory_order_acquire) == 0) {
        return;
    }
    Ledger::Entry entry;
    {
        std::unique_lock<std::mutex> lock(g_ParkedCreatesMutex);
        if (!g_ParkedCreates.Take(swapchain, &entry)) {
            static uint32_t s_presentsSinceSweep = 0;  // guarded by g_ParkedCreatesMutex
            if (++s_presentsSinceSweep < kDeadWindowSweepIntervalPresents) {
                return;
            }
            s_presentsSinceSweep = 0;
            void* deadWindowQueues[ce::deferred_swapchain_create::kLedgerCapacity] = {};
            const size_t deadWindowCount = g_ParkedCreates.ForgetWhereWindowGone(&ParkedWindowGone, deadWindowQueues);
            g_ParkedCreateCount.store(g_ParkedCreates.Count(), std::memory_order_release);
            lock.unlock();
            ReleaseQueues(deadWindowQueues, deadWindowCount);
            if (deadWindowCount) {
                HookLogImportant("DX12: Released %zu parked hidden-window create(s) whose window was destroyed",
                                 deadWindowCount);
            }
            return;
        }
        g_ParkedCreateCount.store(g_ParkedCreates.Count(), std::memory_order_release);
    }
    auto* parkedQueue = static_cast<ID3D12CommandQueue*>(entry.queue);
    auto releaseParked = ce::make_scope_guard([&]() { parkedQueue->Release(); });

    ce::deferred_swapchain_create::PromotionEvidence evidence;
    evidence.parked = true;
    ID3D12CommandQueue* presentedQueue = nullptr;
    evidence.presentedQueueReadable = SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(&presentedQueue))) && presentedQueue;
    evidence.presentedQueueMatchesParked = evidence.presentedQueueReadable && presentedQueue == parkedQueue;
    if (presentedQueue) {
        presentedQueue->Release();
    }
    // An HWND swapchain never changes its window, so another window is another swapchain.
    DXGI_SWAP_CHAIN_DESC presentedDesc = {};
    const auto parkedWindow = static_cast<HWND>(const_cast<void*>(entry.window));
    evidence.presentedWindowReadable = SUCCEEDED(swapchain->GetDesc(&presentedDesc)) && presentedDesc.OutputWindow;
    evidence.presentedWindowMatchesParked =
        evidence.presentedWindowReadable && presentedDesc.OutputWindow == parkedWindow;
    const auto decision = ce::deferred_swapchain_create::DecidePromotion(evidence);
    const DeferredCreate& deferred = entry.evidence;
    HookLogImportant(
        "DX12: First visible Present of hidden-window swapchain %p (HWND=%p presentedHWND=%p) — %s its parked "
        "create (queue=%p presentedQueue=%s presentedWindow=%s captureQueue=%d create=%s caller=%s)",
        swapchain, entry.window, presentedDesc.OutputWindow,
        ce::deferred_swapchain_create::PromotionDecisionName(decision), parkedQueue,
        !evidence.presentedQueueReadable ? "unreadable" : (evidence.presentedQueueMatchesParked ? "same" : "different"),
        !evidence.presentedWindowReadable ? "unreadable"
                                          : (evidence.presentedWindowMatchesParked ? "same" : "different"),
        deferred.captureQueue ? 1 : 0, deferred.context ? deferred.context : "?",
        deferred.evidence.callerModulePath[0] ? deferred.evidence.callerModulePath : "stack");
    if (decision != ce::deferred_swapchain_create::PromotionDecision::kPromote) {
        return;
    }

    // The same order the visible create path runs in.
    if (HandleProtectedOfficialFFXStartupSwapchainCreate(deferred.evidence, parkedQueue, swapchain,
                                                         "deferred hidden-window create")) {
        return;
    }
    if (deferred.captureQueue) {
        CaptureSwapchainQueueFromCreateDevice(parkedQueue, swapchain, "Deferred hidden-window create",
                                              deferred.evidence);
    }
}

bool NoteUncapturedSwapchainQueueFallback(IDXGISwapChain* swapchain, ID3D12CommandQueue* chosenQueue) {
    if (!swapchain) {
        return false;
    }
    ID3D12CommandQueue* ownQueue = nullptr;
    const bool readable = SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(&ownQueue))) && ownQueue;
    if (ownQueue) {
        ownQueue->Release();
    }
    const auto presenter = ce::fg_presenter::PresenterFor(swapchain);
    const auto decision = ce::dx12_overlay_policy::DecideUncapturedQueueFallback(
        readable, readable && ownQueue == chosenQueue, presenter != ce::fg_presenter::Presenter::kApplication);
    static std::atomic<int> s_logCount{0};
    static ce::log_meter::ChangeGate s_decisionGate;
    const int logCount = s_logCount.fetch_add(1, std::memory_order_relaxed);
    const auto verdict = s_decisionGate.Observe(ce::log_meter::FieldKey(swapchain, decision));
    if (verdict || logCount < 5 || (logCount % 600) == 0) {
        // Only the swapchain's own queue may write its back buffers; any other queue removes the
        // device. Seeing "different" here names a create path that captured no queue.
        HookLogImportant(
            "DX12: No swapchain queue captured for sc=%p — %s cmdQ=%p (swapchain's own queue=%p: %s presenter=%s "
            "parked=%zu log=%d)",
            swapchain,
            decision == ce::dx12_overlay_policy::UncapturedQueueFallback::kDraw ? "overlay falls back to"
                                                                               : "SKIPPING the overlay instead of",
            chosenQueue, readable ? ownQueue : nullptr,
            !readable ? "unreadable" : (ownQueue == chosenQueue ? "same" : "DIFFERENT"),
            ce::fg_presenter::PresenterName(presenter), g_ParkedCreateCount.load(std::memory_order_acquire),
            logCount + 1);
    }
    return decision == ce::dx12_overlay_policy::UncapturedQueueFallback::kDraw;
}

void ReleaseParkedCreateSwapchains(const char* context) {
    void* released[ce::deferred_swapchain_create::kLedgerCapacity] = {};
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_ParkedCreatesMutex);
        count = g_ParkedCreates.ForgetAll(released);
        g_ParkedCreateCount.store(0, std::memory_order_release);
    }
    ReleaseQueues(released, count);
    if (count) {
        HookLog("%s: Released %zu parked hidden-window swapchain create(s)", context ? context : "DX12", count);
    }
}
