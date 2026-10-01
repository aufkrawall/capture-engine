#pragma once

#include <atomic>
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

// `sl1::Constants`. Unlike 2.x it has NO BaseStructure header, and it carries
// `notRenderingGameFrames`, which 2.x dropped.
//
// Every 1.x `Boolean` is `enum Boolean : char` - ONE byte (upstream v1.1.1 sl_consts.h), and
// the game's own 1.5.6 sl.common.dll validates the eight of them as
// `cmp byte ptr [rbx+0x19c..0x1a3], 2` (2 = eInvalid). An earlier mirror read them as dwords
// and declared the struct 456 bytes: `cameraMotionIncluded` then read
// `notRenderingGameFrames` (eFalse), so Streamline added camera motion to vectors that already
// carried it - DLSS sharp at rest and aliased in motion, DLSS-G interpolating the wrong motion
// (`20261001_040020`) - and the last three flags were read from past the end of the struct.
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
    uint8_t depthInverted;
    uint8_t cameraMotionIncluded;
    uint8_t motionVectors3D;
    uint8_t reset;
    uint8_t notRenderingGameFrames;  // no 2.x equivalent - dropped in translation
    uint8_t orthographicProjection;
    uint8_t motionVectorsDilated;
    uint8_t motionVectorsJittered;
    void* ext;
};
static_assert(sizeof(V1Constants) == 432, "1.x sl::Constants is 432 bytes on x64");
static_assert(offsetof(V1Constants, jitterOffset) == 320, "");
static_assert(offsetof(V1Constants, cameraNear) == 392, "");
static_assert(offsetof(V1Constants, depthInverted) == 0x19c, "sl.common 1.5.6 reads it at +0x19c");
static_assert(offsetof(V1Constants, cameraMotionIncluded) == 0x19d, "");
static_assert(offsetof(V1Constants, motionVectorsJittered) == 0x1a3, "");
static_assert(offsetof(V1Constants, ext) == 424, "");

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
    uint8_t colorBuffersHDR;  // 1-byte 1.x Boolean, like every Boolean in V1Constants
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

// `sl1::ReflexSettings` (OUT of slGetFeatureSettings(Reflex)). Upstream v1.1.1 layout, and the
// game's own 1.5.6 sl.reflex settings writer stores exactly these: `[rbx]` lowLatencyAvailable,
// `[rbx+1]` latencyReportAvailable, `[rbx+4]` statsWindowMessage, `[rbx+0x1e08]`
// flashIndicatorDriverControlled. The 64 per-frame reports in between are not written by the
// bridge. A title that is told nothing here believes Reflex is unavailable and keeps its mode
// at off, which DLSS-G then refuses (`20261001_040020`).
struct V1ReflexSettings {
    uint8_t lowLatencyAvailable;
    uint8_t latencyReportAvailable;
    uint32_t statsWindowMessage;
    uint8_t frameReport[64][120];
    uint8_t flashIndicatorDriverControlled;
    void* ext;
};
static_assert(offsetof(V1ReflexSettings, statsWindowMessage) == 4, "");
static_assert(offsetof(V1ReflexSettings, flashIndicatorDriverControlled) == 0x1e08,
              "sl.reflex 1.5.6 writes it at +0x1e08");

// `sl1::DLSSGConstants`. mode@0 measured going 0 -> 1 exactly 68 ms before
// `DLSS FG ACTIVATED` in the same session, which is what identifies it. The dword at +4 was
// constantly 1 across every capture, matching 2.x `numFramesToGenerate`'s default of 1.
// It is also the field `dlss_fg_factor` forces a cadence through, and a value transition -
// the only valid identifier - has never been observed for it; its writes are warned about
// (WarnUnconfirmedDlssgCadenceWrite below) so a wrongly-written cadence stays attributable.
struct V1DLSSGConstants {
    uint32_t mode;
    uint32_t numFramesToGenerate;
};

}  // namespace ce::streamline_bridge

#endif  // x64

// The diagnostic attached to the unconfirmed mapping above, deliberately outside the x64
// guard: its writer compiles for the 32-bit build too (Streamline has no 32-bit runtime, so
// the bridge never activates there).

// Redeclared to keep this ABI mirror self-contained: hook/common/hook_common.h declares
// this at global scope, streamline_bridge.h declares IsActive in the namespace below.
void HookLogImportant(const char* fmt, ...);

namespace ce::streamline_bridge {

bool IsActive();

// `dlss_fg_factor` forces a frame-generation cadence by writing
// `DLSSGOptions::numFramesToGenerate`, and while this bridge translates, that cadence maps
// through `V1DLSSGConstants`'s +4 field - the one piece of the bridge identified only by
// "constantly 1 across every capture" rather than by a value transition. The feature stays
// enabled; this warning is what makes a wrongly-written cadence attributable in the session
// log. Rate-limited because the override runs on every slDLSSGSetOptions call.
inline void WarnUnconfirmedDlssgCadenceWrite(uint32_t writtenValue) {
    if (!IsActive())
        return;
    static std::atomic<int> s_logCount{0};
    const int logCount = s_logCount.fetch_add(1, std::memory_order_relaxed);
    if (logCount < 4 || (logCount % 600) == 0) {
        HookLogImportant(
            "Streamline bridge: dlss_fg_factor wrote DLSSG numFramesToGenerate=%u while bridged (#%d) - the 1.x "
            "+4 field that carries it is identified by a constant 1 across captures, not a value transition",
            writtenValue, logCount + 1);
    }
}

}  // namespace ce::streamline_bridge
