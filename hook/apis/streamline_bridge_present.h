#pragma once

#include <windows.h>

#include <cstdint>

// The present-marker guard of the Streamline generation bridge (x64 only).
//
// 2.x DLSS-G requires a Reflex PRESENT_START marker before every present it counts; 1.x did not.
// The bridge hooks its own 2.x sl.common's present hook - the exact place 2.x counts a title
// present - and re-marks the title's last frame when a present arrives without a marker. The
// policy is PresentMarkerLedger in streamline_bridge_dlssg_gate.h.
namespace ce::streamline_bridge {

// Installs the guard on the bridge's 2.x sl.common.dll once `slInit` has loaded it.
bool InstallPresentMarkerGuard(HMODULE v2Common);

// The title's own PRESENT_START marker for `frameIndex` reached 2.x.
void NoteTitlePresentStart(uint32_t frameIndex);

// Sends PRESENT_START/PRESENT_END for a frame the title already presented (translate unit).
bool SynthesizePresentMarkersFor(uint32_t frameIndex);

}  // namespace ce::streamline_bridge
