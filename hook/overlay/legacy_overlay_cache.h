#pragma once

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
        dirty = true;
    }
    void MarkBufferRecreated() {
        dirty = true;
    }
    void MarkUploadSucceeded() {
        dirty = false;
    }
    void MarkUploadFailed() {
        dirty = true;
    }
    bool NeedsUpload() const {
        return dirty;
    }

private:
    bool dirty = true;
};

// Where a frame's vertices go in a dynamic vertex buffer: behind the previous
// frame's, locked with D3DLOCK_NOOVERWRITE, while they fit, and the whole
// buffer discarded (D3DLOCK_DISCARD) only when they do not. A discard has the
// driver hand out fresh memory on the game's thread; an append only promises
// not to touch what the GPU may still be reading. A frame draws from its slot's
// base vertex, so its indices do not depend on where it landed.
class LegacyVertexRing {
public:
    struct Slot {
        size_t offset = 0;
        bool discard = true;
    };
    Slot Next(size_t bytes, size_t capacity) {
        if (!valid || cursor + bytes > capacity) {
            valid = true;
            cursor = bytes;
            return {0, true};
        }
        const Slot slot{cursor, false};
        cursor += bytes;
        return slot;
    }
    // The buffer was recreated, or a lock failed: the next frame discards.
    void Invalidate() {
        valid = false;
    }

private:
    size_t cursor = 0;
    bool valid = false;
};

// The overlay's index list changes only when its text does; a frame that only
// moved the graph's vertices draws with the indices already in the buffer.
class LegacyIndexUploadState {
public:
    bool NeedsUpload(const std::vector<uint16_t>& indices) const {
        return !valid || indices != uploaded;
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
