#pragma once

#include <windows.h>

#include <cstdint>

#include "streamline_bridge_dlssg_gate.h"

// The present-marker guard of the Streamline generation bridge (x64 only).
//
// 2.x DLSS-G requires a Reflex PRESENT_START marker before every present it counts and reads its
// tagged inputs at every present. The bridge hooks its own 2.x sl.common's present hook - the
// exact place 2.x counts a title present - and sl.dlss_g's. A title present without a marker is
// absorbed when it re-presents (kept from both hooks and from DXGI) and otherwise re-marks the
// title's last frame. The policy is PresentMarkerLedger in streamline_bridge_dlssg_gate.h.
namespace ce::streamline_bridge {

// Installs the guard on the bridge's 2.x sl.common.dll and sl.dlss_g.dll once `slInit` has loaded them.
bool InstallPresentMarkerGuard(HMODULE v2Common, HMODULE v2Dlssg);

// The title's own PRESENT_START marker for `frameIndex` reached 2.x.
void NoteTitlePresentStart(uint32_t frameIndex);

// The title's other per-frame calls, recorded so an unmarked present can be told apart from a
// re-present (see TitlePresentActivity). `marker` is the 1.x Reflex marker id for kMarker.
void NoteTitleActivity(TitleActivity kind, uint32_t frameIndex, uint32_t marker = 0);

// The title's 1.x Reflex sleep returned (timeline only; see streamline_bridge_present_timeline.h).
void NoteTitleSleepReturned();

// Sends PRESENT_START/PRESENT_END for a frame the title already presented (translate unit).
bool SynthesizePresentMarkersFor(uint32_t frameIndex);

}  // namespace ce::streamline_bridge
