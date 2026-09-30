#include "dx12_hook_ecl_forward.h"

#include "../common/hook_cpu_cost.h"

namespace ce::dx12_ecl_forward {
namespace {

dx12_overlay_policy::EclBreakTargetClass ClassifyBreakTarget(ExecuteCommandListsPtr candidate) {
    char modulePath[MAX_PATH] = {};
    const bool resolved =
        TryGetModulePathFromCodeAddress(reinterpret_cast<const void*>(candidate), modulePath, sizeof(modulePath));
    return dx12_overlay_policy::ClassifyEclBreakTargetCandidate(resolved, modulePath);
}

}  // namespace

thread_local int recursionDepth = 0;

// Chooses the deepest provably safe ExecuteCommandLists for a recursion break.
// Never returns a known third-party overlay proxy hook (ReShade throws
// resource_deadlock_would_occur when re-entered with the wrapped real queue) or
// CE's own detour.
ExecuteCommandListsPtr ResolveRecursionBreakTarget(ID3D12CommandQueue* queue) {
    void** queueVtable = queue ? *reinterpret_cast<void***>(queue) : nullptr;
    char queueVtablePath[MAX_PATH] = {};
    const bool queueVtableResolved = TryGetModulePathFromCodeAddress(
        reinterpret_cast<const void*>(queueVtable), queueVtablePath, sizeof(queueVtablePath));

    const ExecuteCommandListsPtr perQueueOriginal = GetOriginalExecuteCommandLists(queue);
    const ExecuteCommandListsPtr realD3D12Ecl = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
    switch (dx12_overlay_policy::SelectEclRecursionBreakTarget(
        dx12_overlay_policy::ClassifyEclBreakTargetCandidate(queueVtableResolved, queueVtablePath),
        ClassifyBreakTarget(perQueueOriginal), ClassifyBreakTarget(realD3D12Ecl),
        ClassifyBreakTarget(oExecuteCommandLists))) {
        case dx12_overlay_policy::EclBreakSelection::kPerQueueOriginal:
            return perQueueOriginal;
        case dx12_overlay_policy::EclBreakSelection::kRealD3D12Ecl:
            return realD3D12Ecl;
        case dx12_overlay_policy::EclBreakSelection::kGlobalOriginal:
            return oExecuteCommandLists;
        case dx12_overlay_policy::EclBreakSelection::kNone:
            return nullptr;
    }
    return nullptr;
}

void TransparentNativeFSRCallback(ID3D12CommandQueue* queue, UINT numCommandLists,
                                  ID3D12CommandList* const* commandLists) {
    if (recursionDepth >= 2) {
        static std::atomic<int> s_loopBackLogCount{0};
        const int logCount = s_loopBackLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 5 || (logCount % 1000) == 0) {
            HookLogImportant(
                "DX12: Native-FSR transparent ECL recursion target looped back - dropping submission "
                "(queue=%p lists=%u depth=%d log=%d)",
                queue, numCommandLists, recursionDepth, logCount + 1);
        }
        return;
    }

    ExecuteCommandListsPtr target =
        recursionDepth == 0 ? GetOriginalExecuteCommandLists(queue) : ResolveRecursionBreakTarget(queue);
    if (!target && recursionDepth == 0)
        target = dx12_hook_g_RealD3D12ECL.load(std::memory_order_acquire);
    if (!target && recursionDepth == 0)
        target = oExecuteCommandLists;
    if (!target) {
        static std::atomic<int> s_unresolvedLogCount{0};
        const int logCount = s_unresolvedLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 5 || (logCount % 1000) == 0) {
            HookLogImportant(
                "DX12: Native-FSR transparent ECL forward has no safe target - dropping submission "
                "(queue=%p lists=%u depth=%d log=%d)",
                queue, numCommandLists, recursionDepth, logCount + 1);
        }
        return;
    }

    ++recursionDepth;
    auto depthGuard = make_scope_guard([&]() { --recursionDepth; });
    ScopedHookForwardedCall forwardedCycles;
    target(queue, numCommandLists, commandLists);
}

bool IsPresentedFrameForCapture(int eclSubmissionCount, const present_association::PresentFrameVerdict& verdict) {
    const bool capture = dx12_overlay_policy::IsPresentedFrameForCapture(eclSubmissionCount == 0, verdict.known);
    if (verdict.known) {
        // A recording of a 2x runtime should show about as many generated as
        // application outputs here; a generated count stuck at zero means the
        // runtime stopped reporting generated frames to the callback.
        static std::atomic<uint64_t> s_generatedOutputs{0};
        static std::atomic<uint64_t> s_applicationOutputs{0};
        const uint64_t generated =
            verdict.generated ? s_generatedOutputs.fetch_add(1, std::memory_order_relaxed) + 1
                              : s_generatedOutputs.load(std::memory_order_relaxed);
        const uint64_t application =
            verdict.generated ? s_applicationOutputs.load(std::memory_order_relaxed)
                              : s_applicationOutputs.fetch_add(1, std::memory_order_relaxed) + 1;
        const uint64_t outputs = generated + application;
        if (outputs <= 5 || (outputs % 3000) == 0) {
            HookLogImportant(
                "DX12: Present callback verdict makes this runtime output capturable "
                "(callback=%s eclLists=%d bridgeExpected=%d generatedOutputs=%llu applicationOutputs=%llu "
                "tid=0x%04X)",
                verdict.generated ? "generated" : "application", eclSubmissionCount,
                dx12_hook_g_FFXPresentCallbackBridgeExpected.load(std::memory_order_acquire) ? 1 : 0,
                static_cast<unsigned long long>(generated), static_cast<unsigned long long>(application),
                GetCurrentThreadId());
        }
    }
    return capture;
}

}  // namespace ce::dx12_ecl_forward
