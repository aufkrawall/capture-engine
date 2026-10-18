#pragma once

#include "dx12_ffx_suspend_overlay.h"
#include "dx12_post_process_routes.h"
#include "hook/sharpen/gamma_external_submission.h"
#include "hook/sharpen/sharpen_request.h"
#include "hook/sharpen/sharpen_d3d11.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#include <wrl/client.h>

#include "hook/present/dxgi_shared.h"
#include "hook/runtime/hook_common.h"
#include "hook/overlay/overlay_adapter.h"

namespace ce::dx12_ffx_suspend_overlay {
namespace detail {

using Microsoft::WRL::ComPtr;

constexpr size_t kFrameSlotCount = 16;
// The last slot is reserved for CE-owned, non-presented targets (RenderRequest::reservedSlot): FSR screenshot renders
// and the transparent retirement of CE's substitute UI texture. Swapchain buffer indices and inline final-batch
// candidates stay below it, so such a submission never shares allocator or upload state with an overlay output that
// is still in flight, nor takes the slot the same frame's output overlay needs.
constexpr size_t kScreenshotSlot = kFrameSlotCount - 1;
constexpr size_t kOverlayOutputSlotCount = kFrameSlotCount - 1;

inline DXGI_FORMAT ResolveRtvFormat(DXGI_FORMAT format) {
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

inline void Transition(ID3D12GraphicsCommandList* commandList, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
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

inline bool SameComObject(IUnknown* left, IUnknown* right) {
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
    ComPtr<ID3D12GraphicsCommandList> postProcessList;
    ComPtr<ID3D12CommandAllocator> postProcessAllocator;
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

        {
            for (auto& slot : slots) {
                hr = device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&slot.postProcessAllocator));
                if (FAILED(hr))
                    return false;
                hr = device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, slot.postProcessAllocator.Get(), nullptr,
                                               IID_PPV_ARGS(&slot.postProcessList));
                if (FAILED(hr) || FAILED(slot.postProcessList->Close()))
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
                        bool clearTransparent, bool renderOverlay, bool applyPostProcess, const char* routeName,
                        SubmitCommandListCallback submitCommandList, SignalFenceCallback signalFence,
                        bool embeddedInExistingBatch, bool reservedSlot,
                        SubmitPreparedCommandListsCallback submitPreparedCommandLists, bool separatePostProcess) {
        if (!targetResource || (!submitCommandList && !submitPreparedCommandLists) ||
            (!inlineCompletionMarker && !signalFence) || !DeviceHealthy()) {
            return RenderResult::kFailed;
        }

        const D3D12_RESOURCE_DESC targetDesc = targetResource->GetDesc();
        const UINT64 completed = fence ? fence->GetCompletedValue() : 0;
        size_t slotIndex = kFrameSlotCount;
        if (reservedSlot) {
            // Only the non-inline fence guard below applies to this slot: an in-flight previous screenshot refuses
            // the render and the caller retries on a later Present instead of waiting here.
            slotIndex = kScreenshotSlot;
        } else if (inlineCompletionMarker) {
            // The final-batch route may have several displayed outputs in flight at once. Command allocators and
            // upload buffers, rather than swapchain-buffer identity, are the resources that require CPU-side
            // completion before reuse; queue order already serializes repeated writes to the same backbuffer.
            // Select any completed slot so one freshly submitted marker cannot revoke a previously proven route.
            const size_t firstCandidate = static_cast<size_t>(frameCount % kOverlayOutputSlotCount);
            for (size_t offset = 0; offset < kOverlayOutputSlotCount; ++offset) {
                const size_t candidate = (firstCandidate + offset) % kOverlayOutputSlotCount;
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
        } else if (backBufferIndex < kOverlayOutputSlotCount) {
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
        ComPtr<ID3D12GraphicsCommandList2> commandList2;
        if (inlineCompletionMarker && (!completionMarkers || completionMarkerGpuVA == 0 ||
                                       FAILED(slot.commandList.As(&commandList2)) || !commandList2)) {
            HookLogImportant(
                "DX12: FSR embedded-batch overlay rejected because WriteBufferImmediate is unavailable "
                "(queue=%p slot=%zu)",
                queue.Get(), slotIndex);
            return RenderResult::kFailed;
        }
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

        ID3D12GraphicsCommandList* postProcessList = slot.commandList.Get();
        if (separatePostProcess) {
            if (!slot.postProcessList || FAILED(slot.postProcessAllocator->Reset()) ||
                FAILED(slot.postProcessList->Reset(slot.postProcessAllocator.Get(), nullptr)))
                return RenderResult::kFailed;
            postProcessList = slot.postProcessList.Get();
        }
        bool postProcessRecorded = false;
        if (ce::post_process_route::MayRecordFsrSuspendOverlayPostProcess(
                {applyPostProcess, renderOverlay, clearTransparent})) {
            const auto postProcess = ce::sharpen::ResolveRequest(GetActiveGraphicsConfigCached());
            const ce::sharpen::TargetEncoding targetEncoding = ce::sharpen::ResolveDxgiEncoding(targetDesc.Format, hdr);
            if (ce::sharpen::Requested(postProcess) && ce::sharpen::RequestHasWork(postProcess, targetEncoding)) {
                postProcessRecorded = ce::sharpen::RecordRuntimePostProcess(
                    device.Get(), postProcessList, targetResource, targetState, postProcess, targetEncoding,
                    inlineCompletionMarker);
                NotePostProcessRouteResult(ce::post_process_route::RuntimeRoute::FsrOverlayOutput,
                                           postProcessRecorded ? ce::post_process_route::PassResult::Applied
                                                               : ce::post_process_route::PassResult::Failed);
            }
        }
        if (separatePostProcess && FAILED(postProcessList->Close())) {
            ce::sharpen::AbandonRuntimePostProcessCommandList(postProcessList);
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
            if (postProcessRecorded) {
                ce::sharpen::AbandonRuntimePostProcessCommandList(postProcessList);
            }
            HookLogImportant(
                "DX12: FSR-suspend owner-queue overlay command-list Close failed (slot=%zu hr=0x%08X bb=%p)", slotIndex,
                static_cast<unsigned>(closeHr), targetResource);
            return RenderResult::kFailed;
        }
        slot.inFlightTarget = targetResource;
        const bool submitted = submitPreparedCommandLists
            ? submitPreparedCommandLists(queue.Get(), separatePostProcess && postProcessRecorded ? postProcessList : nullptr,
                                         slot.commandList.Get())
            : submitCommandList(queue.Get(), slot.commandList.Get());
        if (!submitted) {
            slot.inFlightTarget.Reset();
            if (postProcessRecorded) {
                ce::sharpen::AbandonRuntimePostProcessCommandList(postProcessList);
            }
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

    }  // namespace detail
}  // namespace ce::dx12_ffx_suspend_overlay
