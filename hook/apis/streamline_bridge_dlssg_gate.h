#pragma once

#include <cstdint>

// 1.x DLSS-G gates interpolation per present on `sl::Constants::notRenderingGameFrames`; 2.x has
// no such field and generates whenever its mode is on. Pure policy, unit-tested in
// `tests/test_streamline_bridge_dlssg_gate.cpp`.
//
// Measured from The Witcher 3's own 1.5.6 `sl.dlss_g.dll` (`presentCommon`, function 0x19f20):
// r15d is zeroed at +0x1a064, then `cmp byte ptr [rax+0x1a0], r15b; cmove edx, ecx` at +0x1ab8b
// keeps the interpolate flag only when the frame's constants carry eFalse at offset 0x1a0. eTrue
// and eInvalid both suppress (eInvalid also logs "'sl::Constants::notRenderingGameFrames' flag
// cannot be left as invalid"). Both generations pass a constant 0 for the NGX parameter
// `DLSSG.NotRenderingGameFrames`, so the gate lives in the plugin, never in NGX - which is why
// dropping the field in translation let 2.x interpolate frames the title had marked as non-game.
namespace ce::streamline_bridge {

// 1.5.6 interpolates only when the flag is exactly eFalse.
constexpr bool V1FrameIsNotGameFrame(uint8_t notRenderingGameFrames) { return notRenderingGameFrames != 0; }

// What the title has asked for on one viewport, from two different 1.x calls.
struct DlssgViewportRequest {
    bool haveGameRequest = false;          // set once the title sent DLSS-G constants for the viewport
    bool gameRequestsOn = false;           // 1.x DLSSGConstants.mode != 0
    uint32_t numFramesToGenerate = 1;      // 1.x DLSSGConstants +4
    bool notRenderingGameFrames = false;   // last 1.x Constants flag on this viewport
};

// What the bridge forwards to 2.x `slDLSSGSetOptions`.
struct DlssgForwardedOptions {
    bool on = false;
    uint32_t numFramesToGenerate = 1;
    // Set while the TITLE wants FG on, so a non-game stretch suspends generation without
    // releasing DLSS-G's resources: 1.x kept its feature alive through those frames as well,
    // and re-creating it costs a resource rebuild plus a history restart on the first game frame.
    bool retainResourcesWhenOff = false;

    constexpr bool operator==(const DlssgForwardedOptions& other) const {
        return on == other.on && numFramesToGenerate == other.numFramesToGenerate &&
               retainResourcesWhenOff == other.retainResourcesWhenOff;
    }
    constexpr bool operator!=(const DlssgForwardedOptions& other) const { return !(*this == other); }
};

constexpr DlssgForwardedOptions DlssgOptionsFor(const DlssgViewportRequest& request) {
    DlssgForwardedOptions options;
    options.on = request.gameRequestsOn && !request.notRenderingGameFrames;
    options.numFramesToGenerate = request.numFramesToGenerate ? request.numFramesToGenerate : 1;
    options.retainResourcesWhenOff = request.gameRequestsOn;
    return options;
}

// 1.x tags persist until the title replaces them; 1.5.6 sl.common has no expiry at all. 2.x
// legacy (frame-less) global tags expire once the present counter is more than one past the
// tagging present (`commonEntry.cpp` getTag: "Invalidating the hanging tag"), whatever their
// lifecycle. The Witcher 3 occasionally presents twice without re-tagging (16 times in session
// 20261001_092557); 2.x then lost depth and motion vectors, switched interpolation off for that
// present and back on for the next - a visible dark flash under 4x MFG. Re-issuing the title's
// last DLSS-G input tags after each present restores the 1.x lifetime.
inline constexpr uint32_t kV1ReflexMarkerPresentEnd = 5;  // 1.x marker 5 == 2.x PCLMarker::ePresentEnd

// The tags 2.x DLSS-G reads at present: depth, motion vectors, HUD-less color, UI color+alpha.
// Upscaler inputs/outputs are evaluate-time tags the title re-sets every frame anyway.
constexpr bool V1TagPersistsAcrossPresents(uint32_t bufferType) {
    return bufferType == 0 || bufferType == 1 || bufferType == 2 || bufferType == 23;
}

// 2.x DLSS-G checks Reflex at every present: sl.reflex records `presentCount + 1` only on a
// PRESENT_START marker (`reflexEntry.cpp`, kCurrentFrame), and sl.common bumps `presentCount` on
// every non-test present. A present without its own PRESENT_START therefore fails the check
// ("eDLSSGStatusFailReflexNotDetectedAtRuntime ... N != N+1") and DLSS-G skips that present -
// the dark flash that remained in session 20261001_093949 once the tags outlived the extra present
// (three failures, each exactly at a second present burst ~5 ms after a normal one). 1.x had no
// such check. The bridge re-marks the title's last presented frame for a present it left unmarked.
inline constexpr uint32_t kV1ReflexMarkerPresentStart = 4;  // 1.x marker 4 == 2.x PCLMarker::ePresentStart
inline constexpr uint32_t kDxgiPresentTest = 0x1;           // DXGI_PRESENT_TEST; sl.common skips those

// What the title sent the bridge between two presents 2.x counted. Re-marking keeps an unmarked
// present from being skipped, but session 20261001_105517 still showed dark flashes exactly at
// those presents (now generated as a full 4x group). Whether such a present re-shows the last
// frame or carries a new frame the title left unmarked decides which frame it must be attributed
// to, and only this activity tells the two apart: a re-present brings no constants, tags or
// upscaler evaluate; a new frame brings constants for a newer frame index.
struct TitlePresentActivity {
    uint32_t constants = 0;
    uint32_t lastConstantsFrame = 0;
    uint32_t tags = 0;
    uint32_t evaluates = 0;  // feature evaluates other than Reflex markers and sleep
    uint32_t lastEvaluateFrame = 0;
    uint32_t markers = 0;    // Reflex markers and sleeps
    uint32_t lastMarker = 0;
    uint32_t lastMarkerFrame = 0;
};

enum class TitleActivity : uint8_t { kConstants, kTag, kEvaluate, kMarker };

// Not thread-safe; the caller serialises it.
class PresentMarkerLedger {
public:
    void NoteTitlePresentStart(uint32_t frameIndex) {
        haveTitleFrame_ = true;
        lastFrame_ = frameIndex;
        markedSincePresent_ = true;
    }

    void NoteTitleActivity(TitleActivity kind, uint32_t frameIndex, uint32_t marker = 0) {
        switch (kind) {
        case TitleActivity::kConstants:
            ++current_.constants;
            current_.lastConstantsFrame = frameIndex;
            break;
        case TitleActivity::kTag:
            ++current_.tags;
            break;
        case TitleActivity::kEvaluate:
            ++current_.evaluates;
            current_.lastEvaluateFrame = frameIndex;
            break;
        case TitleActivity::kMarker:
            ++current_.markers;
            current_.lastMarker = marker;
            current_.lastMarkerFrame = frameIndex;
            break;
        }
    }

    // Every present 2.x's sl.common sees. True, with the frame to re-mark, when the title sent no
    // PRESENT_START since the previous counted present. Nothing is synthesized for a title that
    // never marks presents (2.x DLSS-G would not run for it at all) or while 2.x is not generating.
    bool PresentNeedsMarker(uint32_t presentFlags, bool dlssgEnabled, uint32_t* frameIndex) {
        if ((presentFlags & kDxgiPresentTest) != 0) {
            return false;
        }
        previousPresentActivity_ = lastPresentActivity_;
        lastPresentActivity_ = current_;
        current_ = {};
        const bool marked = markedSincePresent_;
        markedSincePresent_ = false;
        if (marked || !haveTitleFrame_ || !dlssgEnabled) {
            return false;
        }
        *frameIndex = lastFrame_;
        return true;
    }

    // Activity that led up to the present PresentNeedsMarker last counted, and to the one before.
    const TitlePresentActivity& LastPresentActivity() const { return lastPresentActivity_; }
    const TitlePresentActivity& PreviousPresentActivity() const { return previousPresentActivity_; }

private:
    bool haveTitleFrame_ = false;
    bool markedSincePresent_ = false;
    uint32_t lastFrame_ = 0;
    TitlePresentActivity current_;
    TitlePresentActivity lastPresentActivity_;
    TitlePresentActivity previousPresentActivity_;
};

// A constants call changes the 2.x state only for a viewport the title configured DLSS-G on;
// forwarding an eOff to a viewport DLSS-G never saw would invent a configuration.
constexpr bool DlssgGateNeedsSync(const DlssgViewportRequest& request, bool haveForwarded,
                                  const DlssgForwardedOptions& forwarded) {
    return request.haveGameRequest && (!haveForwarded || forwarded != DlssgOptionsFor(request));
}

}  // namespace ce::streamline_bridge
