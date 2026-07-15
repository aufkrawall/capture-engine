#include "dx12_streamline_ui_overlay.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <memory>
#include <mutex>
#include <vector>

#include <wrl/client.h>

#include "../common/dxgi_shared.h"
#include "../common/hook_common.h"
#include "../common/ipc_client.h"
#include "../common/overlay_adapter.h"

namespace ce::dx12_streamline_ui_overlay {
namespace {

using Microsoft::WRL::ComPtr;

constexpr size_t kSlotCount = 16;
constexpr uint32_t kMaximumCoveragePresents = 6;

enum class BootstrapPhase : uint8_t {
    kInactive,
    kStandbyIdle,
    kStandbyPendingSubmission,
    kStandbySubmitted,
    kActivationIdle,
    kActivationPendingSubmission,
    kActivationSubmitted,
    kFinished,
};

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

void Transition(ID3D12GraphicsCommandList* list, ID3D12Resource* resource, D3D12_RESOURCE_STATES before,
                D3D12_RESOURCE_STATES after) {
    if (!list || !resource || before == after) {
        return;
    }
    D3D12_RESOURCE_BARRIER barrier = {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = resource;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    barrier.Transition.StateBefore = before;
    barrier.Transition.StateAfter = after;
    list->ResourceBarrier(1, &barrier);
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

bool ContainsCommandList(ID3D12GraphicsCommandList* wanted, UINT count, ID3D12CommandList* const* lists) {
    if (!wanted || !lists) {
        return false;
    }
    for (UINT i = 0; i < count; ++i) {
        if (lists[i] == wanted) {
            return true;
        }
    }
    return false;
}

struct Slot {
    ComPtr<ID3D12Resource> target;
    ID3D12GraphicsCommandList* pendingCommandList = nullptr;
    UINT64 fenceValue = 0;
    bool pending = false;
};

class RendererState {
public:
    bool Initialize(ID3D12Device* newDevice, ID3D12CommandQueue* initializationQueue, DXGI_FORMAT newFormat,
                    bool newHdr) {
        if (!newDevice || !initializationQueue ||
            initializationQueue->GetDesc().Type != D3D12_COMMAND_LIST_TYPE_DIRECT) {
            return false;
        }
        ComPtr<ID3D12Device> queueDevice;
        if (FAILED(initializationQueue->GetDevice(IID_PPV_ARGS(&queueDevice))) ||
            !SameComObject(newDevice, queueDevice.Get())) {
            return false;
        }

        device = newDevice;
        format = newFormat;
        hdr = newHdr;
        HRESULT hr = device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
        if (FAILED(hr) || !fence) {
            return false;
        }
        D3D12_DESCRIPTOR_HEAP_DESC heapDesc = {};
        heapDesc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heapDesc.NumDescriptors = static_cast<UINT>(kSlotCount);
        hr = device->CreateDescriptorHeap(&heapDesc, IID_PPV_ARGS(&rtvHeap));
        if (FAILED(hr) || !rtvHeap) {
            return false;
        }
        rtvIncrement = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);

        overlay.SetHwnd(nullptr);
        if (!overlay.InitDX12(device.Get(), initializationQueue, static_cast<int>(format))) {
            return false;
        }
        overlay.SetHDR(hdr, static_cast<int>(format));
        HookLogImportant(
            "DX12: Streamline UI bootstrap overlay initialized (device=%p initQueue=%p fmt=%d hdr=%d slots=%zu)",
            device.Get(), initializationQueue, static_cast<int>(format), hdr ? 1 : 0, kSlotCount);
        return true;
    }

    bool Matches(ID3D12Device* candidateDevice, DXGI_FORMAT candidateFormat) const {
        return SameComObject(device.Get(), candidateDevice) && format == candidateFormat;
    }

    bool IsGpuComplete() const {
        if (!fence) {
            return true;
        }
        for (const Slot& slot : slots) {
            if (slot.pending || slot.fenceValue == UINT64_MAX ||
                (slot.fenceValue != 0 && fence->GetCompletedValue() < slot.fenceValue)) {
                return false;
            }
        }
        return true;
    }

    bool DeviceHealthy() const {
        return device && SUCCEEDED(device->GetDeviceRemovedReason());
    }

    bool Record(const RecordRequest& request, size_t& recordedSlot) {
        const UINT64 completed = fence->GetCompletedValue();
        size_t slotIndex = kSlotCount;
        for (size_t i = 0; i < kSlotCount; ++i) {
            if (!slots[i].pending && slots[i].fenceValue != UINT64_MAX &&
                (slots[i].fenceValue == 0 || slots[i].fenceValue <= completed)) {
                slotIndex = i;
                break;
            }
        }
        if (slotIndex == kSlotCount) {
            HookLogImportant(
                "DX12: Streamline UI bootstrap overlay skipped — all %zu upload/RTV slots are in flight "
                "(completed=%llu)",
                kSlotCount, static_cast<unsigned long long>(completed));
            return false;
        }

        Slot& slot = slots[slotIndex];
        slot.target.Reset();
        D3D12_CPU_DESCRIPTOR_HANDLE rtv = rtvHeap->GetCPUDescriptorHandleForHeapStart();
        rtv.ptr += slotIndex * static_cast<SIZE_T>(rtvIncrement);
        D3D12_RENDER_TARGET_VIEW_DESC rtvDesc = {};
        rtvDesc.Format = format;
        rtvDesc.ViewDimension = D3D12_RTV_DIMENSION_TEXTURE2D;
        device->CreateRenderTargetView(request.uiResource, &rtvDesc, rtv);

        const UINT64 guardValue = ++lastFenceValue;
        Transition(request.commandList, request.uiResource, request.resourceState, D3D12_RESOURCE_STATE_RENDER_TARGET);
        overlay.SetIPCClient(g_IPC);
        overlay.SetReserveInactiveFGSpace(false);
        if (auto* metrics = DXGIShared::GetPerformanceMetrics()) {
            overlay.SetMetrics(metrics);
        }
        overlay.SetGraphicsAPI("DX12");
        overlay.SetDX12UploadSlotFence(fence.Get(), guardValue);
        overlay.SetDX12NextUploadSlot(static_cast<int>(slotIndex));
        overlay.SetDX12RenderTarget(request.commandList, reinterpret_cast<void*>(rtv.ptr));
        overlay.RenderOverlay(static_cast<int>(request.width), static_cast<int>(request.height));
        Transition(request.commandList, request.uiResource, D3D12_RESOURCE_STATE_RENDER_TARGET, request.resourceState);

        slot.target = request.uiResource;
        slot.pendingCommandList = request.commandList;
        slot.fenceValue = guardValue;
        slot.pending = true;
        recordedSlot = slotIndex;
        return true;
    }

    void CompleteSubmittedLists(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* commandLists) {
        if (!queue) {
            return;
        }
        for (Slot& slot : slots) {
            if (!slot.pending || !ContainsCommandList(slot.pendingCommandList, count, commandLists)) {
                continue;
            }
            const HRESULT signalHr = queue->Signal(fence.Get(), slot.fenceValue);
            slot.pending = false;
            slot.pendingCommandList = nullptr;
            if (FAILED(signalHr)) {
                slot.fenceValue = UINT64_MAX;
                HookLogImportant(
                    "DX12: Streamline UI bootstrap completion Signal failed (queue=%p hr=0x%08X); retaining slot "
                    "permanently",
                    queue, static_cast<unsigned>(signalHr));
            }
        }
    }

private:
    ComPtr<ID3D12Device> device;
    ComPtr<ID3D12Fence> fence;
    ComPtr<ID3D12DescriptorHeap> rtvHeap;
    UINT rtvIncrement = 0;
    DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
    bool hdr = false;
    UINT64 lastFenceValue = 0;
    std::array<Slot, kSlotCount> slots;
    OverlayAdapter overlay;
};

std::recursive_mutex g_Mutex;
std::atomic<bool> g_FrameTagTrackingActive{false};
std::atomic<bool> g_ActiveCoverage{false};
std::unique_ptr<RendererState> g_Renderer;
std::vector<std::unique_ptr<RendererState>> g_RetiredRenderers;
BootstrapPhase g_Phase = BootstrapPhase::kInactive;
bool g_PreactivationStandbyEnabled = false;
const void* g_FrameToken = nullptr;
ID3D12GraphicsCommandList* g_ActivationCommandList = nullptr;
uint32_t g_MaximumOutputPresents = 1;
uint32_t g_CoveragePresentsRemaining = 0;
uint64_t g_ActivationEpoch = 0;
bool g_AdoptedStandbyNeedsActivationFrameRecord = false;

void PruneRetiredRenderers() {
    auto it = g_RetiredRenderers.begin();
    while (it != g_RetiredRenderers.end()) {
        if ((*it)->IsGpuComplete() || !(*it)->DeviceHealthy()) {
            it = g_RetiredRenderers.erase(it);
        } else {
            ++it;
        }
    }
}

void RetireRenderer(std::unique_ptr<RendererState>& renderer) {
    if (!renderer) {
        return;
    }
    if (renderer->IsGpuComplete() || !renderer->DeviceHealthy()) {
        renderer.reset();
    } else {
        g_RetiredRenderers.emplace_back(std::move(renderer));
    }
}

}  // namespace

void BeginPreactivationStandby(uint32_t maximumOutputPresents) {
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    g_PreactivationStandbyEnabled = true;
    g_MaximumOutputPresents = std::clamp(maximumOutputPresents, 1u, kMaximumCoveragePresents);
    if (g_Phase == BootstrapPhase::kInactive || g_Phase == BootstrapPhase::kFinished) {
        g_Phase = BootstrapPhase::kStandbyIdle;
        g_FrameToken = nullptr;
        g_ActivationCommandList = nullptr;
        g_CoveragePresentsRemaining = 0;
        g_AdoptedStandbyNeedsActivationFrameRecord = false;
        g_ActiveCoverage.store(false, std::memory_order_release);
        g_FrameTagTrackingActive.store(true, std::memory_order_release);
        HookLogImportant(
            "DX12: Streamline UI preactivation standby armed (maxCoveredOutputs=%u) — inactive-DLSS UI tags "
            "will carry the overlay without copies, extra submits, queues, or waits",
            g_MaximumOutputPresents);
    }
}

void EndPreactivationStandby(const char* reason) {
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    g_PreactivationStandbyEnabled = false;
    if (g_Phase == BootstrapPhase::kStandbyIdle || g_Phase == BootstrapPhase::kStandbyPendingSubmission ||
        g_Phase == BootstrapPhase::kStandbySubmitted) {
        g_Phase = BootstrapPhase::kInactive;
        g_FrameToken = nullptr;
        g_ActivationCommandList = nullptr;
        g_CoveragePresentsRemaining = 0;
        g_AdoptedStandbyNeedsActivationFrameRecord = false;
        g_ActiveCoverage.store(false, std::memory_order_release);
        g_FrameTagTrackingActive.store(false, std::memory_order_release);
        HookLogImportant("DX12: Streamline UI preactivation standby disarmed (%s)",
                         reason ? reason : "Streamline unavailable");
    }
}

void BeginActivation(uint32_t maximumOutputPresents) {
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    if (g_Phase == BootstrapPhase::kActivationIdle || g_Phase == BootstrapPhase::kActivationPendingSubmission ||
        g_Phase == BootstrapPhase::kActivationSubmitted) {
        return;
    }
    ++g_ActivationEpoch;
    g_MaximumOutputPresents = std::clamp(maximumOutputPresents, 1u, kMaximumCoveragePresents);

    const bool adoptedSubmittedStandby = g_Phase == BootstrapPhase::kStandbySubmitted;
    const bool adoptedPendingStandby = g_Phase == BootstrapPhase::kStandbyPendingSubmission;
    g_AdoptedStandbyNeedsActivationFrameRecord = adoptedSubmittedStandby || adoptedPendingStandby;
    if (adoptedSubmittedStandby) {
        g_Phase = BootstrapPhase::kActivationSubmitted;
        g_ActivationCommandList = nullptr;
        g_CoveragePresentsRemaining = g_MaximumOutputPresents;
        g_ActiveCoverage.store(true, std::memory_order_release);
    } else if (adoptedPendingStandby) {
        g_Phase = BootstrapPhase::kActivationPendingSubmission;
        g_CoveragePresentsRemaining = 0;
        g_ActiveCoverage.store(false, std::memory_order_release);
    } else {
        g_Phase = BootstrapPhase::kActivationIdle;
        g_FrameToken = nullptr;
        g_ActivationCommandList = nullptr;
        g_CoveragePresentsRemaining = 0;
        g_ActiveCoverage.store(false, std::memory_order_release);
    }
    g_FrameTagTrackingActive.store(true, std::memory_order_release);
    if (adoptedSubmittedStandby || adoptedPendingStandby) {
        HookLogImportant(
            "DX12: Streamline UI bootstrap adopted %s preactivation standby record for DLSS activation epoch "
            "%llu (maxCoveredOutputs=%u)",
            adoptedSubmittedStandby ? "submitted" : "pending", static_cast<unsigned long long>(g_ActivationEpoch),
            g_MaximumOutputPresents);
    } else {
        HookLogImportant(
            "DX12: Streamline UI bootstrap overlay armed for DLSS activation epoch %llu (maxCoveredOutputs=%u)",
            static_cast<unsigned long long>(g_ActivationEpoch), g_MaximumOutputPresents);
    }
}

void EndActivation(const char* reason) {
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    if (g_Phase == BootstrapPhase::kInactive) {
        return;
    }
    g_Phase = g_PreactivationStandbyEnabled ? BootstrapPhase::kStandbyIdle : BootstrapPhase::kInactive;
    g_FrameToken = nullptr;
    g_ActivationCommandList = nullptr;
    g_CoveragePresentsRemaining = 0;
    g_AdoptedStandbyNeedsActivationFrameRecord = false;
    g_ActiveCoverage.store(false, std::memory_order_release);
    g_FrameTagTrackingActive.store(g_PreactivationStandbyEnabled, std::memory_order_release);
    HookLogImportant("DX12: Streamline UI bootstrap overlay %s (%s)",
                     g_PreactivationStandbyEnabled ? "returned to preactivation standby" : "disarmed",
                     reason ? reason : "DLSS off");
}

bool OnFrameTag(const void* frameToken) {
    if (!g_FrameTagTrackingActive.load(std::memory_order_acquire)) {
        return false;
    }
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    if (g_Phase == BootstrapPhase::kInactive || g_Phase == BootstrapPhase::kFinished || !frameToken) {
        return false;
    }
    if (!g_FrameToken) {
        g_FrameToken = frameToken;
        return g_Phase == BootstrapPhase::kStandbyIdle || g_Phase == BootstrapPhase::kActivationIdle;
    }
    if (g_FrameToken == frameToken) {
        return g_Phase == BootstrapPhase::kStandbyIdle || g_Phase == BootstrapPhase::kActivationIdle;
    }

    if (g_Phase == BootstrapPhase::kStandbyIdle) {
        g_FrameToken = frameToken;
        return true;
    }
    if (g_Phase == BootstrapPhase::kStandbySubmitted) {
        g_FrameToken = frameToken;
        g_Phase = BootstrapPhase::kStandbyIdle;
        g_ActivationCommandList = nullptr;
        return true;
    }
    if (g_Phase == BootstrapPhase::kStandbyPendingSubmission) {
        static std::atomic<int> s_overlappingStandbyTagLogCount{0};
        const int n = s_overlappingStandbyTagLogCount.fetch_add(1, std::memory_order_relaxed);
        if (n < 10 || (n % 120) == 0) {
            HookLogImportant(
                "DX12: Streamline UI preactivation standby saw a new frame before the prior tag command list "
                "was submitted (epoch=%llu log=%d) — retaining the prior exact-submission record",
                static_cast<unsigned long long>(g_ActivationEpoch), n + 1);
        }
        return false;
    }

    if (g_AdoptedStandbyNeedsActivationFrameRecord &&
        (g_Phase == BootstrapPhase::kActivationPendingSubmission ||
         g_Phase == BootstrapPhase::kActivationSubmitted)) {
        // Standby resources use eValidUntilPresent. An activation reported after the standby
        // frame's Present can provisionally adopt that record, but a different frame token proves
        // that its lifetime ended. Record into this first real activation frame instead of
        // retiring coverage before PostSL has produced any output. Renderer slots independently
        // retain any older pending command list until its exact app submission completes.
        const void* adoptedFrameToken = g_FrameToken;
        g_FrameToken = frameToken;
        g_Phase = BootstrapPhase::kActivationIdle;
        g_ActivationCommandList = nullptr;
        g_CoveragePresentsRemaining = 0;
        g_AdoptedStandbyNeedsActivationFrameRecord = false;
        g_ActiveCoverage.store(false, std::memory_order_release);
        HookLogImportant(
            "DX12: Streamline UI bootstrap rolling adopted standby record into first activation frame "
            "(activationEpoch=%llu standbyFrame=%p activationFrame=%p) — prior eValidUntilPresent lifetime "
            "ended; requesting exact current-tag coverage",
            static_cast<unsigned long long>(g_ActivationEpoch), adoptedFrameToken, frameToken);
        return true;
    }

    g_FrameToken = frameToken;
    if (g_Phase == BootstrapPhase::kActivationIdle) {
        g_Phase = BootstrapPhase::kFinished;
        g_ActiveCoverage.store(false, std::memory_order_release);
        g_FrameTagTrackingActive.store(false, std::memory_order_release);
        HookLogImportant(
            "DX12: Streamline UI bootstrap activation frame ended without a usable full-frame UI tag "
            "(activationEpoch=%llu) — retaining normal PostSL fallback",
            static_cast<unsigned long long>(g_ActivationEpoch));
    } else if (g_Phase == BootstrapPhase::kActivationSubmitted) {
        g_CoveragePresentsRemaining = 0;
        g_Phase = BootstrapPhase::kFinished;
        g_ActivationCommandList = nullptr;
        g_ActiveCoverage.store(false, std::memory_order_release);
        g_FrameTagTrackingActive.store(false, std::memory_order_release);
        HookLogImportant("DX12: Streamline UI bootstrap coverage retired at the next frame tag (activationEpoch=%llu)",
                         static_cast<unsigned long long>(g_ActivationEpoch));
    }
    return false;
}

bool TryRecordBootstrap(const RecordRequest& request) {
    if (!request.commandList || !request.uiResource || !request.initializationQueue || !request.frameToken ||
        request.width == 0 || request.height == 0 ||
        request.resourceState == static_cast<D3D12_RESOURCE_STATES>(UINT_MAX)) {
        return false;
    }
    if (!g_IPC || !g_IPC->GetSharedMem() || !g_IPC->GetSharedMem()->ReadOverlayConfig().showOverlay) {
        return false;
    }

    const D3D12_RESOURCE_DESC desc = request.uiResource->GetDesc();
    if (desc.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || desc.SampleDesc.Count != 1 ||
        (desc.Flags & D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET) == 0) {
        return false;
    }
    const DXGI_FORMAT rtvFormat =
        ResolveRtvFormat(request.format != DXGI_FORMAT_UNKNOWN ? request.format : desc.Format);
    ComPtr<ID3D12Device> resourceDevice;
    ComPtr<ID3D12Device> listDevice;
    if (FAILED(request.uiResource->GetDevice(IID_PPV_ARGS(&resourceDevice))) ||
        FAILED(request.commandList->GetDevice(IID_PPV_ARGS(&listDevice))) ||
        !SameComObject(resourceDevice.Get(), listDevice.Get())) {
        return false;
    }
    D3D12_FEATURE_DATA_FORMAT_SUPPORT support = {};
    support.Format = rtvFormat;
    constexpr D3D12_FORMAT_SUPPORT1 required =
        static_cast<D3D12_FORMAT_SUPPORT1>(D3D12_FORMAT_SUPPORT1_RENDER_TARGET | D3D12_FORMAT_SUPPORT1_BLENDABLE);
    if (FAILED(resourceDevice->CheckFeatureSupport(D3D12_FEATURE_FORMAT_SUPPORT, &support, sizeof(support))) ||
        (support.Support1 & required) != required) {
        return false;
    }

    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    const bool standbyRecord = g_Phase == BootstrapPhase::kStandbyIdle;
    if ((!standbyRecord && g_Phase != BootstrapPhase::kActivationIdle) || g_FrameToken != request.frameToken) {
        return false;
    }
    PruneRetiredRenderers();
    if (g_Renderer && !g_Renderer->Matches(resourceDevice.Get(), rtvFormat)) {
        RetireRenderer(g_Renderer);
    }
    if (!g_Renderer) {
        auto renderer = std::make_unique<RendererState>();
        if (!renderer->Initialize(resourceDevice.Get(), request.initializationQueue, rtvFormat, request.hdr)) {
            return false;
        }
        g_Renderer = std::move(renderer);
    }

    size_t slot = 0;
    RecordRequest resolved = request;
    resolved.format = rtvFormat;
    if (!g_Renderer->Record(resolved, slot)) {
        return false;
    }
    g_Phase = standbyRecord ? BootstrapPhase::kStandbyPendingSubmission : BootstrapPhase::kActivationPendingSubmission;
    g_ActivationCommandList = request.commandList;
    static std::atomic<uint64_t> s_standbyRecordLogCount{0};
    const uint64_t standbyLog = standbyRecord ? s_standbyRecordLogCount.fetch_add(1, std::memory_order_relaxed) + 1 : 0;
    if (!standbyRecord || standbyLog <= 8 || (standbyLog % 300) == 0) {
        HookLogImportant(
            "DX12: Recorded inject overlay into Streamline UIColorAndAlpha %s resource (epoch=%llu slot=%zu "
            "frame=%p cmd=%p ui=%p %ux%u fmt=%d state=0x%X log=%llu) — no copy, extra submit, queue, or wait",
            standbyRecord ? "preactivation standby" : "bootstrap", static_cast<unsigned long long>(g_ActivationEpoch),
            slot, request.frameToken, request.commandList, request.uiResource, request.width, request.height,
            static_cast<int>(rtvFormat), static_cast<unsigned>(request.resourceState),
            static_cast<unsigned long long>(standbyLog));
    }
    return true;
}

bool BeforeExecuteCommandLists(UINT count, ID3D12CommandList* const* commandLists) {
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    const bool standbySubmission = g_Phase == BootstrapPhase::kStandbyPendingSubmission;
    if (!g_Renderer || (!standbySubmission && g_Phase != BootstrapPhase::kActivationPendingSubmission) ||
        !ContainsCommandList(g_ActivationCommandList, count, commandLists)) {
        return false;
    }
    g_Phase = standbySubmission ? BootstrapPhase::kStandbySubmitted : BootstrapPhase::kActivationSubmitted;
    g_ActivationCommandList = nullptr;
    g_CoveragePresentsRemaining = standbySubmission ? 0 : g_MaximumOutputPresents;
    g_ActiveCoverage.store(!standbySubmission, std::memory_order_release);
    if (!standbySubmission) {
        HookLogImportant(
            "DX12: Streamline UI bootstrap overlay command list entering app submission (epoch=%llu "
            "coveredOutputs=%u)",
            static_cast<unsigned long long>(g_ActivationEpoch), g_CoveragePresentsRemaining);
    }
    return true;
}

void AfterExecuteCommandLists(ID3D12CommandQueue* queue, UINT count, ID3D12CommandList* const* commandLists) {
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    if (g_Renderer) {
        g_Renderer->CompleteSubmittedLists(queue, count, commandLists);
    }
    for (auto& renderer : g_RetiredRenderers) {
        renderer->CompleteSubmittedLists(queue, count, commandLists);
    }
    PruneRetiredRenderers();
}

bool ConsumePostSLCoverage() {
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    if (g_Phase != BootstrapPhase::kActivationSubmitted || g_CoveragePresentsRemaining == 0) {
        return false;
    }
    g_AdoptedStandbyNeedsActivationFrameRecord = false;
    --g_CoveragePresentsRemaining;
    if (g_CoveragePresentsRemaining == 0) {
        g_Phase = BootstrapPhase::kFinished;
        g_ActiveCoverage.store(false, std::memory_order_release);
        g_FrameTagTrackingActive.store(false, std::memory_order_release);
    }
    static std::atomic<int> s_logCount{0};
    const int n = s_logCount.fetch_add(1, std::memory_order_relaxed);
    if (n < 20 || (n % 120) == 0) {
        HookLogImportant(
            "DX12: PostSL output already covered by Streamline UIColorAndAlpha bootstrap overlay (epoch=%llu "
            "remaining=%u log=%d) — skipping duplicate output-backbuffer draw",
            static_cast<unsigned long long>(g_ActivationEpoch), g_CoveragePresentsRemaining, n + 1);
    }
    return true;
}

bool HasActiveCoverage() {
    return g_ActiveCoverage.load(std::memory_order_acquire);
}

void Shutdown(const char* reason) {
    std::lock_guard<std::recursive_mutex> lock(g_Mutex);
    g_PreactivationStandbyEnabled = false;
    g_Phase = BootstrapPhase::kInactive;
    g_ActivationCommandList = nullptr;
    g_CoveragePresentsRemaining = 0;
    g_AdoptedStandbyNeedsActivationFrameRecord = false;
    g_ActiveCoverage.store(false, std::memory_order_release);
    g_FrameTagTrackingActive.store(false, std::memory_order_release);
    RetireRenderer(g_Renderer);
    size_t abandoned = 0;
    for (auto& renderer : g_RetiredRenderers) {
        if (renderer && !renderer->IsGpuComplete() && renderer->DeviceHealthy()) {
            (void)renderer.release();
            ++abandoned;
        }
    }
    g_RetiredRenderers.clear();
    if (abandoned != 0) {
        HookLogImportant(
            "DX12: Abandoned %zu in-flight Streamline UI bootstrap renderer(s) during %s to avoid blocking GPU "
            "teardown",
            abandoned, reason ? reason : "shutdown");
    }
}

}  // namespace ce::dx12_streamline_ui_overlay
