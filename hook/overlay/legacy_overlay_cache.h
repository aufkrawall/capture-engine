#pragma once

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace CustomOverlay {

// Small backend-local state machines kept independent of graphics API objects so
// cache invalidation can be regression-tested without creating a legacy device.
class LegacyGeometryUploadState {
public:
    void MarkDrawDataChanged() {
        MarkWholeFrameDirty();
    }
    // Only what follows these first vertices and indices differs from the frame last uploaded.
    void MarkTailChanged(uint32_t vertices, uint32_t indices) {
        dirty = true;
        unchangedVertices = vertices;
        unchangedIndices = indices;
    }
    void MarkBufferRecreated() {
        MarkWholeFrameDirty();
    }
    void MarkUploadSucceeded() {
        dirty = false;
        wholeFrameDirty = false;
        unchangedVertices = 0;
        unchangedIndices = 0;
    }
    void MarkUploadFailed() {
        MarkWholeFrameDirty();
    }
    bool NeedsUpload() const {
        return dirty;
    }
    // How much of the pending frame the buffers already hold: nothing unless the frame before it
    // reached them, because the renderer's "unchanged" is relative to the frame it last built.
    uint32_t UnchangedVertices() const {
        return wholeFrameDirty ? 0 : unchangedVertices;
    }
    uint32_t UnchangedIndices() const {
        return wholeFrameDirty ? 0 : unchangedIndices;
    }

private:
    void MarkWholeFrameDirty() {
        dirty = true;
        wholeFrameDirty = true;
    }

    bool dirty = true;
    bool wholeFrameDirty = true;
    uint32_t unchangedVertices = 0;
    uint32_t unchangedIndices = 0;
};

// Where a frame's vertices go in a dynamic vertex buffer: behind the previous
// frame's, locked with D3DLOCK_NOOVERWRITE, while they fit, and the whole
// buffer discarded (D3DLOCK_DISCARD) only when they do not. A discard has the
// driver hand out fresh memory on the game's thread; an append only promises
// not to touch what the GPU may still be reading. A frame draws from its slot's
// base vertex, so its indices do not depend on where it landed.
//
// A frame whose first bytes equal the last whole frame's writes only the rest: appending leaves those
// bytes in place until the next discard, which is why a frame that would discard is written whole.
class LegacyVertexRing {
public:
    struct Slot {
        size_t offset = 0;
        bool discard = true;
        // The frame's first keptBytes are not written: they stay at keptOffset.
        size_t keptBytes = 0;
        size_t keptOffset = 0;
    };
    Slot Next(size_t bytes, size_t capacity, size_t unchangedBytes = 0) {
        if (valid && unchangedBytes > 0 && unchangedBytes < bytes && unchangedBytes <= wholeFrameBytes &&
            cursor + (bytes - unchangedBytes) <= capacity) {
            const Slot slot{cursor, false, unchangedBytes, wholeFrameOffset};
            cursor += bytes - unchangedBytes;
            return slot;
        }
        wholeFrameBytes = bytes;
        if (!valid || cursor + bytes > capacity) {
            valid = true;
            cursor = bytes;
            wholeFrameOffset = 0;
            return {0, true, 0, 0};
        }
        const Slot slot{cursor, false, 0, 0};
        wholeFrameOffset = cursor;
        cursor += bytes;
        return slot;
    }
    // The buffer was recreated, or a lock failed: the next frame discards.
    void Invalidate() {
        valid = false;
    }

private:
    size_t cursor = 0;
    size_t wholeFrameOffset = 0;
    size_t wholeFrameBytes = 0;
    bool valid = false;
};

// Where an uploaded frame's vertices are: one run, or after a tail-only upload two, the part kept from
// an earlier frame and the new tail. Indices count from the frame's first vertex, so each run needs its
// own base vertex, and a command that crosses from one run into the other takes a draw per run.
struct LegacyFrameLayout {
    uint32_t headBaseVertex = 0;
    uint32_t tailBaseVertex = 0;
    uint32_t tailFirstVertex = 0;
    uint32_t tailFirstIndex = 0;

    static LegacyFrameLayout For(const LegacyVertexRing::Slot& slot, size_t stride, uint32_t vertexCount,
                                 uint32_t indexCount, uint32_t unchangedIndices) {
        const uint32_t writtenAt = static_cast<uint32_t>(slot.offset / stride);
        if (slot.keptBytes == 0)
            return {writtenAt, writtenAt, vertexCount, indexCount};
        // The tail is behind the kept vertices, so this does not underflow.
        const uint32_t keptVertices = static_cast<uint32_t>(slot.keptBytes / stride);
        return {static_cast<uint32_t>(slot.keptOffset / stride), writtenAt - keptVertices, keptVertices,
                unchangedIndices};
    }

    // draw(baseVertex, firstVertex, vertexCount, firstIndex, indexCount), once per run the command reads.
    template <typename Draw>
    void ForEachRun(uint32_t indexOffset, uint32_t indexCount, uint32_t vertexCount, Draw&& draw) const {
        const uint32_t indexEnd = indexOffset + indexCount;
        if (indexOffset < tailFirstIndex) {
            draw(headBaseVertex, 0u, tailFirstVertex, indexOffset, (std::min)(indexEnd, tailFirstIndex) - indexOffset);
        }
        if (indexEnd > tailFirstIndex) {
            const uint32_t first = (std::max)(indexOffset, tailFirstIndex);
            draw(tailBaseVertex, tailFirstVertex, vertexCount - tailFirstVertex, first, indexEnd - first);
        }
    }
};

// The overlay's index list changes only when its text does; a frame that only
// moved the graph's vertices draws with the indices already in the buffer. A
// graph line one section shorter is the same list ended earlier, so what the
// buffer holds also serves any list that starts it.
class LegacyIndexUploadState {
public:
    bool NeedsUpload(const std::vector<uint16_t>& indices) const {
        return !valid || indices.size() > uploaded.size() ||
               !std::equal(indices.begin(), indices.end(), uploaded.begin());
    }
    void MarkUploaded(const std::vector<uint16_t>& indices) {
        uploaded = indices;
        valid = true;
    }
    void Invalidate() {
        valid = false;
    }

private:
    std::vector<uint16_t> uploaded;
    bool valid = false;
};

class DX10ConstantBufferState {
public:
    bool NeedsUpdate(int viewportWidth, int viewportHeight, int hdrMode, float paperWhiteNits) const {
        return !valid || width != viewportWidth || height != viewportHeight || mode != hdrMode ||
               std::fabs(paperWhite - paperWhiteNits) > 0.001f;
    }
    void MarkUpdated(int viewportWidth, int viewportHeight, int hdrMode, float paperWhiteNits) {
        width = viewportWidth;
        height = viewportHeight;
        mode = hdrMode;
        paperWhite = paperWhiteNits;
        valid = true;
    }
    void Invalidate() {
        valid = false;
    }

private:
    int width = 0;
    int height = 0;
    int mode = 0;
    float paperWhite = 0.0f;
    bool valid = false;
};

enum class LegacyGLDrawPath { Arrays, Immediate };

inline LegacyGLDrawPath SelectLegacyGLDrawPath(bool matrixPathValid, bool arrayFunctionsAvailable,
                                               bool arrayProbeSucceeded) {
    return matrixPathValid && arrayFunctionsAvailable && arrayProbeSucceeded ? LegacyGLDrawPath::Arrays
                                                                             : LegacyGLDrawPath::Immediate;
}

}  // namespace CustomOverlay
