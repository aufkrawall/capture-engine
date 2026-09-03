#include "dx12_hook_internal.h"

#include "../common/fg_cost_probe.h"


void ShutdownDescFreeBackend(const char* reason, bool shutdownMode) {
dx12_hook_g_DescFreeBackendDevice = nullptr;
dx12_hook_g_DescFreeBackendFormat = DXGI_FORMAT_UNKNOWN;
DX12DescFreeBackend* backend = dx12_hook_g_DescFreeBackend;
const bool adapterInitialized = dx12_hook_g_D3D11On12Adapter.IsInitialized();
if (!backend && !adapterInitialized) {
    return;
}

CustomOverlay::RendererBackend* adapterBackend = adapterInitialized ? dx12_hook_g_D3D11On12Adapter.GetBackend() : nullptr;
const OverlayBackendType adapterType =
    adapterInitialized ? dx12_hook_g_D3D11On12Adapter.GetBackendType() : OverlayBackendType::None;
const bool adapterOwnsBackend = backend && adapterBackend == backend;

if (ce::dx12_overlay_policy::ShouldShutdownDescFreeBackendViaOverlayAdapter(backend != nullptr, adapterInitialized,
                                                                            adapterOwnsBackend)) {
    HookLogImportant(
        "DX12: Shutting down adapter-owned DescFree backend (reason=%s backend=%p adapterBackend=%p "
        "adapterType=%d shutdownMode=%d)",
        reason ? reason : "unknown", backend, adapterBackend, (int)adapterType, shutdownMode ? 1 : 0);
    if (shutdownMode) {
        dx12_hook_g_D3D11On12Adapter.SetShutdownMode(true);
    }
    dx12_hook_g_D3D11On12Adapter.Shutdown();
    dx12_hook_g_DescFreeBackend = nullptr;
    return;
}

if (adapterInitialized) {
    HookLogImportant(
        "DX12: Shutting down DX12 overlay adapter without tracked DescFree ownership "
        "(reason=%s backend=%p adapterBackend=%p adapterType=%d shutdownMode=%d)",
        reason ? reason : "unknown", backend, adapterBackend, (int)adapterType, shutdownMode ? 1 : 0);
    if (shutdownMode) {
        dx12_hook_g_D3D11On12Adapter.SetShutdownMode(true);
    }
    dx12_hook_g_D3D11On12Adapter.Shutdown();
}

if (backend) {
    HookLogImportant("DX12: Shutting down standalone DescFree backend (reason=%s backend=%p)",
                     reason ? reason : "unknown", backend);
    backend->Shutdown();
    delete backend;
    if (dx12_hook_g_DescFreeBackend == backend) {
        dx12_hook_g_DescFreeBackend = nullptr;
    }
}
}


// Lazily (re)builds the device-scoped descriptor-free overlay backend for the
// requested device/format pair. A live backend is reused as-is when both
// match (the warm path that closes the first-present blank after FG
// transitions); a device or format change is the only rebuild trigger.
// Returns true when a ready backend is bound to (device, format).


bool EnsureDescFreeBackendForDeviceAndFormat(ID3D12Device* dev, DXGI_FORMAT format, const char* context) {
if (!dev) {
    return dx12_hook_g_DescFreeBackend != nullptr && dx12_hook_g_D3D11On12Adapter.IsInitialized();
}
if (dx12_hook_g_DescFreeBackend && (dx12_hook_g_DescFreeBackendDevice != dev || dx12_hook_g_DescFreeBackendFormat != format)) {
    HookLogImportant("DX12: DescFree backend stale (device %p->%p fmt %d->%d) — rebuilding (%s)",
                     dx12_hook_g_DescFreeBackendDevice, dev, static_cast<int>(dx12_hook_g_DescFreeBackendFormat),
                     static_cast<int>(format), context ? context : "unknown");
    ShutdownDescFreeBackend(context);
}
if (!dx12_hook_g_DescFreeBackend) {
    auto* backend = new DX12DescFreeBackend();
    if (backend->InitDevice(dev, format)) {
        dx12_hook_g_DescFreeBackend = backend;
        dx12_hook_g_DescFreeBackendDevice = dev;
        dx12_hook_g_DescFreeBackendFormat = format;
        dx12_hook_g_D3D11On12Adapter.InitCustom(dx12_hook_g_DescFreeBackend, OverlayBackendType::DX12);
        dx12_hook_g_D3D11On12Adapter.SetLatencyDevice(dev);
        HookLogImportant("DX12: Descriptor-free overlay backend ready (%s, device=%p fmt=%d)",
                         context ? context : "unknown", dev, static_cast<int>(format));
    } else {
        delete backend;
        HookLogImportant("DX12: Descriptor-free backend init FAILED (%s, fmt=%d)", context ? context : "unknown",
                         static_cast<int>(format));
    }
}
return dx12_hook_g_DescFreeBackend != nullptr && dx12_hook_g_D3D11On12Adapter.IsInitialized();
}


void EnsureOverlayBreadcrumbBuffer(ID3D12Device* device) {
if (dx12_hook_g_OverlayBcBuffer || !device) {
    return;
}
// NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
D3D12_HEAP_PROPERTIES hp = {};
hp.Type = D3D12_HEAP_TYPE_CUSTOM;
hp.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;  // system memory, CPU-cached, GPU-writable
hp.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
D3D12_RESOURCE_DESC rd = {};
rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
rd.Width = static_cast<UINT64>(kOverlayBcSlotCount) * sizeof(uint32_t);
rd.Height = 1;
rd.DepthOrArraySize = 1;
rd.MipLevels = 1;
rd.Format = DXGI_FORMAT_UNKNOWN;
rd.SampleDesc.Count = 1;
rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
ID3D12Resource* buf = nullptr;
HRESULT hr = device->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                             IID_PPV_ARGS(&buf));
if (FAILED(hr) || !buf) {
    static std::atomic<int> s_bcCreateFailLog{0};
    if (s_bcCreateFailLog.fetch_add(1, std::memory_order_relaxed) < 3) {
        HookLogImportant("DX12: Overlay GPU breadcrumb buffer create failed hr=0x%08X", (unsigned)hr);
    }
    return;
}
void* mapped = nullptr;
if (FAILED(buf->Map(0, nullptr, &mapped)) || !mapped) {
    buf->Release();
    return;
}
memset(mapped, 0, static_cast<size_t>(rd.Width));
dx12_hook_g_OverlayBcMapped = static_cast<volatile uint32_t*>(mapped);
dx12_hook_g_OverlayBcGpuVA = buf->GetGPUVirtualAddress();
dx12_hook_g_OverlayBcBuffer = buf;
HookLogImportant("DX12: Overlay GPU breadcrumb buffer armed (slots=%d gpuVA=0x%llX)",
                 static_cast<int>(kOverlayBcSlotCount), static_cast<unsigned long long>(dx12_hook_g_OverlayBcGpuVA));
}


// Call once per overlay submit (before recording) to bump the sequence the GPU will stamp into each slot.


void BeginOverlayGpuBreadcrumbFrame(ID3D12Device* device) {
if (ce::fg_cost_probe::Active(ce::fg_cost_probe::kBreadcrumbsOff)) {
    return;
}
EnsureOverlayBreadcrumbBuffer(device);
if (dx12_hook_g_OverlayBcMapped) {
    dx12_hook_g_OverlayBcSeq.fetch_add(1, std::memory_order_relaxed);
}
}


void WriteOverlayGpuBreadcrumb(ID3D12GraphicsCommandList* list, OverlayGpuBreadcrumbOp op) {
if (ce::fg_cost_probe::Active(ce::fg_cost_probe::kBreadcrumbsOff)) {
    return;
}
if (!list || !dx12_hook_g_OverlayBcMapped || dx12_hook_g_OverlayBcGpuVA == 0 || op == 0 || op >= kOverlayBcSlotCount) {
    return;
}
ID3D12GraphicsCommandList2* list2 = nullptr;
if (FAILED(list->QueryInterface(IID_PPV_ARGS(&list2))) || !list2) {
    return;
}
D3D12_WRITEBUFFERIMMEDIATE_PARAMETER param = {};
param.Dest = dx12_hook_g_OverlayBcGpuVA + static_cast<UINT64>(op) * sizeof(uint32_t);
param.Value = dx12_hook_g_OverlayBcSeq.load(std::memory_order_relaxed);
D3D12_WRITEBUFFERIMMEDIATE_MODE mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
list2->WriteBufferImmediate(1, &param, &mode);
list2->Release();
}
