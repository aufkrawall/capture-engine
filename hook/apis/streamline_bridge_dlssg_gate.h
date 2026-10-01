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

// A constants call changes the 2.x state only for a viewport the title configured DLSS-G on;
// forwarding an eOff to a viewport DLSS-G never saw would invent a configuration.
constexpr bool DlssgGateNeedsSync(const DlssgViewportRequest& request, bool haveForwarded,
                                  const DlssgForwardedOptions& forwarded) {
    return request.haveGameRequest && (!haveForwarded || forwarded != DlssgOptionsFor(request));
}

}  // namespace ce::streamline_bridge
