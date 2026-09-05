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


bool DX12Backend::WaitForSlotGpuComplete(int slot) {
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
    const uint64_t completedBefore = slotFence->GetCompletedValue();
    if (!ce::dx12_overlay_policy::ShouldWaitForOverlayUploadSlot(guardValue, completedBefore)) {
        return true;
    }

    HANDLE eventHandle = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!eventHandle) {
        return false;
    }

    bool completed = false;
    if (SUCCEEDED(slotFence->SetEventOnCompletion(guardValue, eventHandle))) {
        constexpr DWORD kSlotWaitTimeoutMs = 1000;
        const DWORD waitResult = WaitForSingleObject(eventHandle, kSlotWaitTimeoutMs);
        completed = waitResult == WAIT_OBJECT_0;
        if (completed) {
            static std::atomic<int> s_slotWaitCompleteLog{0};
            const int logN = s_slotWaitCompleteLog.fetch_add(1, std::memory_order_relaxed);
            if (logN < 20 || (logN % 200) == 0) {
                HookLogImportant(
                    "DX12 Overlay: slot %d GPU-completion wait completed (guard=%llu completedBefore=%llu "
                    "completedAfter=%llu)",
                    slot, (unsigned long long)guardValue, (unsigned long long)completedBefore,
                    (unsigned long long)slotFence->GetCompletedValue());
            }
        } else {
            static std::atomic<int> s_slotWaitTimeoutLog{0};
            const int logN = s_slotWaitTimeoutLog.fetch_add(1, std::memory_order_relaxed);
            if (logN < 40 || (logN % 200) == 0) {
                HookLogImportant(
                    "DX12 Overlay: slot %d GPU-completion wait %s (guard=%llu completed=%llu) — upload ring may be "
                    "draw skipped to avoid reusing in-flight GPU data",
                    slot, waitResult == WAIT_TIMEOUT ? "timed out" : "failed", (unsigned long long)guardValue,
                    (unsigned long long)slotFence->GetCompletedValue());
            }
        }
    }

    CloseHandle(eventHandle);
    return completed;
}

bool DX12Backend::ResizeVertexBuffer(int slot, size_t requiredBytes) {
    DX12_DEBUG_STEP("ResizeVertexBuffer", "START - slot=%d, required=%zu, current=%zu", slot, requiredBytes,
                    vertexBufferSize[slot]);

    if (!device) {
        DX12_DEBUG_STEP("ResizeVertexBuffer", "FAILED - no device");
        return false;
    }

    size_t newSize = std::max(vertexBufferSize[slot], 4096 * sizeof(DrawVertex));
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
    // Keep the old mapping usable if allocation or mapping fails.
    if (vertexBuffer[slot] && vertexBufferPtr[slot])
        vertexBuffer[slot]->Unmap(0, nullptr);
    vertexBuffer[slot] = newBuffer;
    vertexBufferPtr[slot] = newPointer;
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

    size_t newSize = std::max(indexBufferSize[slot], 8192 * sizeof(uint16_t));
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
    indexBufferSize[slot] = newSize;
    DX12_DEBUG_STEP("ResizeIndexBuffer", "SUCCESS - new buffer[%d] mapped at %p", slot, indexBufferPtr[slot]);

    HookLog("DX12 Overlay: Index buffer[%d] resized to %zu bytes", slot, newSize);
    return true;
}

}  // namespace CustomOverlay
