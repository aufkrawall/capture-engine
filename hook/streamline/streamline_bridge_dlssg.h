#pragma once

#include <cstdint>
#include <vector>

// DLSS-G options for the Streamline generation bridge (x64 only; included by the translation
// unit inside its x64 block).
//
// Split out of streamline_bridge_translate.cpp. The 2.x DLSS-G state of a viewport has two 1.x
// sources: slSetFeatureConstants(DLSS-G) carries the title's mode and frame count, and every
// slSetConstants carries `notRenderingGameFrames`, which 1.x used to skip interpolation and 2.x
// has no field for. The policy is in streamline_bridge_dlssg_gate.h.
#include "sl.h"
#include "sl_dlss_g.h"

namespace ce::streamline_bridge {

// slSetFeatureConstants(DLSS-G): records the title's request and forwards the effective state.
bool TranslateDlssgConstants(PFun_slDLSSGSetOptions* setOptions, uint32_t mode, uint32_t numFramesToGenerate,
                             uint32_t id);

// slSetConstants: records the frame's `notRenderingGameFrames` and resyncs a configured viewport.
void ApplyNotRenderingGameFrames(PFun_slDLSSGSetOptions* setOptions, uint32_t id, uint32_t frameIndex,
                                 uint8_t notRenderingGameFrames);

// Whether 2.x currently generates on any viewport (drives CE's synthesized Reflex sleep).
bool DlssgEnabledOnAnyViewport();

// Whether the TITLE wants DLSS-G on for the viewport, regardless of non-game suspension.
bool DlssgTitleRequestsOn(uint32_t id);

// The title's last DLSS-G input tag of one type on one viewport (see V1TagPersistsAcrossPresents).
struct PersistentTag {
    uint32_t viewport = 0;
    uint32_t bufferType = 0;
    sl::Resource resource{};
    bool haveExtent = false;
    sl::Extent extent{};
};

// slSetTag: remembers a present-time input tag, or forgets it when the title tags null.
void RememberPresentTag(uint32_t viewport, uint32_t bufferType, const sl::Resource* resource,
                        const sl::Extent* extent);

// After a present: the tags to re-issue (only while 2.x generates). `titleRetagged` reports whether
// the title tagged anything since the previous call - false means it presented again without tags.
bool TakePresentTagsForRefresh(std::vector<PersistentTag>& out, bool& titleRetagged);

}  // namespace ce::streamline_bridge
