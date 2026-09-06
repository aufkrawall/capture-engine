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

}  // namespace ce::dx12_ecl_forward
