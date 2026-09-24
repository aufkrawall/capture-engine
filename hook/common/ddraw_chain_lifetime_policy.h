#pragma once

#include <cstdint>

// When CE must stop referencing an application's DirectDraw presentation chain.
//
// DirectDraw allows one primary surface per DirectDraw object, and a primary is
// only gone once its last reference is. CE takes references into the chain on
// the application's behalf: the CPU-prerender queue keeps the surface it last
// presented (for a Flip, the primary itself), the native Direct3D 7 sidecar and
// the device tracker keep the application's device, and a Direct3D 7 device
// keeps its render target - the flip chain's back buffer. None of that is
// visible to the application, which releases everything it owns and asks for a
// new primary.
//
// Gothic II session 20260924_233030 is that sequence after loading a save: four
// `DDSCAPS_PRIMARYSURFACE | DDSCAPS_3DDEVICE` creations on the same DirectDraw
// object, all DDERR_PRIMARYSURFACEALREADYEXISTS, then the game's own
// `Error-Message` box and ExitProcess(0). CE's reset for a new chain ran only
// after a *successful* primary creation, which those references made
// impossible. The references therefore go before the creation is forwarded;
// every piece of CE state they backed is rebuilt from the next presentation.
namespace ce::ddraw_chain_lifetime {

// The application - not CE's own vtable bootstrap - is creating a primary.
inline bool ShouldReleaseChainBeforeCreation(bool primaryDescription, int bootstrapDepth, bool shuttingDown) {
    return primaryDescription && bootstrapDepth == 0 && !shuttingDown;
}

// DirectDraw HRESULTs are MAKE_DDHRESULT(code) = 0x88760000 | code. Spelled out
// here so the policy stays free of <ddraw.h>; the test pins them to the SDK.
inline constexpr uint32_t kPrimarySurfaceAlreadyExists = 0x88760234u;
inline constexpr uint32_t kUnsupportedMode = 0x8876024Eu;
inline constexpr uint32_t kNoExclusiveMode = 0x887600E1u;
inline constexpr uint32_t kExclusiveModeAlreadySet = 0x88760245u;
inline constexpr uint32_t kOutOfVideoMemory = 0x8876017Cu;
inline constexpr uint32_t kInvalidCaps = 0x88760064u;
inline constexpr uint32_t kInvalidParams = 0x80070057u;

// A readable name for the failures a primary creation actually reports, so a
// session log says what went wrong without an SDK lookup.
inline const char* DescribePrimaryCreationFailure(uint32_t hr) {
    switch (hr) {
        case kPrimarySurfaceAlreadyExists:
            return "DDERR_PRIMARYSURFACEALREADYEXISTS";
        case kUnsupportedMode:
            return "DDERR_UNSUPPORTEDMODE";
        case kNoExclusiveMode:
            return "DDERR_NOEXCLUSIVEMODE";
        case kExclusiveModeAlreadySet:
            return "DDERR_EXCLUSIVEMODEALREADYSET";
        case kOutOfVideoMemory:
            return "DDERR_OUTOFVIDEOMEMORY";
        case kInvalidCaps:
            return "DDERR_INVALIDCAPS";
        case kInvalidParams:
            return "DDERR_INVALIDPARAMS";
        default:
            return "unrecognized";
    }
}

}  // namespace ce::ddraw_chain_lifetime
