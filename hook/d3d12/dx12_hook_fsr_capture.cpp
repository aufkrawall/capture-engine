#include "dx12_hook_internal.h"
#include "common/logging/log_meter.h"
#include "hook/sharpen/gamma_external_submission.h"

#include <array>

void DX12_RefreshEmbeddedCapture() {
    dx12_hook_g_SharedCaptureD3D12.RefreshEmbeddedCopies();
}

namespace {

constexpr size_t kBatchCapacity = 131;

struct CaptureBatch {
    std::array<ID3D12CommandList*, kBatchCapacity> lists{};
    UINT count = 0;
    ExecuteCommandListsPtr original = nullptr;
    ID3D12CommandList* overlay = nullptr;
    bool includeOverlay = false;
    bool submitted = false;
};

thread_local CaptureBatch* t_batch = nullptr;

void SubmitBatch(ID3D12CommandQueue* queue, CaptureBatch& batch, ID3D12CommandList* copy = nullptr) {
    if (copy && !batch.includeOverlay)
        batch.lists[batch.count++] = copy;
    batch.lists[batch.count++] = batch.overlay;
    if (copy && batch.includeOverlay)
        batch.lists[batch.count++] = copy;
    ScopedCEOverlayECLSubmission guard("no-callback-fsr-capture-same-batch");
    batch.original(queue, batch.count, batch.lists.data());
    ce::sharpen::NotifyRuntimePostProcessSubmitted(queue, batch.count, batch.lists.data());
    batch.submitted = true;
}

void STDMETHODCALLTYPE ExecuteOwnerQueueLists(ID3D12CommandQueue* queue, UINT count,
                                              ID3D12CommandList* const* lists) {
    queue->ExecuteCommandLists(count, lists);
}

void STDMETHODCALLTYPE SubmitEmbeddedCopy(ID3D12CommandQueue* queue, UINT count,
                                          ID3D12CommandList* const* lists) {
    // CaptureFrame invokes this synchronously with its one closed copy list.
    if (t_batch && !t_batch->submitted && count == 1)
        SubmitBatch(queue, *t_batch, lists[0]);
}

}  // namespace

bool DX12_SubmitFSRBatchWithCapture(IDXGISwapChain* swapChain, ID3D12CommandQueue* queue,
                                  UINT count, ID3D12CommandList* const* lists,
                                  ID3D12CommandList* overlayList, ExecuteCommandListsPtr original,
                                  ID3D12CommandList* postProcessList) {
    if (!queue || !original || !overlayList || (count != 0 && !lists) || count > kBatchCapacity - 3 || t_batch)
        return false;
    CaptureBatch batch;
    batch.original = original;
    batch.overlay = overlayList;
    batch.count = count;
    for (UINT i = 0; i < count; ++i)
        batch.lists[i] = lists[i];

    if (postProcessList)
        batch.lists[batch.count++] = postProcessList;

    auto* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    if (shm && g_IPC->IsRecording() && shm->runtimeState.IsInjectVideoCaptureRequested() &&
        !ShouldSkipCaptureForTargetCadence()) {
        const auto config = GetActiveDX12OverlayConfig(shm);
        batch.includeOverlay = config.showOverlay && config.captureIncludeOverlay;
        t_batch = &batch;
        auto scope = ce::make_scope_guard([] { t_batch = nullptr; });
        const bool published = PublishDX12CapturedFrame(swapChain, shm, queue, false, 0, nullptr,
                                                        &SubmitEmbeddedCopy, true);
        static ce::log_meter::ChangeGate diagnostic;
        const auto verdict = diagnostic.Observe(published ? 1 : 0);
        if (verdict) {
            HookLogImportant("DX12: FSR final-output capture %s (sc=%p queue=%p overlay=%d repeats=%llu) "
                             "— copy in existing ECL batch, inline completion, no queue Signal",
                             published ? "publishing" : "deferred", swapChain, queue,
                             batch.includeOverlay ? 1 : 0,
                             static_cast<unsigned long long>(verdict.suppressed));
        }
    }
    // A busy capture lease/allocator or a failed initialization must still
    // forward AMD's batch and the overlay exactly once.
    if (!batch.submitted)
        SubmitBatch(queue, batch);
    return true;
}

bool DX12_RenderFSRProxyWithCapture(const ce::dx12_ffx_suspend_overlay::RenderRequest& source) {
    auto* shm = g_IPC ? g_IPC->GetSharedMem() : nullptr;
    if (!shm || !g_IPC->IsRecording() || !shm->runtimeState.IsInjectVideoCaptureRequested() ||
        (!DX12_IsNativeFSRFGSuspendedDisablePending() && !DX12_IsNativeFSRStartupConfigureArmingPending()))
        return ce::dx12_ffx_suspend_overlay::Render(source);
    // The disabled runtime presents the proxy 1:1. Put its capture in the
    // already-required owner-queue overlay submit, at the same ordering seam.
    static thread_local IDXGISwapChain* proxy = nullptr;
    if (proxy)
        return false;
    proxy = source.proxySwapChain;
    auto scope = ce::make_scope_guard([] { proxy = nullptr; });
    auto request = source;
    const auto config = shm->ReadOverlayConfig();
    request.separatePostProcess = !config.showOverlay || !config.captureIncludeOverlay;
    request.submitPreparedCommandLists = [](ID3D12CommandQueue* queue, ID3D12CommandList* postProcess,
                                            ID3D12CommandList* overlay) {
        auto original = GetOriginalExecuteCommandLists(queue);
        if (!original)
            original = &ExecuteOwnerQueueLists;
        return DX12_SubmitFSRBatchWithCapture(proxy, queue, 0, nullptr, overlay, original, postProcess);
    };
    return ce::dx12_ffx_suspend_overlay::Render(request);
}
