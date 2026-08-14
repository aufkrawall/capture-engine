#include "dx12_ffx_suspend_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
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
    uint32_t markerValue = 0;
    uint64_t submissionSequence = 0;
};

class RendererState {
public:
    enum class RenderResult {
        kRendered,
        kRenderedCompletionUnknown,
        kBackBufferStillInFlight,
        kFailed,
    };

    bool Initialize(IDXGISwapChain* newProxy, ID3D12Device* newDevice, ID3D12CommandQueue* newQueue,
                    DXGI_FORMAT newFormat, bool newHdr, bool newInlineCompletionMarker) {
        if (!newProxy || !newDevice || !newQueue ||
            newQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            return false;
        }

        proxyLifetime = newProxy;
        device = newDevice;
        queue = newQueue;
        format = newFormat;
        hdr = newHdr;
        inlineCompletionMarker = newInlineCompletionMarker;

        HRESULT hr = S_OK;
        if (inlineCompletionMarker) {
            // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
            D3D12_HEAP_PROPERTIES markerHeap = {};
            markerHeap.Type = D3D12_HEAP_TYPE_CUSTOM;
            markerHeap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
            markerHeap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
            D3D12_RESOURCE_DESC markerDesc = {};
            markerDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            markerDesc.Width = static_cast<UINT64>(kFrameSlotCount) * sizeof(uint32_t);
            markerDesc.Height = 1;
            markerDesc.DepthOrArraySize = 1;
            markerDesc.MipLevels = 1;
            markerDesc.SampleDesc.Count = 1;
            markerDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            hr = device->CreateCommittedResource(&markerHeap, D3D12_HEAP_FLAG_NONE, &markerDesc,
                                                 D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                 IID_PPV_ARGS(&completionMarkerBuffer));
            void* mapped = nullptr;
            if (FAILED(hr) || !completionMarkerBuffer ||
                FAILED(completionMarkerBuffer->Map(0, nullptr, &mapped)) || !mapped) {
                HookLogImportant(
                    "DX12: FSR embedded-batch overlay failed to create its inline completion marker hr=0x%08X",
                    static_cast<unsigned>(hr));
                return false;
            }
            completionMarkers = static_cast<volatile uint32_t*>(mapped);
            memset(const_cast<uint32_t*>(completionMarkers), 0,
                   static_cast<size_t>(markerDesc.Width));
            completionMarkerGpuVA = completionMarkerBuffer->GetGPUVirtualAddress();
        } else {
            hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
            if (FAILED(hr) || !fence) {
                HookLogImportant(
                    "DX12: FSR-suspend owner-queue overlay failed to create completion fence hr=0x%08X",
                    static_cast<unsigned>(hr));
                return false;
            }
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
            "slots=%zu completion=%s) — swapchain backbuffer work stays on the FFX game/presentation queue",
            device.Get(), queue.Get(), static_cast<int>(format), hdr ? 1 : 0, kFrameSlotCount,
            inlineCompletionMarker ? "inline-marker/no-queue-signal" : "queue-fence");
        return true;
    }

    bool Matches(ID3D12Device* candidateDevice, ID3D12CommandQueue* candidateQueue, DXGI_FORMAT candidateFormat,
                 bool candidateInlineCompletionMarker) const {
        return SameComObject(device.Get(), candidateDevice) && queue.Get() == candidateQueue &&
               format == candidateFormat && inlineCompletionMarker == candidateInlineCompletionMarker;
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
        if (inlineCompletionMarker) {
            if (!DeviceHealthy()) {
                return true;
            }
            for (size_t i = 0; i < kFrameSlotCount; ++i) {
                if (slots[i].markerValue != 0 &&
                    (!completionMarkers || completionMarkers[i] != slots[i].markerValue)) {
                    return false;
                }
            }
            return true;
        }
        if (completionUnknown) {
            return !DeviceHealthy();
        }
        return !fence || lastSubmittedFenceValue == 0 || fence->GetCompletedValue() >= lastSubmittedFenceValue;
    }

    bool DeviceHealthy() const {
        return device && SUCCEEDED(device->GetDeviceRemovedReason());
    }

    bool HasCompletedInlineRender() {
        if (!inlineCompletionMarker || frameCount == 0) {
            return false;
        }
        if (!inlineCompletionObserved) {
            for (size_t i = 0; i < kFrameSlotCount; ++i) {
                if (slots[i].submissionSequence >= inlineCompletionProofMinSubmission &&
                    slots[i].markerValue != 0 && completionMarkers &&
                    completionMarkers[i] == slots[i].markerValue) {
                    inlineCompletionObserved = true;
                    break;
                }
            }
        }
        return inlineCompletionObserved;
    }

    void ResetInlineCompletionProof() {
        inlineCompletionObserved = false;
        inlineCompletionProofMinSubmission = frameCount + 1;
    }

    RenderResult Render(ID3D12Resource* targetResource, UINT backBufferIndex, D3D12_RESOURCE_STATES targetState,
                        bool clearTransparent, bool renderOverlay, const char* routeName,
                        SubmitCommandListCallback submitCommandList, SignalFenceCallback signalFence,
                        bool embeddedInExistingBatch) {
        if (!targetResource || !submitCommandList || (!inlineCompletionMarker && !signalFence) || !DeviceHealthy()) {
            return RenderResult::kFailed;
        }

        const D3D12_RESOURCE_DESC targetDesc = targetResource->GetDesc();
        const UINT64 completed = fence ? fence->GetCompletedValue() : 0;
        size_t slotIndex = kFrameSlotCount;
        if (inlineCompletionMarker) {
            // The final-batch route may have several displayed outputs in flight at once. Command allocators and
            // upload buffers, rather than swapchain-buffer identity, are the resources that require CPU-side
            // completion before reuse; queue order already serializes repeated writes to the same backbuffer.
            // Select any completed slot so one freshly submitted marker cannot revoke a previously proven route.
            const size_t firstCandidate = static_cast<size_t>(frameCount % kFrameSlotCount);
            for (size_t offset = 0; offset < kFrameSlotCount; ++offset) {
                const size_t candidate = (firstCandidate + offset) % kFrameSlotCount;
                const uint32_t markerValue = slots[candidate].markerValue;
                if (markerValue == 0 || (completionMarkers && completionMarkers[candidate] == markerValue)) {
                    slotIndex = candidate;
                    if (markerValue != 0 &&
                        slots[candidate].submissionSequence >= inlineCompletionProofMinSubmission) {
                        inlineCompletionObserved = true;
                    }
                    break;
                }
            }
        } else if (backBufferIndex < kFrameSlotCount) {
            slotIndex = backBufferIndex;
        }
        if (slotIndex == kFrameSlotCount) {
            if (!inlineCompletionMarker) {
                HookLogImportant(
                    "DX12: FSR-suspend owner-queue overlay rejected invalid backbuffer index %u (slotCapacity=%zu)",
                    backBufferIndex, kFrameSlotCount);
                return RenderResult::kFailed;
            }
            static std::atomic<int> s_noReusableSlotLogCount{0};
            const int logCount = s_noReusableSlotLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 120) == 0) {
                HookLogImportant(
                    "DX12: FSR-suspend owner-queue overlay REFUSED because all %zu inline allocator/upload slots "
                    "are in flight (bbIndex=%u queue=%p log=%d) — no wait, no overwrite",
                    kFrameSlotCount, backBufferIndex, queue.Get(), logCount + 1);
            }
            return RenderResult::kBackBufferStillInFlight;
        }
        const bool markerPending = inlineCompletionMarker && slots[slotIndex].markerValue != 0 &&
                                   (!completionMarkers ||
                                    completionMarkers[slotIndex] != slots[slotIndex].markerValue);
        const bool fencePending = !inlineCompletionMarker && slots[slotIndex].fenceValue != 0 &&
                                  slots[slotIndex].fenceValue > completed;
        if (markerPending || fencePending) {
            // AMD's proxy cannot legally return a replacement-buffer index until its prior gameQueue work has
            // crossed the internal gameFence/presentQueue handoff. Never wait or overwrite here: this is direct
            // evidence that the captured queue/buffer-lifetime assumption is wrong for this provider instance.
            static std::atomic<int> s_reuseViolationLogCount{0};
            const int logCount = s_reuseViolationLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 120) == 0) {
                HookLogImportant(
                    "DX12: FSR-suspend owner-queue overlay REFUSED in-flight backbuffer-slot reuse "
                    "(bbIndex=%u fenceGuard=%llu completed=%llu markerGuard=%u markerDone=%u queue=%p log=%d) — "
                    "no wait, no overwrite",
                    backBufferIndex, static_cast<unsigned long long>(slots[slotIndex].fenceValue),
                    static_cast<unsigned long long>(completed), slots[slotIndex].markerValue,
                    completionMarkers ? completionMarkers[slotIndex] : 0, queue.Get(), logCount + 1);
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

        const UINT64 submitFenceValue = lastSubmittedFenceValue + 1;
        const bool writesTarget = clearTransparent || renderOverlay;
        if (writesTarget) {
            D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
            rtv.ptr += slotIndex * static_cast<SIZE_T>(rtvIncrement);
            D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
            rtvDesc.Format = format;
            rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
            device->CreateRenderTargetView(targetResource, &rtvDesc, rtv);

            Transition(slot.commandList.Get(), targetResource, targetState, D3D12_RESOURCE_STATE_RENDER_TARGET);
            if (clearTransparent) {
                constexpr float kTransparent[4] = {0.0f, 0.0f, 0.0f, 0.0f};
                slot.commandList->ClearRenderTargetView(rtv, kTransparent, 0, nullptr);
            }
            if (renderOverlay) {
                overlay.SetIPCClient(g_IPC);
                overlay.SetReserveInactiveFGSpace(false);
                if (auto* perf = DXGIShared::GetPerformanceMetrics()) {
                    overlay.SetMetrics(perf);
                }
                overlay.SetGraphicsAPI("DX12");
                overlay.SetDX12UploadSlotFence(fence.Get(), inlineCompletionMarker ? 0 : submitFenceValue);
                overlay.SetDX12NextUploadSlot(static_cast<int>(slotIndex));
                overlay.SetDX12RenderTarget(slot.commandList.Get(), reinterpret_cast<void*>(rtv.ptr));
                overlay.RenderOverlay(static_cast<int>(targetDesc.Width), static_cast<int>(targetDesc.Height));
            }
            Transition(slot.commandList.Get(), targetResource, D3D12_RESOURCE_STATE_RENDER_TARGET, targetState);
        }

        uint32_t submitMarkerValue = 0;
        if (inlineCompletionMarker) {
            ComPtr<ID3D12GraphicsCommandList2> commandList2;
            if (!completionMarkers || completionMarkerGpuVA == 0 ||
                FAILED(slot.commandList.As(&commandList2)) || !commandList2) {
                HookLogImportant(
                    "DX12: FSR embedded-batch overlay rejected because WriteBufferImmediate is unavailable "
                    "(queue=%p slot=%zu)",
                    queue.Get(), slotIndex);
                return RenderResult::kFailed;
            }
            submitMarkerValue = nextMarkerValue++;
            if (submitMarkerValue == 0) {
                submitMarkerValue = nextMarkerValue++;
            }
            D3D12_WRITEBUFFERIMMEDIATE_PARAMETER marker = {};
            marker.Dest = completionMarkerGpuVA + static_cast<UINT64>(slotIndex) * sizeof(uint32_t);
            marker.Value = submitMarkerValue;
            const D3D12_WRITEBUFFERIMMEDIATE_MODE markerMode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
            commandList2->WriteBufferImmediate(1, &marker, &markerMode);
        }

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

        if (inlineCompletionMarker) {
            slot.markerValue = submitMarkerValue;
            ++frameCount;
            slot.submissionSequence = frameCount;
            static std::atomic<int> s_markerRenderLogCount{0};
            const int logCount = s_markerRenderLogCount.fetch_add(1, std::memory_order_relaxed);
            if (logCount < 20 || (logCount % 300) == 0) {
                if (embeddedInExistingBatch) {
                    HookLogImportant(
                        "DX12: FSR topmost %s appended to the existing final ECL batch "
                        "(queue=%p frame=%llu bbIndex=%u target=%p marker=%u slot=%zu route=%s draw=%d log=%d) — "
                        "no extra ExecuteCommandLists and no queue Signal",
                        renderOverlay ? "overlay" : "activation probe", queue.Get(),
                        static_cast<unsigned long long>(frameCount), backBufferIndex, targetResource,
                        submitMarkerValue, slotIndex, routeName ? routeName : "unknown", renderOverlay ? 1 : 0,
                        logCount + 1);
                } else {
                    HookLogImportant(
                        "DX12: FSR topmost %s submitted on the owner presentation queue with marker completion "
                        "(queue=%p frame=%llu bbIndex=%u target=%p marker=%u slot=%zu route=%s draw=%d log=%d) — "
                        "one owner-queue ExecuteCommandLists call and no queue Signal",
                        renderOverlay ? "overlay" : "activation probe", queue.Get(),
                        static_cast<unsigned long long>(frameCount), backBufferIndex, targetResource,
                        submitMarkerValue, slotIndex, routeName ? routeName : "unknown", renderOverlay ? 1 : 0,
                        logCount + 1);
                }
            }
            return RenderResult::kRendered;
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
                "completed=%llu bbIndex=%u target=%p %llux%u fmt=%d hdr=%d slot=%zu route=%s clear=%d draw=%d "
                "log=%d) — "
                "queue ordering "
                "guarantees game draw -> overlay -> proxy Present; no foreign queue and no per-frame CPU wait",
                queue.Get(), static_cast<unsigned long long>(frameCount),
                static_cast<unsigned long long>(submitFenceValue),
                static_cast<unsigned long long>(fence->GetCompletedValue()), backBufferIndex, targetResource,
                static_cast<unsigned long long>(targetDesc.Width), targetDesc.Height, static_cast<int>(format),
                hdr ? 1 : 0, slotIndex, routeName ? routeName : "unknown", clearTransparent ? 1 : 0,
                renderOverlay ? 1 : 0, logCount + 1);
        }
        return RenderResult::kRendered;
    }

    void ReleaseProxyLifetime() {
        // The proxy is needed only while this renderer remains addressable by its raw map key. Once retirement
        // removes that key, release the pin immediately so a still-in-flight renderer cannot delay FFX teardown.
        proxyLifetime.Reset();
    }

private:
    // Warm callback/no-callback route changes release the topmost tracker's reference but intentionally keep this
    // live renderer. Pin the exact key so its raw map identity cannot dangle or be ABA-reused between route flips.
    ComPtr<IDXGISwapChain> proxyLifetime;
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12CommandQueue> queue;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool hdr = false;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12Resource> completionMarkerBuffer;
    volatile uint32_t* completionMarkers = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS completionMarkerGpuVA = 0;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    UINT rtvIncrement = 0;
    std::array<FrameSlot, kFrameSlotCount> slots;
    UINT64 lastSubmittedFenceValue = 0;
    uint32_t nextMarkerValue = 1;
    bool inlineCompletionMarker = false;
    bool inlineCompletionObserved = false;
    uint64_t inlineCompletionProofMinSubmission = 1;
    bool completionUnknown = false;
    uint64_t frameCount = 0;
    OverlayAdapter overlay;
};

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
        !request.submitCommandList || (!request.inlineCompletionMarker && !request.signalFence)) {
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
                      request.renderOverlay, request.routeName, request.submitCommandList, request.signalFence,
                      request.embeddedInExistingBatch);
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
