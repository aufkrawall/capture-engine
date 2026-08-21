#pragma once

#include <cstddef>
#include <cstdint>

// The Streamline **1.x** structures the generation bridge has to read, mirrored.
//
// Separated from the translation itself because it is a different kind of claim. Everything in
// `streamline_bridge_translate.cpp` is code that calls a documented 2.x API; everything here is
// an assertion about an ABI NVIDIA never published - there is no public Streamline 1.5.6
// header, no 1.x release at all, and the upstream 1.x tags stop at v1.1.1, which predates
// DLSS-G. Each type below therefore records where it came from, and nothing is inferred.
//
// x64 only: the layouts are pointer-size dependent and their assertions are x64 facts, which is
// how the 32-bit build caught an earlier version rather than silently mis-laying them out.
// Streamline has no 32-bit runtime, so the bridge never activates there.
#if defined(_M_X64) || defined(__x86_64__)

namespace ce::streamline_bridge {

// ---------------------------------------------------------------------------
// The 1.x side, mirrored
// ---------------------------------------------------------------------------
//
// No public 1.5.6 header exists, so these are mirrors. Each one is either corroborated by
// two independent header sources or measured from a real session - never inferred.

// `sl1::Constants`. Identical in upstream v1.1.1 and OptiScaler's vendored SL1 set, which
// is what makes it trustworthy across the 1.x line. Unlike 2.x it has NO BaseStructure
// header, and it carries `notRenderingGameFrames`, which 2.x dropped.
struct V1Float2 {
    float x, y;
};
struct V1Float3 {
    float x, y, z;
};
struct V1Float4 {
    float x, y, z, w;
};
struct V1Float4x4 {
    V1Float4 row[4];
};

struct V1Constants {
    V1Float4x4 cameraViewToClip;
    V1Float4x4 clipToCameraView;
    V1Float4x4 clipToLensClip;
    V1Float4x4 clipToPrevClip;
    V1Float4x4 prevClipToClip;
    V1Float2 jitterOffset;
    V1Float2 mvecScale;
    V1Float2 cameraPinholeOffset;
    V1Float3 cameraPos;
    V1Float3 cameraUp;
    V1Float3 cameraRight;
    V1Float3 cameraFwd;
    float cameraNear;
    float cameraFar;
    float cameraFOV;
    float cameraAspectRatio;
    float motionVectorsInvalidValue;
    uint32_t depthInverted;
    uint32_t cameraMotionIncluded;
    uint32_t motionVectors3D;
    uint32_t reset;
    uint32_t notRenderingGameFrames;  // no 2.x equivalent - dropped in translation
    uint32_t orthographicProjection;
    uint32_t motionVectorsDilated;
    uint32_t motionVectorsJittered;
    void* ext;
};
static_assert(sizeof(V1Constants) == 456, "1.x sl::Constants is 456 bytes on x64");
static_assert(offsetof(V1Constants, jitterOffset) == 320, "");
static_assert(offsetof(V1Constants, cameraNear) == 392, "");
static_assert(offsetof(V1Constants, depthInverted) == 412, "");

// `sl1::Resource`. Measured layout already encoded in streamline_api_generation.h and
// independently confirmed by OptiScaler's header; note `type` is a 1-byte enum, not a dword.
struct V1Resource {
    uint8_t type;
    void* native;
    void* memory;
    void* view;
    uint32_t state;
    void* ext;
};
static_assert(offsetof(V1Resource, native) == 8, "");
static_assert(offsetof(V1Resource, state) == 32, "");

// `sl1::Extent`. Same shape as 2.x; only ever consumed when the game supplies one.
struct V1Extent {
    uint32_t top, left, width, height;
};

// `sl1::DLSSConstants`, measured from The Witcher 3 session `20260821_042540`: mode@0
// (1 and 4 both observed), outputWidth@4 (3840), outputHeight@8 (2160), sharpness@12 (0.0),
// preExposure@16 (1.0), exposureScale@20 (1.0), colorBuffersHDR@24 (1). The same leading
// run of fields as 2.x `DLSSOptions`, which is why this translates almost verbatim.
struct V1DLSSConstants {
    uint32_t mode;
    uint32_t outputWidth;
    uint32_t outputHeight;
    float sharpness;
    float preExposure;
    float exposureScale;
    uint32_t colorBuffersHDR;
};

// `sl1::DLSSSettings`. Only the first three fields are written back: that is exactly what
// the game's own 1.5.6 runtime filled in the measured capture (1920, 1080, 0.35, then
// zeroes), so replicating more would be inventing behaviour the real runtime did not have.
struct V1DLSSSettings {
    uint32_t optimalRenderWidth;
    uint32_t optimalRenderHeight;
    float optimalSharpness;
};

// `sl1::ReflexConstants`, re-measured from The Witcher 3 session `20260821_042540`.
//
// Only `mode` is real. Every capture has `mode`@0 = 1 and +4 = 0, and everything from +8 on is
// stack leftovers rather than struct: the captures disagree there, and the disagreeing value
// reads as `00 46 00 00 f6 7f 00 00` - a 0x00007ff6.... module address straddling +8 and +12,
// which is a caller's saved pointer, not data. An earlier reading of this same probe recorded
// "frameLimitUs@12" from that tail; it is bytes past the end of an 8-byte struct.
//
// So the translation carries `mode` and nothing else, leaving 2.x's `frameLimitUs`,
// `useMarkersToOptimize`, `virtualKey` and `idThread` at their defaults. That is the whole
// point of Reflex here anyway: DLSS-G will not engage with Reflex off, and turning it on is
// what the game is asking for.
struct V1ReflexConstants {
    uint32_t mode;
};

// `sl1::DLSSGConstants`. mode@0 measured going 0 -> 1 exactly 68 ms before
// `DLSS FG ACTIVATED` in the same session, which is what identifies it. The dword at +4 was
// constantly 1 across every capture, matching 2.x `numFramesToGenerate`'s default of 1.
struct V1DLSSGConstants {
    uint32_t mode;
    uint32_t numFramesToGenerate;
};

}  // namespace ce::streamline_bridge

#endif  // x64
