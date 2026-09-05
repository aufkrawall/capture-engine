#include "custom_overlay_dx12.h"

#include <cstring>
#include "hook_common.h"

namespace CustomOverlay {

bool DX12Backend::CreateInlineCompletionBuffer() {
    // CPU-readable cached memory, matching the existing FFX final-batch
    // marker transport. A MARKER_OUT retires preceding draws on the original
    // command list; it needs no queue Signal, extra submission, or CPU wait.
    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - fields assigned below
    D3D12_HEAP_PROPERTIES heap = {};
    heap.Type = D3D12_HEAP_TYPE_CUSTOM;
    heap.CPUPageProperty = D3D12_CPU_PAGE_PROPERTY_WRITE_BACK;
    heap.MemoryPoolPreference = D3D12_MEMORY_POOL_L0;
    D3D12_RESOURCE_DESC desc = {};
    desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    desc.Width = kMaxUploadSlots * sizeof(uint32_t);
    desc.Height = 1;
    desc.DepthOrArraySize = 1;
    desc.MipLevels = 1;
    desc.SampleDesc.Count = 1;
    desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    HRESULT hr = device->CreateCommittedResource(&heap, D3D12_HEAP_FLAG_NONE, &desc,
                                                  D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                                  IID_PPV_ARGS(&inlineCompletionBuffer));
    void* mapped = nullptr;
    if (SUCCEEDED(hr))
        hr = inlineCompletionBuffer->Map(0, nullptr, &mapped);
    if (FAILED(hr) || !mapped) {
        HookLogImportant("DX12 Overlay: inline upload completion allocation failed hr=0x%08X", hr);
        return false;
    }
    inlineCompletions = static_cast<volatile uint32_t*>(mapped);
    std::memset(mapped, 0, static_cast<std::size_t>(desc.Width));
    inlineCompletionGpuVA = inlineCompletionBuffer->GetGPUVirtualAddress();
    return true;
}

int DX12Backend::AcquireInlineUploadSlot() {
    if (!inlineCompletions)
        return -1;
    const uint64_t fenceComplete = slotGuardBinding.GetFence()
                                       ? slotGuardBinding.GetFence()->GetCompletedValue() : UINT64_MAX;
    const std::size_t previousCount = inlineSlots.Count();
    const int slot = inlineSlots.FindReusable([&](std::size_t index, uint32_t guard) {
        const bool externalComplete = index >= kFramePoolSize || slotFenceValue[index] <= fenceComplete;
        return externalComplete && (guard == 0 || inlineCompletions[index] == guard);
    });
    std::atomic_thread_fence(std::memory_order_acquire);
    if (previousCount == 0 || (inlineSlots.Count() > previousCount && inlineSlots.Count() > kFramePoolSize)) {
        HookLogImportant("DX12 Overlay: callback upload pool slots=%zu completion=inline-marker "
                         "(no queue Signal, no CPU wait)", inlineSlots.Count());
    }
    if (slot < 0) {
        static std::atomic<uint32_t> exhausted{0};
        const uint32_t count = exhausted.fetch_add(1, std::memory_order_relaxed);
        if (count < 3 || count % 600 == 0)
            HookLogImportant("DX12 Overlay: all %d callback upload slots remain in flight; "
                             "refusing overwrite (count=%u)", kMaxUploadSlots, count + 1);
    }
    return slot;
}

void DX12Backend::MarkInlineUploadComplete(ID3D12GraphicsCommandList2* list, int slot) {
    D3D12_WRITEBUFFERIMMEDIATE_PARAMETER marker = {};
    marker.Dest = inlineCompletionGpuVA + static_cast<UINT64>(slot) * sizeof(uint32_t);
    marker.Value = inlineSlots.Commit(static_cast<std::size_t>(slot));
    constexpr auto mode = D3D12_WRITEBUFFERIMMEDIATE_MODE_MARKER_OUT;
    list->WriteBufferImmediate(1, &marker, &mode);
}

bool DX12Backend::HasInlineUploadsInFlight() const {
    if (!inlineCompletions)
        return false;
    for (std::size_t slot = 0; slot < inlineSlots.Count(); ++slot) {
        const uint32_t guard = inlineSlots.Guard(slot);
        if (guard != 0 && inlineCompletions[slot] != guard)
            return device && SUCCEEDED(device->GetDeviceRemovedReason());
    }
    return false;
}

}  // namespace CustomOverlay
