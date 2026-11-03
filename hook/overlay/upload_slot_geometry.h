#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace CustomOverlay {

// What each persistently mapped upload slot of an explicit-API overlay backend still holds. A backend writes
// a frame's geometry into the slot it draws from, and only reuses a slot once the GPU is done with it, so the
// slot's memory is intact by then: only what changed since that slot's last write needs writing. The D3D12 and
// Vulkan backends copied the whole frame on every draw, 1.5 MB of write-combined memory per frame for the
// 32-bit D3D12 text path (Mirror's Edge, 20261103_213712: 70 us per graph-only frame).
//
// The renderer reports each change, like any other backend: a whole new frame, or a frame that differs only
// behind its first vertices and indices. "Head" is the part those frames share. Slots rotate faster than the
// rows change, so most draws find a slot that holds the current head and write only the graph.
class UploadSlotGeometry {
public:
    explicit UploadSlotGeometry(size_t slotCount) : slots_(slotCount) {}

    void NoteWholeFrame() {
        ++frame_;
        ++head_;
        unchangedVertices_ = 0;
        indicesCompareFrom_ = 0;
        indicesPending_ = true;
    }
    void NoteTailFrame(uint32_t unchangedVertices, uint32_t unchangedIndices) {
        ++frame_;
        unchangedVertices_ = unchangedVertices;
        // A change no draw has seen yet still counts: its indices were never compared.
        indicesCompareFrom_ = indicesPending_ ? (std::min)(indicesCompareFrom_, unchangedIndices) : unchangedIndices;
        indicesPending_ = true;
    }

    // What a slot's index buffer has to hold for this frame. A graph line a few points shorter is the same list
    // ended earlier, so the longest list since the text last changed serves every shorter one; size index
    // buffers for it. Compares once per change, and only the part that can differ.
    const std::vector<uint16_t>& IndicesToHold(const std::vector<uint16_t>& indices) {
        if (indicesPending_) {
            indicesPending_ = false;
            const size_t from = (std::min)({static_cast<size_t>(indicesCompareFrom_), indices.size(), held_.size()});
            const bool same = indices.size() <= held_.size() &&
                              std::equal(indices.begin() + static_cast<std::ptrdiff_t>(from), indices.end(),
                                         held_.begin() + static_cast<std::ptrdiff_t>(from));
            if (!same) {
                held_ = indices;
                ++indexList_;
            }
        }
        return held_;
    }

    struct Plan {
        bool writeVertices = true;
        size_t firstVertex = 0;
        bool writeIndices = true;
    };
    Plan PlanSlot(size_t slot, size_t vertexCount) const {
        const Slot& state = slots_[slot];
        Plan plan;
        plan.writeVertices = state.frame != frame_;
        plan.firstVertex = state.head == head_ ? (std::min)(static_cast<size_t>(unchangedVertices_), vertexCount) : 0;
        plan.writeIndices = state.indexList != indexList_;
        return plan;
    }
    void MarkWritten(size_t slot) {
        slots_[slot] = Slot{frame_, head_, indexList_};
    }

    // The slot's memory was replaced.
    void InvalidateSlot(size_t slot) {
        slots_[slot] = Slot{};
    }
    void InvalidateAll() {
        std::fill(slots_.begin(), slots_.end(), Slot{});
    }

    // Bytes written per frame, for the backend's rate-limited log line.
    struct Totals {
        uint32_t frames = 0;
        uint64_t vertexBytes = 0;
        uint64_t indexBytes = 0;
        uint64_t frameBytes = 0;  // what writing everything would have cost
    };
    static constexpr uint32_t kReportFrames = 600;
    bool NoteBytes(uint64_t vertexBytes, uint64_t indexBytes, uint64_t frameBytes, Totals* report) {
        ++totals_.frames;
        totals_.vertexBytes += vertexBytes;
        totals_.indexBytes += indexBytes;
        totals_.frameBytes += frameBytes;
        if (totals_.frames < kReportFrames)
            return false;
        *report = totals_;
        totals_ = {};
        return true;
    }

private:
    // Zero is no generation: the counters start at 1, so a fresh slot holds nothing.
    struct Slot {
        uint64_t frame = 0;
        uint64_t head = 0;
        uint64_t indexList = 0;
    };

    std::vector<Slot> slots_;
    std::vector<uint16_t> held_;
    uint64_t frame_ = 1;
    uint64_t head_ = 1;
    uint64_t indexList_ = 1;
    uint32_t unchangedVertices_ = 0;
    uint32_t indicesCompareFrom_ = 0;
    bool indicesPending_ = true;
    Totals totals_;
};

}  // namespace CustomOverlay
