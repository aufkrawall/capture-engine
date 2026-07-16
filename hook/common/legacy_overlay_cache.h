#pragma once

#include <cmath>

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
