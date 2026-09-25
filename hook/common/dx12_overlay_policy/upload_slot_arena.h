#pragma once

#include <cstddef>

namespace ce::dx12_overlay_policy {

// Initial per-slot capacity of the overlay's upload ring. Indices are sized at three per vertex:
// the frame-time graph emits more than two indices per vertex, and the former 2:1 capacity
// (16384 bytes) overflowed on every slot at FSR FG activation in GTA, recreating 16 index buffers
// on AMD's presenter thread (session 20260925_225006, needed 16416-16584 bytes).
constexpr std::size_t kInitialUploadSlotVertices = 4096;
constexpr std::size_t kInitialUploadSlotIndices = 3 * kInitialUploadSlotVertices;

// Offsets of each slot's vertex and index region inside the one upload-heap allocation that backs
// the whole initial ring. One CreateCommittedResource instead of two per slot: 32 allocations
// took ~6 ms per renderer, on whatever thread first needed the overlay.
struct UploadSlotArenaLayout {
    std::size_t vertexBytes = 0;
    std::size_t indexBytes = 0;
    std::size_t indexOffsetInSlot = 0;
    std::size_t slotStride = 0;
    std::size_t totalBytes = 0;

    constexpr std::size_t VertexOffset(std::size_t slot) const {
        return slot * slotStride;
    }
    constexpr std::size_t IndexOffset(std::size_t slot) const {
        return slot * slotStride + indexOffsetInSlot;
    }
};

constexpr std::size_t AlignUp(std::size_t value, std::size_t alignment) {
    return alignment == 0 ? value : (value + alignment - 1) / alignment * alignment;
}

// alignment: every region starts on this boundary (D3D12 requires 4 for index buffers; 256 keeps
// each region on its own cache-line-aligned block).
constexpr UploadSlotArenaLayout MakeUploadSlotArenaLayout(std::size_t slotCount, std::size_t vertexBytes,
                                                          std::size_t indexBytes, std::size_t alignment = 256) {
    UploadSlotArenaLayout layout;
    layout.vertexBytes = vertexBytes;
    layout.indexBytes = indexBytes;
    layout.indexOffsetInSlot = AlignUp(vertexBytes, alignment);
    layout.slotStride = AlignUp(layout.indexOffsetInSlot + indexBytes, alignment);
    layout.totalBytes = layout.slotStride * slotCount;
    return layout;
}

}  // namespace ce::dx12_overlay_policy
