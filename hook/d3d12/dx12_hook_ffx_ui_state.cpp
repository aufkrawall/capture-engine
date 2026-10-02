#include "dx12_hook_internal.h"
#include "dx12_hook_ffx_shared.h"


void RecordFFXUiCompositeTimelineEntry(const FFXUiCompositeTimelineEntry& entry) {
    const uint32_t idx = g_FFXUiCompositeTimelineIdx.fetch_add(1, std::memory_order_relaxed);
    g_FFXUiCompositeTimeline[idx % dx12_hook_kFFXUiCompositeTimelineSize] = entry;
}
static void DumpFFXUiCompositeTimeline(const char* reason) {
    const uint32_t idx = g_FFXUiCompositeTimelineIdx.load(std::memory_order_relaxed);
    const int count = (idx < static_cast<uint32_t>(dx12_hook_kFFXUiCompositeTimelineSize)) ? static_cast<int>(idx)
                                                                                 : dx12_hook_kFFXUiCompositeTimelineSize;
    if (count == 0) {
        HookLogImportant("DX12: [ffx-ui-composite-timeline] %s — no composite calls recorded yet", reason ?: "freeze");
        return;
    }
    LARGE_INTEGER freq;
    QueryPerformanceFrequency(&freq);
    const double freqMs = static_cast<double>(freq.QuadPart) / 1000.0;
    const uint32_t startIdx =
        (idx >= static_cast<uint32_t>(dx12_hook_kFFXUiCompositeTimelineSize)) ? (idx % dx12_hook_kFFXUiCompositeTimelineSize) : 0;
    HookLogImportant("DX12: [ffx-ui-composite-timeline] %s — %d entries (total=%u)", reason ?: "freeze", count, idx);
    for (int i = 0; i < count; ++i) {
        const uint32_t slotIdx = (startIdx + i) % dx12_hook_kFFXUiCompositeTimelineSize;
        const auto& e = g_FFXUiCompositeTimeline[slotIdx];
        const double submitToReturnMs =
            (e.returnQpc && e.submitQpc && freqMs > 0) ? static_cast<double>(e.returnQpc - e.submitQpc) / freqMs : -1.0;
        HookLogImportant(
            "  [timeline %d/%d] frame=%llu slot=%u fenceVal=%llu fenceCompleted=%llu waitTimedOut=%u "
            "gameEcl=%u submitQpc=%llu returnQpc=%llu submitToReturnMs=%.3f uiTex=%p ffxState=0x%X queue=%p",
            i + 1, count, static_cast<unsigned long long>(e.frame), e.slot, static_cast<unsigned long long>(e.fenceVal),
            static_cast<unsigned long long>(e.fenceCompleted), e.waitTimedOut, e.gameEclCount,
            static_cast<unsigned long long>(e.submitQpc), static_cast<unsigned long long>(e.returnQpc),
            submitToReturnMs, e.uiTexture, e.ffxState, e.queue);
    }
    const uint64_t lastForwardQpc = g_LastFfxConfigureForwardQpc.load(std::memory_order_relaxed);
    const uint64_t cfgFrame = g_FfxConfigureFrame.load(std::memory_order_relaxed);
    HookLogImportant("  [timeline] lastFfxConfigureForwardQpc=%llu ffxConfigureFrame=%llu",
                     static_cast<unsigned long long>(lastForwardQpc), static_cast<unsigned long long>(cfgFrame));
}
void DX12_LogFFXUiCompositeFreezeDiagnostics(const char* reason) {
    const uint64_t fenceVal = dx12_hook_g_FFXUiCompositeFenceVal;
    const uint64_t fenceCompleted = dx12_hook_g_FFXUiCompositeFence ? dx12_hook_g_FFXUiCompositeFence->GetCompletedValue() : 0;
    const int frame = dx12_hook_g_FFXUiCompositeFrame;
    const bool compositionActive = g_FFXUiResourceCompositionActive.load(std::memory_order_acquire);
    const uint64_t lastTickMs = g_FFXUiCompositeLastTickMs.load(std::memory_order_acquire);
    HookLogImportant(
        "DX12: [ffx-ui-composite-freeze-diag] %s — frame=%d fenceVal=%llu fenceCompleted=%llu "
        "fenceMatch=%d compositionActive=%d lastTickMs=%llu nowMs=%llu",
        reason ? reason : "freeze", frame, static_cast<unsigned long long>(fenceVal),
        static_cast<unsigned long long>(fenceCompleted), fenceVal == fenceCompleted ? 1 : 0, compositionActive ? 1 : 0,
        static_cast<unsigned long long>(lastTickMs), static_cast<unsigned long long>(GetTickCount64()));
    DumpFFXUiCompositeTimeline(reason);
    // Proxy-present driver + re-assert bracket state (defined later in this file / ffx_hook.cpp): shows in
    // one freeze dump whether the game-thread driver was live and whether a substitute re-assert was
    // in-flight (the historical presenter-thread deadlock signature).
    DX12_LogFFXProxyPresentHookFreezeDiagnostics(reason);
    FFXHook_LogSubstituteReRegFreezeDiagnostics(reason);
}
bool DX12_IsFFXUiResourceCompositionActive() {
    if (!g_FFXUiResourceCompositionActive.load(std::memory_order_acquire)) {
        return false;
    }
    // Recency-gated so it auto-disables once FG turns off and the per-frame UI-resource configures stop.
    const uint64_t last = g_FFXUiCompositeLastTickMs.load(std::memory_order_acquire);
    return last != 0 && (GetTickCount64() - last) < 500;
}
bool DX12_ShouldCacheFFXUiResourceForBundle() {
    return dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire);
}
bool DX12_IsFFXUiResourceCachedForBundle() {
    return g_CachedFFXUiTexture.load(std::memory_order_acquire) != nullptr;
}
bool DX12_IsNativeFSRInternalNoCallbackCompositionActive() {
    return dx12_hook_g_NativeFSRInternalNoCallbackComposition.load(std::memory_order_acquire);
}
bool DX12_IsLiveSwapchainQueueOriginalGameQueue() {
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    return dx12_hook_g_SwapchainQueue != nullptr && dx12_hook_g_OriginalGameQueue != nullptr && dx12_hook_g_SwapchainQueue == dx12_hook_g_OriginalGameQueue;
}
bool DX12_IsNativeFSRFGSuspendedDisablePending() {
    return dx12_hook_g_ExplicitNativeFSROffPendingRuntimeOwnedTeardown.load(std::memory_order_acquire);
}
bool IsResourceOwnedByDevice(ID3D12Resource* resource, ID3D12Device* expectedDevice) {
    if (!resource || !expectedDevice) {
        return false;
    }
    ID3D12Device* resourceDevice = nullptr;
    if (FAILED(resource->GetDevice(IID_PPV_ARGS(&resourceDevice))) || !resourceDevice) {
        return false;
    }
    IUnknown* resourceIdentity = nullptr;
    IUnknown* expectedIdentity = nullptr;
    const bool same = SUCCEEDED(resourceDevice->QueryInterface(IID_PPV_ARGS(&resourceIdentity))) &&
                      SUCCEEDED(expectedDevice->QueryInterface(IID_PPV_ARGS(&expectedIdentity))) &&
                      resourceIdentity == expectedIdentity;
    if (resourceIdentity) {
        resourceIdentity->Release();
    }
    if (expectedIdentity) {
        expectedIdentity->Release();
    }
    resourceDevice->Release();
    return same;
}
void DX12_NoteFfxConfigureForward(uint64_t configureType) {
    LARGE_INTEGER qpc;
    QueryPerformanceCounter(&qpc);
    g_LastFfxConfigureForwardQpc.store(static_cast<uint64_t>(qpc.QuadPart), std::memory_order_relaxed);
    const uint64_t frame = g_FfxConfigureFrame.fetch_add(1, std::memory_order_relaxed) + 1;
    // Log RegisterUiResource (type=0x30002) calls with frame context so the FG-configure (0x20002) and
    // UI-register (0x30002) streams are correlatable per frame in the freeze dump.
    if (configureType == ce::ffx_api::kConfigureDescTypeFrameGenerationSwapChainRegisterUiResourceDX12) {
        static std::atomic<int> s_registerUiResLogCount{0};
        const int n = s_registerUiResLogCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 20 || (n % 300) == 0) {
            HookLogImportant("FFX Hook: RegisterUiResource forwarded (type=0x30002 cfgFrame=%llu qpc=%llu log=%d)",
                             static_cast<unsigned long long>(frame), static_cast<unsigned long long>(qpc.QuadPart),
                             n + 1);
        }
    }
}
