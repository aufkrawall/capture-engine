#include "dx12_ffx_suspend_overlay_internal.h"

namespace ce::dx12_ffx_suspend_overlay {
namespace {
using detail::RendererState;
using detail::ResolveRtvFormat;
using detail::SameComObject;
using detail::ComPtr;

// NOLINTNEXTLINE(bugprone-throwing-static-initialization) - std::mutex-family constructors are noexcept on this toolchain
std::recursive_mutex g_StateMutex;
std::unordered_map<void*, std::unique_ptr<RendererState>> g_ProxyStates;
// The UI-resource baseline and final-batch renderer intentionally coexist during bootstrap. Keeping separate
// maps prevents a still-needed baseline render from retiring the inline-marker state before its first marker
// can prove completion on the next proxy Present.
std::unordered_map<void*, std::unique_ptr<RendererState>> g_InlineProxyStates;
std::vector<std::unique_ptr<RendererState>> g_RetiredStates;
std::atomic<bool> g_ShuttingDown{false};

void PruneRetiredStates() {
    auto it = g_RetiredStates.begin();
    while (it != g_RetiredStates.end()) {
        if ((*it)->IsGpuComplete()) {
            it = g_RetiredStates.erase(it);
        } else {
            ++it;
        }
    }
}

void RetireState(std::unique_ptr<RendererState>& state, const char* reason) {
    if (!state) {
        return;
    }
    state->ReleaseProxyLifetime();
    if (state->IsGpuComplete()) {
        state.reset();
        return;
    }
    HookLogImportant("DX12: Retaining in-flight FSR-suspend overlay resources across %s",
                     reason ? reason : "renderer change");
    g_RetiredStates.emplace_back(std::move(state));
}

}  // namespace

bool Render(const RenderRequest& request) {
    if (g_ShuttingDown.load(std::memory_order_acquire) || !request.proxySwapChain || !request.presentationQueue ||
        (!request.submitCommandList && !request.submitPreparedCommandLists) ||
        (!request.inlineCompletionMarker && !request.signalFence)) {
        return false;
    }

    IDXGISwapChain3* swapChain3Raw = nullptr;
    if (FAILED(request.proxySwapChain->QueryInterface(IID_PPV_ARGS(&swapChain3Raw))) || !swapChain3Raw) {
        return false;
    }
    ComPtr<IDXGISwapChain3> swapChain3;
    swapChain3.Attach(swapChain3Raw);
    const UINT backBufferIndex = swapChain3->GetCurrentBackBufferIndex();
    ComPtr<ID3D12Resource> targetResource;
    HRESULT bufferHr = S_OK;
    if (request.targetResource) {
        targetResource = request.targetResource;
    } else {
        bufferHr = swapChain3->GetBuffer(backBufferIndex, IID_PPV_ARGS(&targetResource));
    }
    if (FAILED(bufferHr) || !targetResource) {
        static std::atomic<int> s_getBufferFailureLogCount{0};
        const int logCount = s_getBufferFailureLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant("DX12: FSR-suspend owner-queue overlay GetBuffer(%u) failed hr=0x%08X log=%d",
                             backBufferIndex, static_cast<unsigned>(bufferHr), logCount + 1);
        }
        return false;
    }

    const D3D12_RESOURCE_DESC desc = targetResource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.Width == 0 || desc.Height == 0 ||
        desc.SampleDesc.Count != 1 || (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) {
        return false;
    }
    const DXGI_FORMAT rtvFormat = ResolveRtvFormat(desc.Format);

    ComPtr<ID3D12Device> backBufferDevice;
    ComPtr<ID3D12Device> queueDevice;
    if (FAILED(targetResource->GetDevice(IID_PPV_ARGS(&backBufferDevice))) || !backBufferDevice ||
        FAILED(request.presentationQueue->GetDevice(IID_PPV_ARGS(&queueDevice))) || !queueDevice ||
        !SameComObject(backBufferDevice.Get(), queueDevice.Get())) {
        static std::atomic<int> s_deviceMismatchLogCount{0};
        const int logCount = s_deviceMismatchLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: FSR-suspend owner-queue overlay rejected queue/device mismatch (queue=%p queueDev=%p "
                "backbuffer=%p bbDev=%p log=%d)",
                request.presentationQueue, queueDevice.Get(), targetResource.Get(), backBufferDevice.Get(),
                logCount + 1);
        }
        return false;
    }

    D3D12_FEATURE_DATA_FORMAT_SUPPORT formatSupport = {};
    formatSupport.Format = rtvFormat;
    constexpr D3D12_FORMAT_SUPPORT1 kRequiredFormatSupport = static_cast<D3D12_FORMAT_SUPPORT1>(
        static_cast<UINT>(D3D12_FORMAT_SUPPORT1_RENDER_TARGET) |
        static_cast<UINT>(D3D12_FORMAT_SUPPORT1_BLENDABLE));
    if (FAILED(backBufferDevice->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &formatSupport,
                                                     sizeof(formatSupport))) ||
        (formatSupport.Support1 & kRequiredFormatSupport) != kRequiredFormatSupport) {
        static std::atomic<int> s_formatRejectLogCount{0};
        const int logCount = s_formatRejectLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 10 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: FSR-suspend owner-queue overlay rejected non-renderable/non-blendable backbuffer format "
                "(resourceFmt=%d rtvFmt=%d support1=0x%X log=%d)",
                static_cast<int>(desc.Format), static_cast<int>(rtvFormat),
                static_cast<unsigned>(formatSupport.Support1), logCount + 1);
        }
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
    if (g_ShuttingDown.load(std::memory_order_acquire)) {
        return false;
    }
    PruneRetiredStates();
    auto& states = request.inlineCompletionMarker ? g_InlineProxyStates : g_ProxyStates;
    auto& state = states[request.proxySwapChain];
    if (state && !state->Matches(backBufferDevice.Get(), request.presentationQueue, rtvFormat,
                                 request.inlineCompletionMarker)) {
        RetireState(state, "device/queue/format change");
    }
    auto createState = [&]() -> bool {
        auto replacement = std::make_unique<RendererState>();
        if (!replacement->Initialize(request.proxySwapChain, backBufferDevice.Get(), request.presentationQueue,
                                     rtvFormat, request.hdr, request.inlineCompletionMarker)) {
            return false;
        }
        state = std::move(replacement);
        return true;
    };
    if (!state) {
        if (!createState()) {
            return false;
        }
    } else {
        state->UpdateHdr(request.hdr);
    }

    const auto result =
        state->Render(targetResource.Get(), backBufferIndex, request.targetState, request.clearTransparent,
                      request.renderOverlay, request.applyPostProcess, request.routeName, request.submitCommandList,
                      request.signalFence, request.embeddedInExistingBatch, request.reservedSlot,
                      request.submitPreparedCommandLists, request.separatePostProcess);
    if (result == RendererState::RenderResult::kRenderedCompletionUnknown) {
        RetireState(state, "completion Signal failure");
        return true;
    }
    if (result == RendererState::RenderResult::kFailed) {
        RetireState(state, "record/submit failure");
    }
    return result == RendererState::RenderResult::kRendered;
}

bool HasCompletedInlineRender(void* proxySwapChain) {
    if (!proxySwapChain || g_ShuttingDown.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
    const auto it = g_InlineProxyStates.find(proxySwapChain);
    return it != g_InlineProxyStates.end() && it->second && it->second->HasCompletedInlineRender();
}

void ResetInlineCompletionProof(void* proxySwapChain) {
    if (!proxySwapChain || g_ShuttingDown.load(std::memory_order_acquire)) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
    const auto it = g_InlineProxyStates.find(proxySwapChain);
    if (it != g_InlineProxyStates.end() && it->second) {
        it->second->ResetInlineCompletionProof();
    }
}

void RetireProxy(void* proxySwapChain, const char* reason) {
    if (!proxySwapChain) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
    PruneRetiredStates();
    for (auto* states : {&g_ProxyStates, &g_InlineProxyStates}) {
        const auto it = states->find(proxySwapChain);
        if (it != states->end()) {
            RetireState(it->second, reason ? reason : "FFX proxy retired");
            states->erase(it);
            HookLog("DX12: FFX overlay renderer retired for swapchain %p (%s)", proxySwapChain,
                    reason ? reason : "FFX proxy retired");
        }
    }
    PruneRetiredStates();
}

void RetireAllForNativeFSRTeardown(const char* reason) {
    if (g_ShuttingDown.load(std::memory_order_acquire)) {
        return;
    }

    const char* const retireReason = reason && reason[0] ? reason : "native FSR teardown";
    std::vector<void*> keys;
    {
        std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
        PruneRetiredStates();
        keys.reserve(g_ProxyStates.size() + g_InlineProxyStates.size());
        for (const auto* states : {&g_ProxyStates, &g_InlineProxyStates}) {
            // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order) - teardown retires every state independently
            for (const auto& entry : *states) {
                if (entry.second && std::find(keys.begin(), keys.end(), entry.first) == keys.end()) {
                    keys.push_back(entry.first);
                }
            }
        }
    }

    size_t retired = 0;
    for (void* key : keys) {
        std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
        for (auto* states : {&g_ProxyStates, &g_InlineProxyStates}) {
            const auto it = states->find(key);
            if (it == states->end() || !it->second) {
                continue;
            }
            RetireState(it->second, retireReason);
            states->erase(it);
            ++retired;
        }
    }
    if (retired != 0) {
        HookLogImportant(
            "DX12: Retired %zu FSR-suspend overlay renderer state(s) at the native-FSR teardown boundary (%s) — "
            "no command-list/backbuffer references survive the FFX swapchain teardown",
            retired, retireReason);
    }
}

void Shutdown(const char* reason) {
    g_ShuttingDown.store(true, std::memory_order_release);
    std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
    for (auto* states : {&g_ProxyStates, &g_InlineProxyStates}) {
        // NOLINTNEXTLINE(bugprone-nondeterministic-pointer-iteration-order) - shutdown retires every state independently
        for (auto& [proxy, state] : *states) {
            (void)proxy;
            RetireState(state, reason ? reason : "shutdown");
        }
        states->clear();
    }

    size_t abandoned = 0;
    for (auto& state : g_RetiredStates) {
        // Device removal makes completion impossible but releases are safe. A healthy incomplete queue is never
        // waited here (Shutdown may run under loader-sensitive teardown); intentionally leak that tiny terminal
        // state rather than block or release GPU-referenced resources.
        if (state && !state->IsGpuComplete() && state->DeviceHealthy()) {
            // NOLINTNEXTLINE(bugprone-unused-return-value) - intentional ownership abandonment on device-removal teardown
            (void)state.release();
            ++abandoned;
        }
    }
    g_RetiredStates.clear();
    if (abandoned != 0) {
        HookLogImportant(
            "DX12: Abandoned %zu in-flight FSR-suspend overlay state(s) during %s to avoid blocking or releasing "
            "GPU-referenced resources (terminating=%d)",
            abandoned, reason ? reason : "shutdown", IsProcessTerminating() ? 1 : 0);
    }
}

}  // namespace ce::dx12_ffx_suspend_overlay
