#pragma once

// What a running recording does when its capture source changes under it.
//
// Once the output file is open, the encoder's frame geometry and colour
// contract are fixed: the codec, its frame pool and the file's stream header
// were created for them. Two kinds of change reach the encoder anyway:
//
//  * A size change (window resized, game switched resolution, fullscreen
//    toggled). The encoder used to only log it and keep feeding frames of the
//    new size through a converter built for the old one (cropped, stretched or
//    rejected frames). The new source is now fitted into the locked geometry,
//    aspect-preserving and centred on black, so the recording continues.
//  * A colour-contract change (HDR switched on or off, 10-bit input appearing
//    or disappearing). The encoder used to re-initialize and re-open the SAME
//    output file for writing, which truncated everything recorded before the
//    switch. There is no conversion into the other contract, so the recording
//    is finalized as saved (degraded) with everything recorded so far.
//
// Before the file is open nothing is committed, and the old re-initialization
// (which picks the new contract and size) stays correct.

#include <algorithm>
#include <cstdint>

#include "../common/cursor_capture_state.h"

namespace ce::encode_geometry {

struct Rect {
    int32_t x = 0;
    int32_t y = 0;
    int32_t width = 0;
    int32_t height = 0;
};

enum class SourceChangeAction : uint8_t {
    kNone,                 // matches the running output
    kReinitialize,         // nothing committed yet: adopt the new source
    kFitIntoLockedFrame,   // letterbox the new size into the locked geometry
    kFinalizeAndStop,      // colour contract changed after commit: keep the file, end the recording
};

inline SourceChangeAction ClassifySourceChange(bool outputCommitted, bool contractChanged, uint32_t sourceWidth,
                                               uint32_t sourceHeight, uint32_t lockedWidth, uint32_t lockedHeight) {
    if (!outputCommitted) {
        return contractChanged ? SourceChangeAction::kReinitialize : SourceChangeAction::kNone;
    }
    if (contractChanged) {
        return SourceChangeAction::kFinalizeAndStop;
    }
    if (lockedWidth == 0 || lockedHeight == 0 || sourceWidth == 0 || sourceHeight == 0) {
        return SourceChangeAction::kNone;
    }
    return (sourceWidth != lockedWidth || sourceHeight != lockedHeight) ? SourceChangeAction::kFitIntoLockedFrame
                                                                        : SourceChangeAction::kNone;
}

// Largest aspect-preserving rectangle for the source inside the frame,
// centred. Integer arithmetic only, so every pixel of the frame is either
// inside the rectangle or a black border.
inline Rect FitSourceIntoFrame(uint32_t sourceWidth, uint32_t sourceHeight, uint32_t frameWidth, uint32_t frameHeight) {
    Rect fit;
    if (sourceWidth == 0 || sourceHeight == 0 || frameWidth == 0 || frameHeight == 0) {
        return fit;
    }
    // Compare aspect ratios exactly: source wider than the frame -> full width.
    const uint64_t sourceByFrame = static_cast<uint64_t>(sourceWidth) * frameHeight;
    const uint64_t frameBySource = static_cast<uint64_t>(frameWidth) * sourceHeight;
    uint64_t width = frameWidth;
    uint64_t height = frameHeight;
    if (sourceByFrame > frameBySource) {
        height = (static_cast<uint64_t>(frameWidth) * sourceHeight + sourceWidth / 2) / sourceWidth;
    } else if (sourceByFrame < frameBySource) {
        width = (static_cast<uint64_t>(frameHeight) * sourceWidth + sourceHeight / 2) / sourceHeight;
    }
    width = std::clamp<uint64_t>(width, 1, frameWidth);
    height = std::clamp<uint64_t>(height, 1, frameHeight);
    fit.width = static_cast<int32_t>(width);
    fit.height = static_cast<int32_t>(height);
    fit.x = static_cast<int32_t>((frameWidth - width) / 2);
    fit.y = static_cast<int32_t>((frameHeight - height) / 2);
    return fit;
}

// The cursor is composited into the fitted frame, which maps the capture area
// onto the whole frame. Widen the capture area by the letterbox so the
// original capture area lands exactly on the fitted rectangle:
//   frameX = (screenX - left') * frameW / width'  ==  fit.x + (screenX - left) * fit.w / width
inline cursor::CaptureState AdjustCursorForFit(const cursor::CaptureState& state, uint32_t sourceWidth,
                                               uint32_t sourceHeight, uint32_t frameWidth, uint32_t frameHeight,
                                               const Rect& fit) {
    cursor::CaptureState adjusted = state;
    if (fit.width <= 0 || fit.height <= 0 || frameWidth == 0 || frameHeight == 0) {
        return adjusted;
    }
    const int64_t captureWidth = state.captureWidth != 0 ? state.captureWidth : sourceWidth;
    const int64_t captureHeight = state.captureHeight != 0 ? state.captureHeight : sourceHeight;
    if (captureWidth <= 0 || captureHeight <= 0) {
        return adjusted;
    }
    adjusted.captureWidth = static_cast<uint32_t>((captureWidth * frameWidth + fit.width / 2) / fit.width);
    adjusted.captureHeight = static_cast<uint32_t>((captureHeight * frameHeight + fit.height / 2) / fit.height);
    const int64_t shiftX = (static_cast<int64_t>(fit.x) * captureWidth + fit.width / 2) / fit.width;
    const int64_t shiftY = (static_cast<int64_t>(fit.y) * captureHeight + fit.height / 2) / fit.height;
    adjusted.captureLeft = static_cast<int32_t>(state.captureLeft - shiftX);
    adjusted.captureTop = static_cast<int32_t>(state.captureTop - shiftY);
    return adjusted;
}

}  // namespace ce::encode_geometry
