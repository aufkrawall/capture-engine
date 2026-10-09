#include "dx12_hook_internal.h"

#include "hook/fg/fg_cost_probe.h"
#include "hook/streamline/streamline_bridge.h"
#include "common/logging/log_meter.h"

DX12RetiredQueueBinding DX12_AdoptCommandQueue(ID3D12CommandQueue* queue, bool fromExecuteCommandLists) {
    DX12RetiredQueueBinding retired;
    if (!queue || ce::fg_cost_probe::Active(ce::fg_cost_probe::kQueueAdoptionOff))
        return retired;

    ce::ComGuard<ID3D12Device> incomingDevice;
    std::lock_guard<std::recursive_mutex> lock(g_CommandQueueMutex);
    ID3D12CommandQueue* current = g_CommandQueue.load(std::memory_order_acquire);
    if (current == queue)
        return retired;
    // ECL cannot revoke ownership by exposing a different native/interposer device view.
    if (!ce::dx12_overlay_policy::ShouldAdoptDiscoveredCommandQueue(
            fromExecuteCommandLists, current != nullptr, true)) {
        static ce::log_meter::ChangeGate retainedGate;
        const auto verdict = retainedGate.Observe(ce::log_meter::FieldKey(current));
        if (verdict) {
            HookLogImportant("[DX12QueueBinding] discovery=retained reason=established-binding "
                             "selected=%p incoming=%p device-query=skipped%s", current, queue,
                             ce::log_meter::SuppressedNote(verdict.suppressed).c_str());
        }
        return retired;
    }

    // The caller owns the incoming queue for the call. Resolve its device before publishing either
    // half of the pair, and keep old references alive until the replacement has been published.
    ID3D12Device* device = nullptr;
    const HRESULT hr = queue->GetDevice(IID_PPV_ARGS(incomingDevice.addressof()));
    device = incomingDevice.get();
    if (FAILED(hr) || !device) {
        static std::atomic<uint32_t> failures{0};
        if (failures.fetch_add(1, std::memory_order_relaxed) < 10)
            HookLogImportant("DX12: Queue adoption refused: GetDevice failed queue=%p hr=0x%08X", queue, hr);
        return retired;
    }

    queue->AddRef();
    retired.queue.attach(g_CommandQueue.exchange(queue, std::memory_order_acq_rel));
    const bool deviceChanged = g_Device.load(std::memory_order_acquire) != device;
    if (!current || deviceChanged)
        dx12_hook_g_PrimaryGameQueue.store(queue, std::memory_order_release);
    static std::atomic<uint32_t> adoptions{0};
    const uint32_t adoptionCount = adoptions.fetch_add(1, std::memory_order_relaxed) + 1;
    if (adoptionCount <= 16 || (adoptionCount != 0 && (adoptionCount & (adoptionCount - 1)) == 0))
        HookLogImportant("DX12: Adopted queue=%p previous=%p device=%p source=%s sameDevice=%d deviceChanged=%d count=%u",
                     queue, retired.queue.get(), device, fromExecuteCommandLists ? "device-discovery" : "explicit-binding",
                     deviceChanged ? 0 : 1, deviceChanged ? 1 : 0, adoptionCount);

    if (ce::fg_cost_probe::Active(ce::fg_cost_probe::kQueueDevicePublishOff))
        return retired;
    DX12_PublishNativeLimiterDevice(device, queue, "command queue");
    ce::streamline_bridge::NotifyD3D12Device(device);
    if (deviceChanged) {
        retired.device.attach(g_Device.exchange(incomingDevice.release(), std::memory_order_acq_rel));
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
    return retired;
}
