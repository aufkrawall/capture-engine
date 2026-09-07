#include "dx12_hook_internal.h"

#include "../common/fg_cost_probe.h"
#include "streamline_bridge.h"

void DX12_AdoptCommandQueue(ID3D12CommandQueue* queue, bool fromExecuteCommandLists) {
    if (!queue || ce::fg_cost_probe::Active(ce::fg_cost_probe::kQueueAdoptionOff))
        return;

    // The caller owns the incoming queue for the call. Resolve its device before publishing either
    // half of the pair, and keep old references alive until the replacement has been published.
    ID3D12Device* device = nullptr;
    const HRESULT hr = queue->GetDevice(IID_PPV_ARGS(&device));
    if (FAILED(hr) || !device) {
        static std::atomic<uint32_t> failures{0};
        if (failures.fetch_add(1, std::memory_order_relaxed) < 10)
            HookLogImportant("DX12: Queue adoption refused: GetDevice failed queue=%p hr=0x%08X", queue, hr);
        return;
    }
    auto releaseDevice = ce::make_scope_guard([&]() { if (device) device->Release(); });
    ID3D12CommandQueue* oldQueue = nullptr;
    ID3D12Device* oldDevice = nullptr;
    auto releaseOld = ce::make_scope_guard([&]() {
        if (oldDevice) oldDevice->Release();
        if (oldQueue) oldQueue->Release();
    });

    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    ID3D12CommandQueue* current = g_CommandQueue.load(std::memory_order_acquire);
    if (current == queue)
        return;
    ID3D12Device* currentDevice = nullptr;
    HRESULT currentDeviceHr = S_OK;
    if (current)
        currentDeviceHr = current->GetDevice(IID_PPV_ARGS(&currentDevice));
    const bool sameDevice = currentDevice && currentDevice == device;
    const bool adopt = ce::dx12_overlay_policy::ShouldAdoptDiscoveredCommandQueue(
        fromExecuteCommandLists, current != nullptr, true, currentDevice != nullptr, sameDevice);
    if (currentDevice)
        currentDevice->Release();
    if (!adopt) {
        static std::atomic<uint32_t> retained{0};
        const uint32_t count = retained.fetch_add(1, std::memory_order_relaxed) + 1;
        if (count <= 8 || (count != 0 && (count & (count - 1)) == 0))
            HookLogImportant("DX12: Execution discovery retained queue=%p instead of auxiliary=%p "
                             "device=%p reason=%s currentDeviceHr=0x%08X count=%u", current, queue, device,
                             sameDevice ? "same-device-discovery" : "current-device-unresolved", currentDeviceHr, count);
        return;
    }

    queue->AddRef();
    oldQueue = g_CommandQueue.exchange(queue, std::memory_order_acq_rel);
    const bool deviceChanged = g_Device.load(std::memory_order_acquire) != device;
    if (!current || !sameDevice)
        dx12_hook_g_PrimaryGameQueue.store(queue, std::memory_order_release);
    static std::atomic<uint32_t> adoptions{0};
    const uint32_t adoptionCount = adoptions.fetch_add(1, std::memory_order_relaxed) + 1;
    if (adoptionCount <= 16 || (adoptionCount != 0 && (adoptionCount & (adoptionCount - 1)) == 0))
        HookLogImportant("DX12: Adopted queue=%p previous=%p device=%p source=%s sameDevice=%d deviceChanged=%d count=%u",
                     queue, oldQueue, device, fromExecuteCommandLists ? "device-discovery" : "explicit-binding",
                     sameDevice ? 1 : 0, deviceChanged ? 1 : 0, adoptionCount);

    if (ce::fg_cost_probe::Active(ce::fg_cost_probe::kQueueDevicePublishOff))
        return;
    DX12_PublishNativeLimiterDevice(device, queue, "command queue");
    ce::streamline_bridge::NotifyD3D12Device(device);
    if (deviceChanged) {
        oldDevice = g_Device.exchange(device, std::memory_order_acq_rel);
        device = nullptr;  // the global owns the GetDevice reference
        dx12_hook_g_DeviceRemoved.store(false, std::memory_order_release);
        DXGIShared::g_SharedState.deviceRemovedFatal.store(false, std::memory_order_release);
        g_RenderWatchdog.SetForceMonitor(false);
        dx12_hook_g_LastSuccessfulPostSLSwapchain.store(nullptr, std::memory_order_release);
        dx12_hook_g_LastSwapchainQueueCaptureSwapchain.store(nullptr, std::memory_order_release);
        dx12_hook_g_LastProvenOriginalQueueSwapchain.store(nullptr, std::memory_order_release);
        const LUID luid = g_Device.load(std::memory_order_acquire)->GetAdapterLuid();
        ReportLUID(luid.LowPart, luid.HighPart);
        HookLog("DX12: Reported LUID %08x-%08x", luid.HighPart, luid.LowPart);
    }
}
