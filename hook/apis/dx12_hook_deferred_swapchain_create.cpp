#include "dx12_hook_internal.h"

#include "../common/deferred_swapchain_create_ledger.h"

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

void ForgetParkedCreateSwapchainsForWindow(HWND hWnd, const char* context) {
    if (!hWnd || g_ParkedCreateCount.load(std::memory_order_acquire) == 0) {
        return;
    }
    void* released[ce::deferred_swapchain_create::kLedgerCapacity] = {};
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_ParkedCreatesMutex);
        count = g_ParkedCreates.Forget(hWnd, released);
        g_ParkedCreateCount.store(g_ParkedCreates.Count(), std::memory_order_release);
    }
    ReleaseQueues(released, count);
    if (count) {
        HookLogImportant("%s: Visible swapchain create for HWND=%p superseded %zu parked hidden-window create(s)",
                         context && context[0] ? context : "CreateSwapChain", hWnd, count);
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

    ID3D12CommandQueue* presentedQueue = nullptr;
    const bool readable = SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(&presentedQueue))) && presentedQueue;
    const bool matches = readable && presentedQueue == parkedQueue;
    if (presentedQueue) {
        presentedQueue->Release();
    }
    const auto decision = ce::deferred_swapchain_create::DecidePromotion(true, readable, matches);
    const DeferredCreate& deferred = entry.evidence;
    HookLogImportant(
        "DX12: First visible Present of hidden-window swapchain %p (HWND=%p) — %s its parked create "
        "(queue=%p presentedQueue=%s captureQueue=%d create=%s caller=%s)",
        swapchain, entry.window, ce::deferred_swapchain_create::PromotionDecisionName(decision), parkedQueue,
        !readable ? "unreadable" : (matches ? "same" : "different"), deferred.captureQueue ? 1 : 0,
        deferred.context ? deferred.context : "?",
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

void NoteUncapturedSwapchainQueueFallback(IDXGISwapChain* swapchain, ID3D12CommandQueue* chosenQueue) {
    static std::atomic<int> s_logCount{0};
    const int logCount = s_logCount.fetch_add(1, std::memory_order_relaxed);
    if (!swapchain || (logCount >= 5 && (logCount % 600) != 0)) {
        return;
    }
    ID3D12CommandQueue* ownQueue = nullptr;
    const bool readable = SUCCEEDED(swapchain->GetDevice(IID_PPV_ARGS(&ownQueue))) && ownQueue;
    if (ownQueue) {
        ownQueue->Release();
    }
    // Only the swapchain's own queue may write its back buffers; any other queue removes the
    // device. Seeing "different" here names a create path that captured no queue.
    HookLogImportant(
        "DX12: No swapchain queue captured for sc=%p — overlay falls back to cmdQ=%p (swapchain's own queue=%p: "
        "%s parked=%zu log=%d)",
        swapchain, chosenQueue, readable ? ownQueue : nullptr,
        !readable ? "unreadable" : (ownQueue == chosenQueue ? "same" : "DIFFERENT"),
        g_ParkedCreateCount.load(std::memory_order_acquire), logCount + 1);
}

void ReleaseParkedCreateSwapchains(const char* context) {
    void* released[ce::deferred_swapchain_create::kLedgerCapacity] = {};
    size_t count = 0;
    {
        std::lock_guard<std::mutex> lock(g_ParkedCreatesMutex);
        count = g_ParkedCreates.Forget(nullptr, released);
        g_ParkedCreateCount.store(0, std::memory_order_release);
    }
    ReleaseQueues(released, count);
    if (count) {
        HookLog("%s: Released %zu parked hidden-window swapchain create(s)", context ? context : "DX12", count);
    }
}
