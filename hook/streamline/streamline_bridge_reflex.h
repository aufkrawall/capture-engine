#pragma once

#include <cstdint>

// Reflex and latency markers for the Streamline generation bridge (x64 only; included by the
// translation unit inside its x64 block).
//
// Split out of streamline_bridge_translate.cpp. 1.x configures Reflex through
// slSetFeatureConstants, queries it through slGetFeatureSettings, and drives markers and sleep
// through slEvaluateFeature with the marker in `id`. 2.x has dedicated sl.reflex / sl.pcl entry
// points for each, and DLSS-G only generates frames once Reflex sees the title's PRESENT markers.
#include "sl.h"
#include "sl_reflex.h"

namespace ce::streamline_bridge {

// Resolves the sl.reflex / sl.pcl entry points. Retried until complete, because they exist only
// once the runtime has a device. Returns whether every Reflex entry point is resolved.
bool ResolveReflexFunctions(PFun_slGetFeatureFunction* getFeatureFunction);
void* ReflexSetOptionsEntry();
void* ReflexSleepEntry();

// Sends one Reflex mode and suppresses repeats. `synthesized` marks a mode CE chose for DLSS-G.
bool ForwardReflexMode(sl::ReflexMode mode, bool synthesized);

// slSetFeatureConstants(Reflex). While DLSS-G is on, Reflex is promoted to low latency + boost.
bool TranslateReflexConstants(const void* constants1x, bool dlssgEnabled);

// A DLSS-G state change: promote Reflex while it is on, and hand back the title's own last
// requested mode (off only if the title never asked for one) when it turns off.
bool UpdateReflexForDlssg(bool dlssgEnabled);

// slGetFeatureSettings(Reflex): answers the 1.x settings struct from the 2.x state.
bool TranslateReflexSettings(void* settings1x);

// slEvaluateFeature(Reflex): `id` is a 1.x marker. `token` is the token of the call's frame,
// or of the newest frame when the call carries none (sleep may pass frame 0).
bool TranslateReflexEvaluate(uint32_t id, uint32_t frameIndex, const sl::FrameToken* token);

// PRESENT_START + PRESENT_END for a frame the title already presented, ahead of a present it left
// unmarked (see PresentMarkerLedger).
bool SynthesizePresentMarkers(const sl::FrameToken& token);

// One synthesized sleep per frame while DLSS-G is on - only until the title is seen driving
// its own sleep, after which a second sleep per frame would halve its frame rate.
bool MaybeSynthesizeReflexSleep(uint32_t frameIndex, const sl::FrameToken* token, bool dlssgEnabled);

}  // namespace ce::streamline_bridge
