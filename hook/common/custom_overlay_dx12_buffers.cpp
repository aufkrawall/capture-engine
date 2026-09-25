/**
 * Custom Overlay - DX12 upload-slot and vertex/index buffer management.
 */

#include "custom_overlay_dx12.h"
#include <algorithm>
#include <cstring>
#include "../apis/dx12_hook.h"
#include "dx12_overlay_policy.h"
#include "hook_common.h"
#include "overlay_shader_bytecode.h"

namespace CustomOverlay {


bool DX12Backend::IsUploadSlotReusable(int slot) {
    if (slot >= 0 && slot < kMaxUploadSlots && inlineCompletions &&
        inlineSlots.Guard(static_cast<std::size_t>(slot)) != 0 &&
        inlineCompletions[slot] != inlineSlots.Guard(static_cast<std::size_t>(slot))) {
        static std::atomic<uint32_t> conflicts{0};
        const uint32_t count = conflicts.fetch_add(1, std::memory_order_relaxed);
        if (count < 3 || count % 600 == 0)
            HookLogImportant("DX12 Overlay: external upload slot is still owned by an inline callback draw "
                             "(count=%u)", count + 1);
        return false;
    }
    ID3D12Fence* slotFence = slotGuardBinding.GetFence();
    if (!slotFence || slot < 0 || slot >= kFramePoolSize) {
        return true;
    }

    const uint64_t guardValue = slotFenceValue[slot];
    const uint64_t completedValue = slotFence->GetCompletedValue();
    if (!ce::dx12_overlay_policy::IsOverlayUploadSlotInFlight(guardValue, completedValue)) {
        if (inFlightSkipStreak != 0) {
            HookLogImportant("DX12 Overlay: slot %d upload ring retiring again after %u in-flight draw skip(s) "
                             "(guard=%llu completed=%llu)",
                             slot, inFlightSkipStreak, (unsigned long long)guardValue,
                             (unsigned long long)completedValue);
            inFlightSkipStreak = 0;
        }
        return true;
    }

    // Never block the present thread on the slot (see IsOverlayUploadSlotInFlight):
    // the queue owing this completion may belong to an FG runtime that is not
    // retiring CE's work. Skipping the draw protects the in-flight reads just as
    // a wait would.
    ++inFlightSkipStreak;
    static std::atomic<int> s_slotInFlightLog{0};
    const int logN = s_slotInFlightLog.fetch_add(1, std::memory_order_relaxed);
    if (logN < 40 || (logN % 600) == 0) {
        HookLogImportant(
            "DX12 Overlay: slot %d still in flight (guard=%llu completed=%llu streak=%u) — no GPU-completion wait "
            "on the present thread; overlay draw skipped this frame",
            slot, (unsigned long long)guardValue, (unsigned long long)completedValue, inFlightSkipStreak);
    }
    return false;
}

bool DX12Backend::ResizeVertexBuffer(int slot, size_t requiredBytes) {
    DX12_DEBUG_STEP("ResizeVertexBuffer", "START - slot=%d, required=%zu, current=%zu", slot, requiredBytes,
                    vertexBufferSize[slot]);

    if (!device) {
        DX12_DEBUG_STEP("ResizeVertexBuffer", "FAILED - no device");
        return false;
    }

    size_t newSize =
        std::max(vertexBufferSize[slot], ce::dx12_overlay_policy::kInitialUploadSlotVertices * sizeof(DrawVertex));
    while (newSize < requiredBytes) {
        newSize *= 2;
    }
    DX12_DEBUG_STEP("ResizeVertexBuffer", "New size: %zu bytes (old=%zu, slot=%d)", newSize, vertexBufferSize[slot],
                    slot);

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = newSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> newBuffer;
    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&newBuffer));
    DX12_DEBUG_STEP("ResizeVertexBuffer", "Create result: hr=0x%08X (%s)", hr, SUCCEEDED(hr) ? "OK" : "FAILED");
    if (FAILED(hr)) {
        HookLog(
            "DX12 Overlay: ResizeVertexBuffer - Failed to create new buffer "
            "(slot=%d, size=%zu), hr=0x%08X",
            slot, newSize, hr);
        return false;
    }

    D3D12_RANGE readRange = {0, 0};
    void* newPointer = nullptr;
    hr = newBuffer->Map(0, &readRange, &newPointer);
    if (FAILED(hr) || !newPointer) {
        HookLogImportant("DX12 Overlay: ResizeVertexBuffer Map failed (slot=%d hr=0x%08X)", slot, hr);
        return false;
    }
    // Keep the old mapping usable if allocation or mapping fails. An arena-backed slot owns no resource of
    // its own; its region simply stays unused.
    if (vertexBuffer[slot] && vertexBufferPtr[slot])
        vertexBuffer[slot]->Unmap(0, nullptr);
    vertexBuffer[slot] = newBuffer;
    vertexBufferPtr[slot] = newPointer;
    vertexBufferGpu[slot] = newBuffer->GetGPUVirtualAddress();
    vertexBufferSize[slot] = newSize;
    DX12_DEBUG_STEP("ResizeVertexBuffer", "SUCCESS - new buffer[%d] mapped at %p", slot, vertexBufferPtr[slot]);


    HookLog("DX12 Overlay: Vertex buffer[%d] resized to %zu bytes", slot, newSize);
    return true;
}

bool DX12Backend::ResizeIndexBuffer(int slot, size_t requiredBytes) {
    DX12_DEBUG_STEP("ResizeIndexBuffer", "START - slot=%d, required=%zu, current=%zu", slot, requiredBytes,
                    indexBufferSize[slot]);

    if (!device) {
        DX12_DEBUG_STEP("ResizeIndexBuffer", "FAILED - no device");
        return false;
    }

    size_t newSize =
        std::max(indexBufferSize[slot], ce::dx12_overlay_policy::kInitialUploadSlotIndices * sizeof(uint16_t));
    while (newSize < requiredBytes) {
        newSize *= 2;
    }
    DX12_DEBUG_STEP("ResizeIndexBuffer", "New size: %zu bytes (old=%zu, slot=%d)", newSize, indexBufferSize[slot],
                    slot);

    // NOLINTNEXTLINE(bugprone-invalid-enum-default-initialization) - zero-initialized placeholder; enum fields are assigned before use
    D3D12_HEAP_PROPERTIES heapProps = {};
    heapProps.Type = D3D12_HEAP_TYPE_UPLOAD;

    D3D12_RESOURCE_DESC bufferDesc = {};
    bufferDesc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bufferDesc.Width = newSize;
    bufferDesc.Height = 1;
    bufferDesc.DepthOrArraySize = 1;
    bufferDesc.MipLevels = 1;
    bufferDesc.SampleDesc.Count = 1;
    bufferDesc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;

    ComPtr<ID3D12Resource> newBuffer;
    HRESULT hr = device->CreateCommittedResource(&heapProps, D3D12_HEAP_FLAG_NONE, &bufferDesc,
                                                 D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&newBuffer));
    DX12_DEBUG_STEP("ResizeIndexBuffer", "Create result: hr=0x%08X (%s)", hr, SUCCEEDED(hr) ? "OK" : "FAILED");
    if (FAILED(hr)) {
        HookLog(
            "DX12 Overlay: ResizeIndexBuffer - Failed to create new buffer "
            "(slot=%d, size=%zu), hr=0x%08X",
            slot, newSize, hr);
        return false;
    }

    D3D12_RANGE readRange = {0, 0};
    void* newPointer = nullptr;
    hr = newBuffer->Map(0, &readRange, &newPointer);
    if (FAILED(hr) || !newPointer) {
        HookLogImportant("DX12 Overlay: ResizeIndexBuffer Map failed (slot=%d hr=0x%08X)", slot, hr);
        return false;
    }
    // Keep the old mapping usable if allocation or mapping fails.
    if (indexBuffer[slot] && indexBufferPtr[slot])
        indexBuffer[slot]->Unmap(0, nullptr);
    indexBuffer[slot] = newBuffer;
    indexBufferPtr[slot] = newPointer;
    indexBufferGpu[slot] = newBuffer->GetGPUVirtualAddress();
    indexBufferSize[slot] = newSize;
    DX12_DEBUG_STEP("ResizeIndexBuffer", "SUCCESS - new buffer[%d] mapped at %p", slot, indexBufferPtr[slot]);

    HookLog("DX12 Overlay: Index buffer[%d] resized to %zu bytes", slot, newSize);
    return true;
}

}  // namespace CustomOverlay
