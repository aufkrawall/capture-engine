#include "dx12_ffx_suspend_overlay.h"

#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <wrl/client.h>

#include "../common/dxgi_shared.h"
#include "../common/hook_common.h"
#include "../common/overlay_adapter.h"

namespace ce::dx12_ffx_suspend_overlay {
namespace {

using Microsoft::WRL::ComPtr;

constexpr size_t kFrameSlotCount = 16;

DXGI_FORMAT ResolveRtvFormat(DXGI_FORMAT format) {
    switch (format) {
        case DXGI_FORMAT_R8G8B8A8_TYPELESS:
            return DXGI_FORMAT_R8G8B8A8_UNORM;
        case DXGI_FORMAT_B8G8R8A8_TYPELESS:
            return DXGI_FORMAT_B8G8R8A8_UNORM;
        case DXGI_FORMAT_R10G10B10A2_TYPELESS:
            return DXGI_FORMAT_R10G10B10A2_UNORM;
        case DXGI_FORMAT_R16G16B16A16_TYPELESS:
            return DXGI_FORMAT_R16G16B16A16_FLOAT;
        default:
            return format;
    }
}

void Transition(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
    if (!commandList || !resource || before == after) {
        return;
    }

    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    commandList->ResourceBarrier(1, &barrier);
}

bool SameComObject(IUnknown* left, IUnknown* right) {
    if (!left || !right) {
        return false;
    }
    ComPtr<IUnknown> leftIdentity;
    ComPtr<IUnknown> rightIdentity;
    return SUCCEEDED(left->QueryInterface(IID_PPV_ARGS(&leftIdentity))) &&
           SUCCEEDED(right->QueryInterface(IID_PPV_ARGS(&rightIdentity))) && leftIdentity.Get() == rightIdentity.Get();
}

struct FrameSlot {
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> commandList;
    // D3D12 command lists do not retain referenced resources. Keep the target alive until this slot's
    // completion fence proves the GPU is finished with it. This is required for rotating game-owned FFX UI
    // resources and also makes proxy-backbuffer retirement robust across resize/context destruction.
    ComPtr<ID3D12Resource> inFlightTarget;
    UINT64 fenceValue = 0;
};

class RendererState {
public:
    enum class RenderResult {
        kRendered,
        kRenderedCompletionUnknown,
        kBackBufferStillInFlight,
        kFailed,
    };

    bool Initialize(ID3D12Device* newDevice, ID3D12CommandQueue* newQueue, DXGI_FORMAT newFormat, bool newHdr) {
        if (!newDevice || !newQueue || newQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            return false;
        }

        device = newDevice;
        queue = newQueue;
        format = newFormat;
        hdr = newHdr;

        HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(hr) || !fence) {
            HookLogImportant("DX12: FSR-suspend owner-queue overlay failed to create completion fence hr=0x%08X",
                             static_cast<unsigned>(hr));
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC rtvHeapDesc = {};
        rtvHeapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        rtvHeapDesc.NumDescriptors = static_cast<UINT>(kFrameSlotCount);
        hr = device->CreateDescriptorHeap(&rtvHeapDesc, IID_PPV_ARGS(&rtvHeap));
        if (FAILED(hr) || !rtvHeap) {
            HookLogImportant("DX12: FSR-suspend owner-queue overlay failed to create RTV heap hr=0x%08X",
                             static_cast<unsigned>(hr));
            return false;
        }
        rtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        for (auto& slot : slots) {
            hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.allocator));
            if (FAILED(hr) || !slot.allocator) {
                HookLogImportant("DX12: FSR-suspend owner-queue overlay failed to create command allocator hr=0x%08X",
                                 static_cast<unsigned>(hr));
                return false;
            }
            hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.allocator.Get(), nullptr,
                                           IID_PPV_ARGS(&slot.commandList));
            if (FAILED(hr) || !slot.commandList || FAILED(slot.commandList->Close())) {
                HookLogImportant("DX12: FSR-suspend owner-queue overlay failed to create command list hr=0x%08X",
                                 static_cast<unsigned>(hr));
                return false;
            }
        }

        overlay.SetHwnd(nullptr);
        if (!overlay.InitDX12(device.Get(), queue.Get(), static_cast<int>(format))) {
            HookLogImportant("DX12: FSR-suspend owner-queue overlay failed to initialize adapter fmt=%d",
                             static_cast<int>(format));
            return false;
        }
        overlay.SetHDR(hdr, static_cast<int>(format));
        HookLogImportant(
            "DX12: FSR-suspend owner-queue overlay initialized (device=%p presentationQueue=%p fmt=%d hdr=%d "
            "slots=%zu) — swapchain backbuffer work stays on the FFX game/presentation queue",
            device.Get(), queue.Get(), static_cast<int>(format), hdr ? 1 : 0, kFrameSlotCount);
        return true;
    }

    bool Matches(ID3D12Device* candidateDevice, ID3D12CommandQueue* candidateQueue, DXGI_FORMAT candidateFormat) const {
        return SameComObject(device.Get(), candidateDevice) && queue.Get() == candidateQueue &&
               format == candidateFormat;
    }

    void UpdateHdr(bool newHdr) {
        if (hdr != newHdr) {
            hdr = newHdr;
            overlay.SetHDR(hdr, static_cast<int>(format));
            HookLogImportant("DX12: FSR-suspend owner-queue overlay HDR state changed (fmt=%d hdr=%d)",
                             static_cast<int>(format), hdr ? 1 : 0);
        }
    }

    bool IsGpuComplete() const {
        if (completionUnknown) {
            return !DeviceHealthy();
        }
        return !fence || lastSubmittedFenceValue == 0 || fence->GetCompletedValue() >= lastSubmittedFenceValue;
    }

    bool DeviceHealthy() const {
        return device && SUCCEEDED(device->GetDeviceRemovedReason());
    }

    RenderResult Render(ID3D12Resource* targetResource, UINT backBufferIndex, D3D12_RESOURCE_STATES targetState,
                        bool clearTransparent, const char* routeName, SubmitCommandListCallback submitCommandList,
                        SignalFenceCallback signalFence) {
        if (!targetResource || !submitCommandList || !signalFence || !DeviceHealthy()) {
            return RenderResult::kFailed;
        }

        const D3D12_RESOURCE_DESC targetDesc = targetResource->GetDesc();
        const UINT64 completed = fence->GetCompletedValue();
        if (backBufferIndex >= kFrameSlotCount) {
            HookLogImportant(
                "DX12: FSR-suspend owner-queue overlay rejected invalid backbuffer index %u (slotCapacity=%zu)",
                backBufferIndex, kFrameSlotCount);
            return RenderResult::kFailed;
        }
        const size_t slotIndex = backBufferIndex;
        if (slots[slotIndex].fenceValue != 0 && slots[slotIndex].fenceValue > completed) {
            // AMD's proxy cannot legally return a replacement-buffer index until its prior gameQueue work has
            // crossed the internal gameFence/presentQueue handoff. Never wait or overwrite here: this is direct
            // evidence that the captured queue/buffer-lifetime assumption is wrong for this provider instance.
            static std::atomic<int> s_reuseViolationLogCount{0};
            const int logCount = s_reuseViolationLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 120) == 0) {
                HookLogImportant(
                    "DX12: FSR-suspend owner-queue overlay REFUSED in-flight backbuffer-slot reuse "
                    "(bbIndex=%u guard=%llu completed=%llu queue=%p log=%d) — no wait, no overwrite",
                    backBufferIndex, static_cast<unsigned long long>(slots[slotIndex].fenceValue),
                    static_cast<unsigned long long>(completed), queue.Get(), logCount + 1);
            }
            return RenderResult::kBackBufferStillInFlight;
        }

        FrameSlot& slot = slots[slotIndex];
        // Completion was established above, so releasing the target retained by the previous use of this
        // slot is safe. Assigning the new target before submission keeps it alive through GPU execution.
        slot.inFlightTarget.Reset();
        const HRESULT allocatorHr = slot.allocator->Reset();
        const HRESULT listHr = SUCCEEDED(allocatorHr) ? slot.commandList->Reset(slot.allocator.Get(), nullptr) : E_FAIL;
        if (FAILED(allocatorHr) || FAILED(listHr)) {
            HookLogImportant(
                "DX12: FSR-suspend owner-queue overlay reset failed (slot=%zu allocHr=0x%08X listHr=0x%08X)", slotIndex,
                static_cast<unsigned>(allocatorHr), static_cast<unsigned>(listHr));
            return RenderResult::kFailed;
        }

        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += slotIndex * static_cast<SIZE_T>(rtvIncrement);
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = format;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(targetResource, &rtvDesc, rtv);

        const UINT64 submitFenceValue = lastSubmittedFenceValue + 1;
        Transition(slot.commandList.Get(), targetResource, targetState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        if (clearTransparent) {
            constexpr float kTransparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
            slot.commandList->ClearRenderTargetView(rtv, kTransparent, 0, nullptr);
        }
        overlay.SetIPCClient(g_IPC);
        overlay.SetReserveInactiveFGSpace(false);
        if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
            overlay.SetMetrics(perf);
        }
        overlay.SetGraphicsAPI("DX12");
        overlay.SetDX12UploadSlotFence(fence.Get(), submitFenceValue);
        overlay.SetDX12NextUploadSlot(static_cast<int>(slotIndex));
        overlay.SetDX12RenderTarget(slot.commandList.Get(), reinterpret_cast<void*>(rtv.ptr));
        overlay.RenderOverlay(static_cast<int>(targetDesc.Width), static_cast<int>(targetDesc.Height));
        Transition(slot.commandList.Get(), targetResource, D3D12_RESOURCE_STATE_RENDER_TARGET, targetState);

        const HRESULT closeHr = slot.commandList->Close();
        if (FAILED(closeHr)) {
            HookLogImportant(
                "DX12: FSR-suspend owner-queue overlay command-list Close failed (slot=%zu hr=0x%08X bb=%p)", slotIndex,
                static_cast<unsigned>(closeHr), targetResource);
            return RenderResult::kFailed;
        }
        slot.inFlightTarget = targetResource;
        if (!submitCommandList(queue.Get(), slot.commandList.Get())) {
            slot.inFlightTarget.Reset();
            HookLogImportant(
                "DX12: FSR-suspend owner-queue overlay submit callback rejected command list (queue=%p slot=%zu)",
                queue.Get(), slotIndex);
            return RenderResult::kFailed;
        }

        const HRESULT signalHr = signalFence(queue.Get(), fence.Get(), submitFenceValue);
        if (FAILED(signalHr)) {
            // The command list was already submitted. Never recycle or destroy this state without completion
            // proof. Queue order still places the recorded overlay before Present, so this frame is drawable.
            slot.fenceValue = UINT64_MAX;
            lastSubmittedFenceValue = submitFenceValue;
            completionUnknown = true;
            HookLogImportant(
                "DX12: FSR-suspend owner-queue overlay fence Signal failed (queue=%p slot=%zu value=%llu "
                "hr=0x%08X)",
                queue.Get(), slotIndex, static_cast<unsigned long long>(submitFenceValue),
                static_cast<unsigned>(signalHr));
            return RenderResult::kRenderedCompletionUnknown;
        }

        slot.fenceValue = submitFenceValue;
        lastSubmittedFenceValue = submitFenceValue;
        ++frameCount;
        static std::atomic<int> s_renderLogCount{0};
        const int logCount = s_renderLogCount.fetch_add(1, std::memory_order_relaxed);
        if (logCount < 20 || (logCount % 300) == 0) {
            HookLogImportant(
                "DX12: FSR-suspend overlay submitted on FFX OWNER presentation queue %p (frame=%llu fence=%llu "
                "completed=%llu bbIndex=%u target=%p %llux%u fmt=%d hdr=%d slot=%zu route=%s clear=%d log=%d) — "
                "queue ordering "
                "guarantees game draw -> overlay -> proxy Present; no foreign queue and no per-frame CPU wait",
                queue.Get(), static_cast<unsigned long long>(frameCount),
                static_cast<unsigned long long>(submitFenceValue),
                static_cast<unsigned long long>(fence->GetCompletedValue()), backBufferIndex, targetResource,
                static_cast<unsigned long long>(targetDesc.Width), targetDesc.Height, static_cast<int>(format),
                hdr ? 1 : 0, slotIndex, routeName ? routeName : "unknown", clearTransparent ? 1 : 0, logCount + 1);
        }
        return RenderResult::kRendered;
    }

private:
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool hdr = false;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    UINT rtvIncrement = 0;
    std::array<FrameSlot, kFrameSlotCount> slots;
    UINT64 lastSubmittedFenceValue = 0;
    bool completionUnknown = false;
    uint64_t frameCount = 0;
    OverlayAdapter overlay;
};

std::recursive_mutex g_StateMutex;
std::unordered_map<void*, std::unique_ptr<RendererState>> g_ProxyStates;
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
        !request.submitCommandList || !request.signalFence) {
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
    constexpr D3D12_FORMAT_SUPPORT1 kRequiredFormatSupport =
        static_cast<D3D12_FORMAT_SUPPORT1>(D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_BLENDABLE);
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
    auto& state = g_ProxyStates[request.proxySwapChain];
    if (state && !state->Matches(backBufferDevice.Get(), request.presentationQueue, rtvFormat)) {
        RetireState(state, "device/queue/format change");
    }
    auto createState = [&]() -> bool {
        auto replacement = std::make_unique<RendererState>();
        if (!replacement->Initialize(backBufferDevice.Get(), request.presentationQueue, rtvFormat, request.hdr)) {
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
                      request.routeName, request.submitCommandList, request.signalFence);
    if (result == RendererState::RenderResult::kRenderedCompletionUnknown) {
        RetireState(state, "completion Signal failure");
        return true;
    }
    if (result == RendererState::RenderResult::kFailed) {
        RetireState(state, "record/submit failure");
    }
    return result == RendererState::RenderResult::kRendered;
}

void RetireProxy(void* proxySwapChain, const char* reason) {
    if (!proxySwapChain) {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
    PruneRetiredStates();
    const auto it = g_ProxyStates.find(proxySwapChain);
    if (it != g_ProxyStates.end()) {
        RetireState(it->second, reason ? reason : "FFX proxy retired");
        g_ProxyStates.erase(it);
    }
    PruneRetiredStates();
}

void Shutdown(const char* reason) {
    g_ShuttingDown.store(true, std::memory_order_release);
    std::lock_guard<std::recursive_mutex> lock(g_StateMutex);
    for (auto& [proxy, state] : g_ProxyStates) {
        (void)proxy;
        RetireState(state, reason ? reason : "shutdown");
    }
    g_ProxyStates.clear();

    size_t abandoned = 0;
    for (auto& state : g_RetiredStates) {
        // Device removal makes completion impossible but releases are safe. A healthy incomplete queue is never
        // waited here (Shutdown may run under loader-sensitive teardown); intentionally leak that tiny terminal
        // state rather than block or release GPU-referenced resources.
        if (state && !state->IsGpuComplete() && state->DeviceHealthy()) {
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
